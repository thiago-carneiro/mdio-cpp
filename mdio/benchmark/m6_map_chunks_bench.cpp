/**
 * @file m6_map_chunks_bench.cpp
 * @brief M6 gate instrument: serial per-chunk loop vs MapChunks
 *        (ParallelOptions arms) on the NFS backend.
 *
 * Dataset builder is kept IDENTICAL to nfs_metadata_bench.cpp (M7 harness)
 * so the created stores are byte-comparable and `create` can be driven by
 * either binary: same shape (383, 304, 256) float32, same deterministic
 * formula, differing only in the seismic chunk grid:
 *   few  : chunks [48, 76, 64]  ->  128 chunk files
 *   many : chunks [ 1, 76, 64]  ->  6128 chunk files
 *
 * Each timed run applies an in-place x2 gain over a region (per-chunk
 * read-modify-write), reports elapsed_ms and the post-gain checksum, then
 * restores the region with an exact /2 pass (x2 and /2 are lossless for the
 * integer-valued samples) and verifies the restored checksum. All arms must
 * print the SAME post-gain checksum — the byte-identical gate.
 *
 * Usage:
 *   m6_map_chunks_bench create <dataset_path> <few|many>
 *   m6_map_chunks_bench read <dataset_path> <arm> <region> [fusion] [workers]
 *
 *   arm    : slice-serial | map
 *   region : "all" | an inline index (one inline = 16 chunks in both grids)
 *   fusion : chunks_per_task for arm=map (default 1)
 *   workers: max_workers for arm=map (default 1)
 *
 * Output: elapsed_ms=<ms> checksum=<sum> first=<v> last=<v>
 * (same line format as nfs_metadata_bench so the M7 strace census pattern
 * applies verbatim).
 */

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "mdio/execution.h"
#include "mdio/mdio.h"
#include "nlohmann/json.hpp"

