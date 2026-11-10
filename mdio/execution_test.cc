// Copyright 2026 TGS

// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.

//    http://www.apache.org/licenses/LICENSE-2.0

// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "mdio/execution.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "mdio/variable.h"
#include "tensorstore/tensorstore.h"
#include "tensorstore/util/status_testutil.h"

// clang-format off
#include <nlohmann/json.hpp>  // NOLINT
// clang-format on

namespace {

// Test stores live under a per-test directory inside gtest's temp root
// (TEST_TMPDIR/TMPDIR, else /tmp) so a run never litters its working
// directory and parallel runs never collide on fixed store names. Same
// pattern as transform_test.cc and the other suites.
std::string TestStorePath(const std::string& relative) {
  std::string path = ::testing::TempDir();
  const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
  if (info != nullptr) {
    path += std::string(info->test_suite_name()) + "." + info->name() + "/";
  }
  return path + relative;
}

/// Builds a Zarr V3 variable spec. `chunks` defaults to `shape`; Zarr V3
/// requires chunk entries >= 1, so zero-length dimensions must pass an
/// explicit unit chunk.
::nlohmann::json MakeVariableSpec(const std::string& name,
                                  const std::string& dataType,
                                  const std::vector<mdio::Index>& shape,
                                  const std::vector<mdio::Index>& chunks = {}) {
  ::nlohmann::json spec;
  spec["driver"] = "zarr3";
  spec["kvstore"]["driver"] = "file";
  spec["kvstore"]["path"] = TestStorePath(name);
  spec["metadata"]["data_type"] = dataType;
  spec["metadata"]["shape"] = shape;
  spec["metadata"]["chunk_grid"]["name"] = "regular";
  spec["metadata"]["chunk_grid"]["configuration"]["chunk_shape"] =
      chunks.empty() ? shape : chunks;
  spec["metadata"]["chunk_key_encoding"]["name"] = "default";
  spec["metadata"]["chunk_key_encoding"]["configuration"]["separator"] = "/";
  ::nlohmann::json bytesCodec;
  bytesCodec["name"] = "bytes";
  spec["metadata"]["codecs"] = ::nlohmann::json::array({bytesCodec});
  std::vector<std::string> dimensionNames;
  for (std::size_t index = 0; index < shape.size(); ++index) {
    dimensionNames.push_back("dim_" + std::to_string(index));
  }
  spec["attributes"]["dimension_names"] = dimensionNames;
  spec["attributes"]["long_name"] = "execution test variable";
  return spec;
}

/// Builds a Zarr V2 struct spec: fields a (int16) and b (int32), packed to a
/// 6-byte record.
::nlohmann::json MakeStructSpecV2(
    const std::string& name, const std::vector<mdio::Index>& shape = {4, 6},
    const std::vector<mdio::Index>& chunks = {2, 3}) {
  ::nlohmann::json spec;
  spec["driver"] = "zarr";
  spec["kvstore"]["driver"] = "file";
  spec["kvstore"]["path"] = TestStorePath(name);
  spec["metadata"]["compressor"] = {{"id", "blosc"}};
  spec["metadata"]["dtype"] = ::nlohmann::json::array({
      ::nlohmann::json::array({"a", "<i2"}),
      ::nlohmann::json::array({"b", "<i4"}),
  });
  spec["metadata"]["shape"] = shape;
  spec["metadata"]["chunks"] = chunks;
  spec["metadata"]["dimension_separator"] = "/";
  spec["metadata"]["zarr_format"] = 2;
  spec["attributes"]["dimension_names"] = {"x", "y"};
  spec["attributes"]["long_name"] = "execution test struct";
  return spec;
}

/// Creates a Zarr V3 variable at `name` and fills it with `values` in
/// row-major order. `values.size()` must equal the product of `shape`; an
/// empty `values` only creates the store.
template <typename T>
mdio::Result<mdio::Variable<>> MakePopulatedVariable(
    const std::string& name, const std::string& dataType,
    const std::vector<mdio::Index>& shape, const std::vector<T>& values,
    const std::vector<mdio::Index>& chunks = {}) {
  MDIO_ASSIGN_OR_RETURN(
      auto variable,
      mdio::Variable<>::Open(MakeVariableSpec(name, dataType, shape, chunks),
                             mdio::constants::kCreateClean)
          .result());
  if (values.empty()) {
    return variable;
  }
  auto data = tensorstore::AllocateArray<T>(absl::MakeConstSpan(shape));
  T* flat = data.data();
  for (std::size_t index = 0; index < values.size(); ++index) {
    flat[index] = values[index];
  }
  auto writeFutures = tensorstore::Write(std::move(data), variable.get_store());
  writeFutures.commit_future.Wait();
  MDIO_RETURN_IF_ERROR(writeFutures.copy_future.status());
  return variable;
}

/// Convenience: sequential values [first, first + count) as floats.
std::vector<float> SequentialValues(const float first,
                                    const std::size_t count) {
  std::vector<float> values;
  values.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    values.push_back(first + static_cast<float>(index));
  }
  return values;
}

