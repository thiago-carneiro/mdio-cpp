/**
 * @file nfs_metadata_bench.cpp
 * @brief M7 gate instrument: mdio-cpp per-open cost scales with the TOTAL
 *        chunk-file count in the dataset directory, not with the chunks
 *        actually read (NFS metadata path).
 *
 * Ported from the autonomous reproducer in formato-dados issue 06
 * (issues/repro/repro_nfs_file_count.cpp, pinned against fcbfb85); the
 * data formula is kept IDENTICAL so checksums are comparable across
 * versions and with the recorded issue evidence.
 *
 * Creates two datasets with IDENTICAL data (same shape, dtype, values,
 * coordinates) differing only in the seismic chunk grid:
 *   few  : chunks [48, 76, 64]  ->   128 chunk files
 *   many : chunks [ 1, 76, 64]  ->  6128 chunk files
 * then times, in a fresh process per read (mirroring one task per
 * process), an open + read of ONE inline (same 16 chunks touched in
 * both grids, same bytes returned; the few-grid read actually
 * decompresses MORE bytes per chunk, so a byte-cost model predicts
 * "few" as the slower arm).
 *
 * One read per process by design: the measured cost is per-open
 * metadata traffic, and in-process repeats would hit the very caches
 * this benchmark exists to gate (M7 deliverable 2). Multiple runs and
 * medians are driven by count_metadata_syscalls.py, which also counts
 * the metadata syscalls under strace.
 *
 * Usage:
 *   nfs_metadata_bench create <dataset_path> <few|many>
 *   nfs_metadata_bench read   <dataset_path> <inline_index>
 *
 * "read" prints: elapsed_ms=<ms> checksum=<sum> first=<v> last=<v>
 * (checksum/first/last must MATCH between the two datasets).
 *
 * Depends only on mdio (mdio/mdio.h), abseil, nlohmann/json.
 */

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "mdio/mdio.h"
#include "nlohmann/json.hpp"

namespace {

constexpr int64_t kInlineSize = 383;
constexpr int64_t kCrosslineSize = 304;
constexpr int64_t kTimeSize = 256;
constexpr int64_t kReadInline = 191;  // middle inline, away from edges

// Deterministic sample formula (integer arithmetic, stable across runs).
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
      {"name", "regular"},
      {"configuration", {{"chunkShape", chunk_shape}}}};
  return spec;
}

nlohmann::json BuildCreationSpec(const std::vector<int64_t>& seismic_chunk) {
  nlohmann::json spec;
  spec["metadata"] = {
      {"name", "nfs_metadata_bench"},
      {"apiVersion", "1.0.0"},
      {"createdOn", "2026-09-12T00:00:00+00:00"},
      {"attributes", {{"origin", "nfs_metadata_bench"}}}};
  spec["variables"] = nlohmann::json::array(
      {BuildVariableSpec(
           "seismic", "float32",
           {{"inline", kInlineSize},
            {"crossline", kCrosslineSize},
            {"time", kTimeSize}},
           seismic_chunk),
       BuildVariableSpec("inline", "uint16", {{"inline", kInlineSize}},
                         {kInlineSize}),
       BuildVariableSpec("crossline", "uint16",
                         {{"crossline", kCrosslineSize}}, {kCrosslineSize}),
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
  MDIO_ASSIGN_OR_RETURN(
      auto dataset,
      mdio::Dataset::from_json(creation_spec, path,
                               mdio::constants::kCreateClean)
          .result());
  MDIO_RETURN_IF_ERROR(FillAndWrite<mdio::dtypes::float32_t>(
      dataset, "seismic", [&](mdio::dtypes::float32_t* buffer) {
        size_t index = 0;
        for (int64_t i = 0; i < kInlineSize; ++i) {
          for (int64_t j = 0; j < kCrosslineSize; ++j) {
            for (int64_t k = 0; k < kTimeSize; ++k) {
              buffer[index++] =
                  static_cast<float>(SampleAt(i, j, k));
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

absl::Status RunRead(const std::string& path, int64_t inline_index) {
  const auto start = std::chrono::steady_clock::now();
  MDIO_ASSIGN_OR_RETURN(auto dataset, mdio::Dataset::Open(
                                          path, mdio::constants::kOpen)
                                          .result());
  MDIO_ASSIGN_OR_RETURN(
      auto variable,
      dataset.get_variable<mdio::dtypes::float32_t>("seismic"));
  MDIO_ASSIGN_OR_RETURN(
      auto sliced,
      variable.slice({{"inline", inline_index, inline_index + 1},
                      {"crossline", 0, kCrosslineSize},
                      {"time", 0, kTimeSize}}));
  MDIO_ASSIGN_OR_RETURN(auto data, sliced.Read().result());
  const auto elapsed = std::chrono::steady_clock::now() - start;
  const float* buffer = BufferPointer(data);
  const size_t count =
      static_cast<size_t>(kCrosslineSize) * kTimeSize;
  double checksum = 0;
  for (size_t i = 0; i < count; ++i) {
    checksum += buffer[i];
  }
  std::cout << "elapsed_ms="
            << std::chrono::duration<double, std::milli>(elapsed).count()
            << " checksum=" << checksum << " first=" << buffer[0]
            << " last=" << buffer[count - 1] << std::endl;
  return absl::OkStatus();
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 4 && std::string(argv[1]) == "create") {
    const absl::Status status = RunCreate(argv[2], argv[3]);
    if (!status.ok()) {
      std::cerr << "create failed: " << status << std::endl;
      return 1;
    }
    return 0;
  }
  if (argc == 4 && std::string(argv[1]) == "read") {
    const absl::Status status =
        RunRead(argv[2], std::stoll(argv[3]));
    if (!status.ok()) {
      std::cerr << "read failed: " << status << std::endl;
      return 1;
    }
    return 0;
  }
  std::cerr << "usage: " << argv[0]
            << " create <path> <few|many> | read <path> <inline>"
            << std::endl;
  return 2;
}