namespace {

constexpr int64_t kInlineSize = 383;
constexpr int64_t kCrosslineSize = 304;
constexpr int64_t kTimeSize = 256;

// Deterministic sample formula (integer arithmetic, stable across runs) —
// identical to the M7 harness so checksums are comparable.
uint32_t SampleAt(int64_t inline_index, int64_t crossline_index,
                  int64_t time_index) {
  return static_cast<uint32_t>(
      (inline_index * 31 + crossline_index * 17 + time_index * 7) % 1000);
}

nlohmann::json BuildVariableSpec(
    const std::string& name, const std::string& dtype,
    const std::vector<std::pair<std::string, int64_t>>& dimensions,
    const std::vector<int64_t>& chunk_shape) {
  nlohmann::json spec;
  spec["name"] = name;
  spec["dataType"] = dtype;
  nlohmann::json dims = nlohmann::json::array();
  nlohmann::json coords = nlohmann::json::array();
  for (const auto& [label, size] : dimensions) {
    dims.push_back({{"name", label}, {"size", size}});
    coords.push_back(label);
  }
  spec["dimensions"] = std::move(dims);
  spec["coordinates"] = std::move(coords);
  spec["metadata"]["chunkGrid"] = {
      {"name", "regular"}, {"configuration", {{"chunkShape", chunk_shape}}}};
  return spec;
}

nlohmann::json BuildCreationSpec(const std::vector<int64_t>& seismic_chunk) {
  nlohmann::json spec;
  spec["metadata"] = {{"name", "nfs_metadata_bench"},
                      {"apiVersion", "1.0.0"},
                      {"createdOn", "2026-09-12T00:00:00+00:00"},
                      {"attributes", {{"origin", "nfs_metadata_bench"}}}};
  spec["variables"] = nlohmann::json::array(
      {BuildVariableSpec("seismic", "float32",
                         {{"inline", kInlineSize},
                          {"crossline", kCrosslineSize},
                          {"time", kTimeSize}},
                         seismic_chunk),
       BuildVariableSpec("inline", "uint16", {{"inline", kInlineSize}},
                         {kInlineSize}),
       BuildVariableSpec("crossline", "uint16", {{"crossline", kCrosslineSize}},
                         {kCrosslineSize}),
       BuildVariableSpec("time", "uint16", {{"time", kTimeSize}},
                         {kTimeSize})});
  return spec;
}

// Typed pointer to the first element of a VariableData buffer (mdio's own
// accessor API; the 0th allocation element may precede the sliced data).
template <typename T, mdio::DimensionIndex R, mdio::ArrayOriginKind K>
T* BufferPointer(mdio::VariableData<T, R, K>& variable_data) {
  auto accessor = variable_data.get_data_accessor();
  char* base = reinterpret_cast<char*>(accessor.data()) +
               variable_data.get_flattened_offset() * sizeof(T);
  return reinterpret_cast<T*>(base);
}

template <typename T, typename Fill>
absl::Status FillAndWrite(mdio::Dataset& dataset, const std::string& name,
                          Fill fill) {
  MDIO_ASSIGN_OR_RETURN(auto variable, dataset.get_variable<T>(name));
  MDIO_ASSIGN_OR_RETURN(auto data, mdio::from_variable<T>(variable));
  T* buffer = BufferPointer(data);
  fill(buffer);
  mdio::WriteFutures futures = variable.Write(data);
  return futures.status();
}

absl::Status RunCreate(const std::string& path, const std::string& variant) {
  std::vector<int64_t> seismic_chunk;
  if (variant == "few") {
    seismic_chunk = {48, 76, 64};
  } else if (variant == "many") {
    seismic_chunk = {1, 76, 64};
  } else {
    return absl::InvalidArgumentError("variant must be 'few' or 'many'");
  }
  const auto start = std::chrono::steady_clock::now();
  nlohmann::json creation_spec = BuildCreationSpec(seismic_chunk);
  MDIO_ASSIGN_OR_RETURN(auto dataset,
                        mdio::Dataset::from_json(creation_spec, path,
                                                 mdio::constants::kCreateClean)
                            .result());
  MDIO_RETURN_IF_ERROR(FillAndWrite<mdio::dtypes::float32_t>(
      dataset, "seismic", [&](mdio::dtypes::float32_t* buffer) {
        size_t index = 0;
        for (int64_t i = 0; i < kInlineSize; ++i) {
          for (int64_t j = 0; j < kCrosslineSize; ++j) {
            for (int64_t k = 0; k < kTimeSize; ++k) {
              buffer[index++] = static_cast<float>(SampleAt(i, j, k));
            }
          }
        }
      }));
  MDIO_RETURN_IF_ERROR(FillAndWrite<mdio::dtypes::uint16_t>(
      dataset, "inline", [&](mdio::dtypes::uint16_t* buffer) {
        for (int64_t i = 0; i < kInlineSize; ++i) {
          buffer[i] = static_cast<uint16_t>(1000 + i);
        }
      }));
  MDIO_RETURN_IF_ERROR(FillAndWrite<mdio::dtypes::uint16_t>(
      dataset, "crossline", [&](mdio::dtypes::uint16_t* buffer) {
        for (int64_t j = 0; j < kCrosslineSize; ++j) {
          buffer[j] = static_cast<uint16_t>(1000 + j);
        }
      }));
  MDIO_RETURN_IF_ERROR(FillAndWrite<mdio::dtypes::uint16_t>(
      dataset, "time", [&](mdio::dtypes::uint16_t* buffer) {
        for (int64_t k = 0; k < kTimeSize; ++k) {
          buffer[k] = static_cast<uint16_t>(k);
        }
      }));
  const auto elapsed = std::chrono::steady_clock::now() - start;
  std::cout << "create " << variant << " ok in "
            << std::chrono::duration<double, std::milli>(elapsed).count()
            << " ms" << std::endl;
  return absl::OkStatus();
}

/// The variable under measurement and, for an inline region, its slice.
mdio::Result<mdio::Variable<>> OpenRegion(mdio::Dataset& dataset,
                                          const std::string& region) {
  MDIO_ASSIGN_OR_RETURN(
      auto variable, dataset.get_variable<mdio::dtypes::float32_t>("seismic"));
  if (region == "all") {
    return mdio::Variable<>(variable);
  }
  const int64_t inline_index = std::stoll(region);
  MDIO_ASSIGN_OR_RETURN(
      auto sliced, variable.slice({{"inline", inline_index, inline_index + 1},
                                   {"crossline", 0, kCrosslineSize},
                                   {"time", 0, kTimeSize}}));
  return mdio::Variable<>(sliced);
}

double ChecksumOf(const mdio::Variable<>& variable) {
  auto read_result = tensorstore::Read(variable.get_store()).result();
  if (!read_result.ok()) {
    std::cerr << "checksum read failed: " << read_result.status() << std::endl;
    std::exit(1);  // NOLINT — a bench harness may abort on internal error.
  }
  const auto& array = read_result.value();
  const float* values =
      static_cast<const float*>(array.byte_strided_origin_pointer().get());
  double checksum = 0;
  for (mdio::Index i = 0; i < array.num_elements(); ++i) {
    checksum += values[i];
  }
  return checksum;
}

/// The serial per-chunk baseline: per-chunk box read -> gain -> write, over
/// the variable's own chunk grid. This is today's downstream per-chunk
/// pattern (chunks() + restricted tensorstore I/O).
absl::Status GainPerChunkSerial(const mdio::Variable<>& variable,
                                const float factor) {
  MDIO_ASSIGN_OR_RETURN(auto chunks, variable.chunks());
  for (const auto& box : chunks) {
    auto chunk_store =
        variable.get_store() | tensorstore::AllDims().TranslateBoxSlice(box);
    if (!chunk_store.ok()) {
      return chunk_store.status();
    }
    auto read_result = tensorstore::Read(chunk_store.value()).result();
    if (!read_result.ok()) {
      return read_result.status();
    }
    auto array = std::move(read_result).value();
    float* values =
        static_cast<float*>(array.byte_strided_origin_pointer().get());
    for (mdio::Index i = 0; i < array.num_elements(); ++i) {
      values[i] *= factor;
    }
    auto commit =
        tensorstore::Write(array, chunk_store.value()).commit_future.result();
    if (!commit.ok()) {
      return commit.status();
    }
  }
  return absl::OkStatus();
}

absl::Status GainWithMapChunks(const mdio::Variable<>& variable,
                               const float factor, std::size_t fusion,
                               std::size_t workers) {
  mdio::ParallelOptions options;
  options.max_workers = workers;
  options.chunks_per_task = fusion;
  options.write_back = true;
  auto result = mdio::MapChunks(
      variable,
      [factor](const mdio::ChunkView& view) -> absl::Status {
        float* values =
            static_cast<float*>(view.data.byte_strided_origin_pointer().get());
        for (mdio::Index i = 0; i < view.data.num_elements(); ++i) {
          values[i] *= factor;
        }
        return absl::OkStatus();
      },
      options);
  auto statuses = result.result();
  if (!statuses.ok()) {
    return statuses.status();
  }
  for (const absl::Status& status : statuses.value()) {
    if (!status.ok()) {
      return status;
    }
  }
  return absl::OkStatus();
}

absl::Status RunRead(const std::string& path, const std::string& arm,
                     const std::string& region, std::size_t fusion,
                     std::size_t workers) {
  MDIO_ASSIGN_OR_RETURN(
      auto dataset, mdio::Dataset::Open(path, mdio::constants::kOpen).result());
  MDIO_ASSIGN_OR_RETURN(auto variable, OpenRegion(dataset, region));
  const double initial = ChecksumOf(variable);

  const auto start = std::chrono::steady_clock::now();
  absl::Status gained;
  if (arm == "slice-serial") {
    gained = GainPerChunkSerial(variable, 2.0F);
  } else if (arm == "map") {
    gained = GainWithMapChunks(variable, 2.0F, fusion, workers);
  } else {
    return absl::InvalidArgumentError("arm must be 'slice-serial' or 'map'");
  }
  if (!gained.ok()) {
    return gained;
  }
  const auto elapsed = std::chrono::steady_clock::now() - start;
  const double checksum = ChecksumOf(variable);

  // Restore with the exact inverse pass, then verify.
  if (arm == "slice-serial") {
    MDIO_RETURN_IF_ERROR(GainPerChunkSerial(variable, 0.5F));
  } else {
    MDIO_RETURN_IF_ERROR(GainWithMapChunks(variable, 0.5F, fusion, workers));
  }
  const double restored = ChecksumOf(variable);
  if (restored != initial) {
    return absl::InternalError(
        absl::StrCat("checksum drift after restore: initial=", initial,
                     " restored=", restored));
  }

  std::cout << "elapsed_ms="
            << std::chrono::duration<double, std::milli>(elapsed).count()
            << " checksum=" << checksum << " restored=" << restored
            << std::endl;
  return absl::OkStatus();
}

}  // namespace

int main(int argc, char** argv) {
  const std::vector<std::string> args(argv + 1, argv + argc);
  if (args.size() == 3 && args[0] == "create") {
    const absl::Status status = RunCreate(args[1], args[2]);
    if (!status.ok()) {
      std::cerr << "create failed: " << status << std::endl;
      return 1;
    }
    return 0;
  }
  if (args.size() >= 4 && args[0] == "read") {
    const std::size_t fusion =
        args.size() > 4 ? std::stoull(args[4]) : std::size_t{1};
    const std::size_t workers =
        args.size() > 5 ? std::stoull(args[5]) : std::size_t{1};
    const absl::Status status =
        RunRead(args[1], args[2], args[3], fusion, workers);
    if (!status.ok()) {
      std::cerr << "read failed: " << status << std::endl;
      return 1;
    }
    return 0;
  }
  std::cerr << "usage: " << argv[0]
            << " create <path> <few|many> | read <path> <arm> <region> "
               "[fusion] [workers]"
            << std::endl;
  return 2;
}