/// Reads a whole float32 variable as flat row-major values.
std::vector<float> ReadFloat32Values(const mdio::Variable<>& variable) {
  auto readResult = tensorstore::Read(variable.get_store()).result();
  EXPECT_TRUE(readResult.ok()) << readResult.status();
  if (!readResult.ok()) {
    return {};
  }
  const auto& array = readResult.value();
  const float* flat =
      static_cast<const float*>(array.byte_strided_origin_pointer().get());
  return std::vector<float>(flat, flat + array.num_elements());
}

/// The chunk view's data buffer is mutable storage viewed as bytes; this is
/// the documented way for a callback to rewrite the chunk in place.
void* MutableChunkBytes(const mdio::ChunkView& view) {
  return view.data.byte_strided_origin_pointer().get();
}

/// In-place gain over every float32 element of the chunk.
mdio::ChunkFunction MakeFloat32Gain(const float gain) {
  return [gain](const mdio::ChunkView& view) -> absl::Status {
    float* values = static_cast<float*>(MutableChunkBytes(view));
    const std::size_t count =
        static_cast<std::size_t>(view.data.num_elements());
    for (std::size_t index = 0; index < count; ++index) {
      values[index] *= gain;
    }
    return absl::OkStatus();
  };
}

/// The serial per-chunk baseline: for each chunk box, one restricted read,
/// one in-place gain, one restricted write. This is the pattern a downstream
/// program writes with chunks() + tensorstore today.
absl::Status SerialGainPerChunk(const mdio::Variable<>& variable,
                                const float gain) {
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
    const std::size_t count = static_cast<std::size_t>(array.num_elements());
    for (std::size_t index = 0; index < count; ++index) {
      values[index] *= gain;
    }
    auto commit =
        tensorstore::Write(array, chunk_store.value()).commit_future.result();
    if (!commit.ok()) {
      return commit.status();
    }
  }
  return absl::OkStatus();
}

/// The (index, origin, shape) triple the callback saw, for fusion invariant
/// assertions.
struct ChunkRecord {
  std::size_t index = 0;
  std::vector<mdio::Index> origin;
  std::vector<mdio::Index> shape;
  std::size_t element_size = 0;
};

/// A callback that gains every float32 element in place, records every view
/// into a preallocated per-chunk slot (disjoint writes: race-free without
/// locks under any worker count) and counts invocations through
/// `call_count`. `fail_index`, when set, makes that chunk fail with an
/// error — after the gain, before any write-back.
mdio::ChunkFunction MakeGainingRecordingFn(
    std::vector<ChunkRecord>* records,
    std::shared_ptr<std::atomic<std::size_t>> call_count, const float gain,
    std::size_t fail_index = SIZE_MAX) {
  return [records, call_count, gain,
          fail_index](const mdio::ChunkView& view) -> absl::Status {
    if (call_count) {
      call_count->fetch_add(1, std::memory_order_relaxed);
    }
    (*records)[view.index] =
        ChunkRecord{view.index,
                    std::vector<mdio::Index>(view.domain.origin().begin(),
                                             view.domain.origin().end()),
                    std::vector<mdio::Index>(view.domain.shape().begin(),
                                             view.domain.shape().end()),
                    view.element_size};
    float* values = static_cast<float*>(MutableChunkBytes(view));
    const std::size_t count =
        static_cast<std::size_t>(view.data.num_elements());
    for (std::size_t index = 0; index < count; ++index) {
      values[index] *= gain;
    }
    if (view.index == fail_index) {
      return absl::InvalidArgumentError("boom");
    }
    return absl::OkStatus();
  };
}

/// The chunk boxes of a variable, in the row-major order chunks() yields.
mdio::Result<std::vector<tensorstore::Box<>>> ChunkBoxes(
    const mdio::Variable<>& variable) {
  MDIO_ASSIGN_OR_RETURN(auto chunks, variable.chunks());
  std::vector<tensorstore::Box<>> boxes;
  boxes.reserve(chunks.size());
  for (const auto& box : chunks) {
    boxes.push_back(box);
  }
  return boxes;
}

TEST(MapChunksTest, SerialWriteBackIsByteIdenticalToPlainChunkLoop) {
  const std::vector<float> values = SequentialValues(1.0F, 24);
  auto variable = MakePopulatedVariable<float>("map_serial_equiv", "float32",
                                               {4, 6}, values, {2, 2});
  ASSERT_TRUE(variable.ok()) << variable.status();

  mdio::ParallelOptions options;
  options.max_workers = 1;
  options.write_back = true;
  auto result =
      mdio::MapChunks(variable.value(), MakeFloat32Gain(2.0F), options)
          .result();
  ASSERT_TRUE(result.ok()) << result.status();
  for (const absl::Status& status : result.value()) {
    ASSERT_TRUE(status.ok()) << status;
  }

  auto baseline = MakePopulatedVariable<float>(
      "map_serial_equiv_baseline", "float32", {4, 6}, values, {2, 2});
  ASSERT_TRUE(baseline.ok()) << baseline.status();
  ASSERT_TRUE(SerialGainPerChunk(baseline.value(), 2.0F).ok());

  const std::vector<float> mapped = ReadFloat32Values(variable.value());
  const std::vector<float> plain = ReadFloat32Values(baseline.value());
  ASSERT_EQ(values.size(), mapped.size());
  ASSERT_EQ(values.size(), plain.size());
  for (std::size_t index = 0; index < values.size(); ++index) {
    // All values are exactly representable and the gain is exact in
    // float32, so equality here IS the byte-identity gate.
    EXPECT_EQ(values[index] * 2.0F, mapped[index]) << "index " << index;
    EXPECT_EQ(plain[index], mapped[index]) << "index " << index;
  }
}

TEST(MapChunksTest, ParallelWriteBackMatchesSerial) {
  const std::vector<float> values = SequentialValues(1.0F, 48);
  auto serial = MakePopulatedVariable<float>("map_par_serial", "float32",
                                             {6, 8}, values, {2, 2});
  ASSERT_TRUE(serial.ok()) << serial.status();
  auto parallel = MakePopulatedVariable<float>("map_par_parallel", "float32",
                                               {6, 8}, values, {2, 2});
  ASSERT_TRUE(parallel.ok()) << parallel.status();

  mdio::ParallelOptions serial_options;
  serial_options.max_workers = 1;
  serial_options.write_back = true;
  auto serial_result =
      mdio::MapChunks(serial.value(), MakeFloat32Gain(3.0F), serial_options)
          .result();
  ASSERT_TRUE(serial_result.ok()) << serial_result.status();
  for (const absl::Status& status : serial_result.value()) {
    ASSERT_TRUE(status.ok()) << status;
  }

  mdio::ParallelOptions parallel_options;
  parallel_options.max_workers = 4;
  parallel_options.max_in_flight = 2;
  parallel_options.chunks_per_task = 2;
  parallel_options.write_back = true;
  auto parallel_result =
      mdio::MapChunks(parallel.value(), MakeFloat32Gain(3.0F), parallel_options)
          .result();
  ASSERT_TRUE(parallel_result.ok()) << parallel_result.status();
  for (const absl::Status& status : parallel_result.value()) {
    ASSERT_TRUE(status.ok()) << status;
  }

  const std::vector<float> got_serial = ReadFloat32Values(serial.value());
  const std::vector<float> got_parallel = ReadFloat32Values(parallel.value());
  ASSERT_EQ(got_serial.size(), got_parallel.size());
  for (std::size_t index = 0; index < got_serial.size(); ++index) {
    EXPECT_EQ(got_serial[index], got_parallel[index]) << "index " << index;
  }
}

TEST(MapChunksTest, FusionInvokesOncePerChunkWithGridIdentity) {
  const std::vector<float> values = SequentialValues(0.0F, 12);
  auto variable = MakePopulatedVariable<float>("map_fusion", "float32", {3, 4},
                                               values, {2, 2});
  ASSERT_TRUE(variable.ok()) << variable.status();
  auto expected = ChunkBoxes(variable.value());
  ASSERT_TRUE(expected.ok()) << expected.status();

  mdio::ParallelOptions options;
  options.max_workers = 1;
  options.chunks_per_task = 2;
  std::vector<ChunkRecord> records(expected.value().size());
  auto call_count = std::make_shared<std::atomic<std::size_t>>(0);
  auto result = mdio::MapChunks(
                    variable.value(),
                    MakeGainingRecordingFn(&records, call_count, 2.0F), options)
                    .result();
  ASSERT_TRUE(result.ok()) << result.status();
  EXPECT_EQ(expected.value().size(), call_count->load());
  ASSERT_EQ(expected.value().size(), records.size());
  for (std::size_t index = 0; index < records.size(); ++index) {
    EXPECT_EQ(index, records[index].index) << "chunk " << index;
    for (std::size_t dim = 0; dim < records[index].origin.size(); ++dim) {
      EXPECT_EQ(expected.value()[index].origin()[dim],
                records[index].origin[dim])
          << "chunk " << index << " dim " << dim;
      EXPECT_EQ(expected.value()[index].shape()[dim], records[index].shape[dim])
          << "chunk " << index << " dim " << dim;
    }
    EXPECT_EQ(sizeof(float), records[index].element_size) << "chunk " << index;
  }
}

TEST(MapChunksTest, FusionNeverSpansGridRows) {
  // Grid 2x3 over shape {3,5}, chunks {2,2}: row 0 is chunks 0-2 (the last
  // one partial along the columns), row 1 is chunks 3-5 (partial rows).
  // chunks_per_task = 4 exceeds the row length, so each row is one task and
  // no task mixes rows.
  const std::vector<float> values = SequentialValues(0.0F, 15);
  auto variable = MakePopulatedVariable<float>("map_fusion_rows", "float32",
                                               {3, 5}, values, {2, 2});
  ASSERT_TRUE(variable.ok()) << variable.status();
  auto expected = ChunkBoxes(variable.value());
  ASSERT_TRUE(expected.ok()) << expected.status();
  ASSERT_EQ(6u, expected.value().size());

  mdio::ParallelOptions options;
  options.max_workers = 1;
  options.chunks_per_task = 4;
  std::vector<ChunkRecord> records(expected.value().size());
  auto call_count = std::make_shared<std::atomic<std::size_t>>(0);
  auto result = mdio::MapChunks(
                    variable.value(),
                    MakeGainingRecordingFn(&records, call_count, 2.0F), options)
                    .result();
  ASSERT_TRUE(result.ok()) << result.status();
  EXPECT_EQ(expected.value().size(), call_count->load());
  for (std::size_t index = 0; index < records.size(); ++index) {
    EXPECT_EQ(index, records[index].index) << "chunk " << index;
    for (std::size_t dim = 0; dim < records[index].origin.size(); ++dim) {
      EXPECT_EQ(expected.value()[index].origin()[dim],
                records[index].origin[dim])
          << "chunk " << index << " dim " << dim;
      EXPECT_EQ(expected.value()[index].shape()[dim], records[index].shape[dim])
          << "chunk " << index << " dim " << dim;
    }
  }
}

TEST(MapChunksTest, ParallelCallbackRunsOncePerChunkWithoutLocks) {
  // Race exercise: every callback writes only its own slot (disjoint memory),
  // so TSan-clean execution under 8 workers is itself the assertion; the
  // post-condition (every slot filled exactly once with the right index) is
  // checked afterwards.
  const std::vector<float> values = SequentialValues(0.0F, 30);
  auto variable = MakePopulatedVariable<float>("map_race", "float32", {5, 6},
                                               values, {2, 2});
  ASSERT_TRUE(variable.ok()) << variable.status();
  auto expected = ChunkBoxes(variable.value());
  ASSERT_TRUE(expected.ok()) << expected.status();

  mdio::ParallelOptions options;
  options.max_workers = 8;
  options.chunks_per_task = 1;
  std::vector<ChunkRecord> records(expected.value().size());
  auto call_count = std::make_shared<std::atomic<std::size_t>>(0);
  auto result = mdio::MapChunks(
                    variable.value(),
                    MakeGainingRecordingFn(&records, call_count, 2.0F), options)
                    .result();
  ASSERT_TRUE(result.ok()) << result.status();
  EXPECT_EQ(expected.value().size(), call_count->load());
  for (std::size_t index = 0; index < records.size(); ++index) {
    EXPECT_EQ(index, records[index].index) << "chunk " << index;
    for (std::size_t dim = 0; dim < records[index].origin.size(); ++dim) {
      EXPECT_EQ(expected.value()[index].origin()[dim],
                records[index].origin[dim])
          << "chunk " << index << " dim " << dim;
    }
  }
}

TEST(MapChunksTest, FailureSkipsWriteForFailedChunkOnly) {
  // {3,4} float32, chunks {2,2} -> 4 chunks; chunk 2 fails. With fusion
  // (chunks_per_task = 2) chunk 2 shares a task with chunk 3: chunk 3 must
  // still be written, chunk 2 must keep its original bytes.
  const std::vector<float> values = SequentialValues(1.0F, 12);
  for (const std::size_t chunks_per_task : {std::size_t{1}, std::size_t{2}}) {
    auto variable = MakePopulatedVariable<float>(
        "map_fail_k" + std::to_string(chunks_per_task), "float32", {3, 4},
        values, {2, 2});
    ASSERT_TRUE(variable.ok()) << variable.status();

    mdio::ParallelOptions options;
    options.max_workers = 4;
    options.chunks_per_task = chunks_per_task;
    options.write_back = true;
    std::vector<ChunkRecord> records(4);
    auto call_count = std::make_shared<std::atomic<std::size_t>>(0);
    auto result =
        mdio::MapChunks(variable.value(),
                        MakeGainingRecordingFn(&records, call_count, 2.0F,
                                               /*fail_index=*/2),
                        options)
            .result();
    ASSERT_TRUE(result.ok()) << result.status();
    EXPECT_EQ(4u, call_count->load());
    ASSERT_EQ(4u, result.value().size());
    EXPECT_TRUE(result.value()[0].ok());
    EXPECT_TRUE(result.value()[1].ok());
    EXPECT_FALSE(result.value()[2].ok());
    EXPECT_TRUE(result.value()[3].ok());

    // Expected: every element gained x2 EXCEPT chunk 2 (rows 2..3 are chunk
    // row 1, cols 0..1 are chunk col 0 -> chunk index 1*2+0 = 2).
    const std::vector<float> got = ReadFloat32Values(variable.value());
    ASSERT_EQ(values.size(), got.size());
    for (std::size_t row = 0; row < 3; ++row) {
      for (std::size_t col = 0; col < 4; ++col) {
        const std::size_t flat = row * 4 + col;
        const std::size_t chunk = (row / 2) * 2 + (col / 2);
        const float expected_value =
            chunk == 2 ? values[flat] : values[flat] * 2.0F;
        EXPECT_EQ(expected_value, got[flat])
            << "row " << row << " col " << col << " chunk " << chunk;
      }
    }
  }
}

TEST(MapChunksTest, WriteBackFollowsVariableWriteSemantics) {
  // Measured (2026-11, this repo): mdio's dynamic-mode Variable<> conversion
  // drops the read-only tag of a store opened with ReadWriteMode::read
  // (get_store().read_write_mode() reports read_write), so MapChunks cannot
  // fail fast on it — and its write_back path has exactly the constraint
  // profile of Variable::Write on the same variable. This test pins that
  // contract: the write succeeds on the read-opened handle and the bytes
  // match the expected gain.
  const std::vector<float> values = SequentialValues(1.0F, 12);
  auto created = MakePopulatedVariable<float>("map_readonly", "float32", {3, 4},
                                              values, {2, 2});
  ASSERT_TRUE(created.ok()) << created.status();
  auto read_only =
      mdio::Variable<>::Open(
          MakeVariableSpec("map_readonly", "float32", {3, 4}, {2, 2}),
          tensorstore::ReadWriteMode::read)
          .result();
  ASSERT_TRUE(read_only.ok()) << read_only.status();

  mdio::ParallelOptions options;
  options.max_workers = 2;
  options.write_back = true;
  auto result =
      mdio::MapChunks(read_only.value(), MakeFloat32Gain(2.0F), options)
          .result();
  ASSERT_TRUE(result.ok()) << result.status();
  for (const absl::Status& status : result.value()) {
    ASSERT_TRUE(status.ok()) << status;
  }
  const std::vector<float> got = ReadFloat32Values(read_only.value());
  ASSERT_EQ(values.size(), got.size());
  for (std::size_t index = 0; index < values.size(); ++index) {
    EXPECT_EQ(values[index] * 2.0F, got[index]) << "index " << index;
  }
}

TEST(MapChunksTest, ValidationErrorsReturnErroredFutures) {
  auto variable = MakePopulatedVariable<float>(
      "map_validation", "float32", {2, 2}, {1.0F, 2.0F, 3.0F, 4.0F}, {1, 1});
  ASSERT_TRUE(variable.ok()) << variable.status();

  auto empty_fn_result = mdio::MapChunks(variable.value(), nullptr).result();
  EXPECT_FALSE(empty_fn_result.ok());

  mdio::ParallelOptions options;
  options.chunks_per_task = 0;
  auto zero_fusion_result =
      mdio::MapChunks(variable.value(), MakeFloat32Gain(1.0F), options)
          .result();
  EXPECT_FALSE(zero_fusion_result.ok());
}

TEST(MapChunksTest, StructuredVoidByteViewFailsFastOnChunksRank) {
  // The void byte view of a structured dtype is rank N+1 while the stored
  // chunk shape has rank N — the same pre-existing chunks() limitation the
  // M5 notes documented — so MapChunks fails fast with that error instead of
  // running.
  auto created = mdio::Variable<>::Open(MakeStructSpecV2("map_struct_void"),
                                        mdio::constants::kCreateClean)
                     .result();
  ASSERT_TRUE(created.ok()) << created.status();

  auto result = mdio::MapChunks(created.value(), [](const mdio::ChunkView&) {
                  return absl::OkStatus();
                }).result();
  EXPECT_FALSE(result.ok());
  EXPECT_EQ(absl::StatusCode::kInvalidArgument, result.status().code());
}

TEST(MapChunksTest, StructuredFieldViewElementSizeIsTheItem) {
  // A typed field view is rank N and chunks() works on it: the element unit
  // is the field's dtype itemsize.
  ::nlohmann::json spec = MakeStructSpecV2("map_struct_field");
  spec["field"] = "a";
  auto variable = mdio::Variable<mdio::dtypes::int16_t>::Open(
                      spec, mdio::constants::kCreateClean)
                      .result();
  ASSERT_TRUE(variable.ok()) << variable.status();
  auto expected = ChunkBoxes(variable.value());
  ASSERT_TRUE(expected.ok()) << expected.status();

  mdio::ParallelOptions options;
  options.max_workers = 1;
  options.chunks_per_task = 2;
  std::vector<ChunkRecord> records(expected.value().size());
  auto result =
      mdio::MapChunks(variable.value(),
                      MakeGainingRecordingFn(&records, nullptr, 1.0F), options)
          .result();
  ASSERT_TRUE(result.ok()) << result.status();
  ASSERT_EQ(expected.value().size(), records.size());
  for (std::size_t index = 0; index < records.size(); ++index) {
    EXPECT_EQ(2u, records[index].element_size) << "chunk " << index;
  }
}

}  // namespace
