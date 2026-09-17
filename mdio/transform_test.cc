// Copyright 2026 TGS

// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at

//    http://www.apache.org/licenses/LICENSE-2.0

// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "mdio/transform.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
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

/// Builds a Zarr V3 variable spec (the format mdio-python defaults to).
/// `chunks` defaults to `shape`; Zarr V3 requires chunk entries >= 1, so
/// zero-length dimensions must pass an explicit unit chunk.
::nlohmann::json MakeVariableSpec(const std::string& name,
                                  const std::string& dataType,
                                  const std::vector<mdio::Index>& shape,
                                  const std::vector<mdio::Index>& chunks = {}) {
  ::nlohmann::json spec;
  spec["driver"] = "zarr3";
  spec["kvstore"]["driver"] = "file";
  spec["kvstore"]["path"] = name;
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
  spec["attributes"]["long_name"] = "transform test variable";
  return spec;
}

/// Builds a Zarr V2 variable spec. `dtype` uses the V2 dtype string form
/// (for example "<f4" for float32).
::nlohmann::json MakeVariableSpecV2(
    const std::string& name, const std::string& dtype,
    const std::vector<mdio::Index>& shape,
    const std::vector<mdio::Index>& chunks = {}) {
  ::nlohmann::json spec;
  spec["driver"] = "zarr";
  spec["kvstore"]["driver"] = "file";
  spec["kvstore"]["path"] = name;
  spec["metadata"]["compressor"] = {{"id", "blosc"}};
  spec["metadata"]["dtype"] = dtype;
  spec["metadata"]["shape"] = shape;
  spec["metadata"]["chunks"] = chunks.empty() ? shape : chunks;
  spec["metadata"]["dimension_separator"] = "/";
  spec["metadata"]["zarr_format"] = 2;
  std::vector<std::string> dimensionNames;
  for (std::size_t index = 0; index < shape.size(); ++index) {
    dimensionNames.push_back("dim_" + std::to_string(index));
  }
  spec["attributes"]["dimension_names"] = dimensionNames;
  spec["attributes"]["long_name"] = "transform test variable";
  return spec;
}

/// Builds a Zarr V2 struct spec: fields a (int16) and b (int32), packed to a
/// 6-byte record.
::nlohmann::json MakeStructSpecV2(
    const std::string& name, const std::vector<mdio::Index>& shape = {4, 3},
    const std::vector<mdio::Index>& chunks = {2, 3}) {
  ::nlohmann::json spec;
  spec["driver"] = "zarr";
  spec["kvstore"]["driver"] = "file";
  spec["kvstore"]["path"] = name;
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
  spec["attributes"]["long_name"] = "transform test struct";
  return spec;
}

/// Builds a Zarr V3 struct spec: fields a (int16) and b (int32), packed to a
/// 6-byte record. The innermost bytes codec must name its endianness.
::nlohmann::json MakeStructSpecV3(
    const std::string& name, const std::vector<mdio::Index>& shape = {4, 3},
    const std::vector<mdio::Index>& chunks = {2, 3}) {
  ::nlohmann::json spec;
  spec["driver"] = "zarr3";
  spec["kvstore"]["driver"] = "file";
  spec["kvstore"]["path"] = name;
  spec["metadata"]["data_type"] = {
      {"name", "struct"},
      {"configuration",
       {{"fields", ::nlohmann::json::array({
                       {{"name", "a"}, {"data_type", "int16"}},
                       {{"name", "b"}, {"data_type", "int32"}},
                   })}}}};
  spec["metadata"]["shape"] = shape;
  spec["metadata"]["chunk_grid"]["name"] = "regular";
  spec["metadata"]["chunk_grid"]["configuration"]["chunk_shape"] = chunks;
  spec["metadata"]["chunk_key_encoding"]["name"] = "default";
  spec["metadata"]["chunk_key_encoding"]["configuration"]["separator"] = "/";
  spec["metadata"]["codecs"] = ::nlohmann::json::array({
      {{"name", "bytes"}, {"configuration", {{"endian", "little"}}}},
  });
  spec["attributes"]["dimension_names"] = {"x", "y"};
  spec["attributes"]["long_name"] = "transform test struct v3";
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

/// Creates a Zarr V2 variable at `name` and fills it with `values` in
/// row-major order; an empty `values` only creates the store.
template <typename T>
mdio::Result<mdio::Variable<>> MakePopulatedVariableV2(
    const std::string& name, const std::string& dtype,
    const std::vector<mdio::Index>& shape, const std::vector<T>& values,
    const std::vector<mdio::Index>& chunks = {}) {
  MDIO_ASSIGN_OR_RETURN(
      auto variable,
      mdio::Variable<>::Open(MakeVariableSpecV2(name, dtype, shape, chunks),
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

/// The destination buffer is mutable storage viewed through a string_view;
/// this is the documented way for an ElementTransform to write through it.
char* WritableDestination(std::string_view dst_bytes) {
  return const_cast<char*>(dst_bytes.data());
}

/// Scalar transform: multiplies a float32 element by `gain`.
mdio::ElementTransform MakeFloat32Gain(const float gain) {
  return [gain](std::string_view src_bytes,
                std::string_view dst_bytes) -> absl::Status {
    float value;
    std::memcpy(&value, src_bytes.data(), sizeof(value));
    value *= gain;
    std::memcpy(WritableDestination(dst_bytes), &value, sizeof(value));
    return absl::OkStatus();
  };
}

/// Record transform for the struct {a: int16 @0, b: int32 @2}: doubles field
/// a and copies field b verbatim. Rejects any element that is not one whole
/// 6-byte record, so the test fails loudly if the unit is not the record.
mdio::ElementTransform MakeRecordTransform() {
  return [](std::string_view src_bytes,
            std::string_view dst_bytes) -> absl::Status {
    constexpr std::size_t kRecordSize = 6;
    if (src_bytes.size() != kRecordSize || dst_bytes.size() != kRecordSize) {
      return absl::InternalError("record unit is not 6 bytes");
    }
    std::int16_t fieldA;
    std::memcpy(&fieldA, src_bytes.data(), sizeof(fieldA));
    fieldA = static_cast<std::int16_t>(fieldA * 2);
    std::memcpy(WritableDestination(dst_bytes), &fieldA, sizeof(fieldA));
    std::memcpy(WritableDestination(dst_bytes) + sizeof(fieldA),
                src_bytes.data() + sizeof(fieldA), sizeof(std::int32_t));
    return absl::OkStatus();
  };
}

/// Opens the void byte view of an existing struct store (path-only spec).
mdio::Result<mdio::Variable<>> OpenVoidView(const std::string& name,
                                            const std::string& driver) {
  ::nlohmann::json spec;
  spec["driver"] = driver;
  spec["kvstore"]["driver"] = "file";
  spec["kvstore"]["path"] = name;
  return mdio::Variable<>::Open(spec).result();
}

/// Opens a typed field view of an existing struct store.
template <typename T>
mdio::Result<mdio::Variable<T>> OpenStructField(const std::string& name,
                                                const std::string& field,
                                                const std::string& driver) {
  ::nlohmann::json spec;
  spec["driver"] = driver;
  spec["kvstore"]["driver"] = "file";
  spec["kvstore"]["path"] = name;
  spec["field"] = field;
  return mdio::Variable<T>::Open(spec).result();
}

/// Populates a struct store with packed records through a single void write:
/// field a (int16) at offset 0 and field b (int32) at offset 2 of each
/// 6-byte record. One void write is used because a full-domain write to a
/// field view replaces the whole record — the driver does not
/// read-modify-write the other fields, so field-by-field population would
/// leave every field but the last zeroed.
void PopulateStructRecords(const std::string& name, const std::string& driver,
                           const std::vector<std::int16_t>& fieldAValues,
                           const std::vector<std::int32_t>& fieldBValues) {
  constexpr std::size_t kRecordSize = 6;
  auto voidView = OpenVoidView(name, driver);
  ASSERT_TRUE(voidView.ok()) << voidView.status();
  auto data = tensorstore::AllocateArray<mdio::dtypes::byte_t>(
      voidView.value().get_store().domain().box(),
      mdio::ContiguousLayoutOrder::c, tensorstore::value_init,
      mdio::constants::kByte);
  unsigned char* flat = reinterpret_cast<unsigned char*>(data.data());
  for (std::size_t index = 0; index < fieldAValues.size(); ++index) {
    std::memcpy(flat + index * kRecordSize, &fieldAValues[index],
                sizeof(std::int16_t));
    std::memcpy(flat + index * kRecordSize + sizeof(std::int16_t),
                &fieldBValues[index], sizeof(std::int32_t));
  }
  auto writeFutures =
      tensorstore::Write(std::move(data), voidView.value().get_store());
  writeFutures.commit_future.Wait();
  ASSERT_TRUE(writeFutures.copy_future.status().ok())
      << writeFutures.copy_future.status();
}

/// Reads a field of an existing struct store as flat row-major values.
template <typename T>
std::vector<T> ReadStructFieldValues(const std::string& name,
                                     const std::string& field,
                                     const std::string& driver) {
  auto fieldVariable = OpenStructField<T>(name, field, driver);
  EXPECT_TRUE(fieldVariable.ok()) << fieldVariable.status();
  if (!fieldVariable.ok()) {
    return {};
  }
  auto readResult =
      tensorstore::Read(fieldVariable.value().get_store()).result();
  EXPECT_TRUE(readResult.ok()) << readResult.status();
  if (!readResult.ok()) {
    return {};
  }
  const auto& array = readResult.value();
  const T* flat = array.data();
  return std::vector<T>(flat, flat + array.num_elements());
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

TEST(TransformVariableTest, ScalarGainFloat32V3) {
  const std::vector<float> values = SequentialValues(1.0F, 20);
  auto src = MakePopulatedVariable<float>("transform_test_v3_src", "float32",
                                          {4, 5}, values);
  ASSERT_TRUE(src.ok()) << src.status();
  auto dst = MakePopulatedVariable<float>("transform_test_v3_dst", "float32",
                                          {4, 5}, {});
  ASSERT_TRUE(dst.ok()) << dst.status();

  auto status =
      mdio::TransformVariable(src.value(), dst.value(), MakeFloat32Gain(2.0F))
          .status();
  ASSERT_TRUE(status.ok()) << status;

  const std::vector<float> got = ReadFloat32Values(dst.value());
  ASSERT_EQ(values.size(), got.size());
  for (std::size_t index = 0; index < values.size(); ++index) {
    EXPECT_FLOAT_EQ(values[index] * 2.0F, got[index]);
  }
}

TEST(TransformVariableTest, ScalarGainFloat32V2) {
  const std::vector<float> values = SequentialValues(1.0F, 20);
  auto src = MakePopulatedVariableV2<float>("transform_test_v2_src", "<f4",
                                            {4, 5}, values);
  ASSERT_TRUE(src.ok()) << src.status();
  auto dst = MakePopulatedVariableV2<float>("transform_test_v2_dst", "<f4",
                                            {4, 5}, {});
  ASSERT_TRUE(dst.ok()) << dst.status();

  auto status =
      mdio::TransformVariable(src.value(), dst.value(), MakeFloat32Gain(2.0F))
          .status();
  ASSERT_TRUE(status.ok()) << status;

  const std::vector<float> got = ReadFloat32Values(dst.value());
  ASSERT_EQ(values.size(), got.size());
  for (std::size_t index = 0; index < values.size(); ++index) {
    EXPECT_FLOAT_EQ(values[index] * 2.0F, got[index]);
  }
}

TEST(TransformVariableTest, IdentityEqualsDirectCopy) {
  std::vector<std::int16_t> values;
  for (int index = 0; index < 24; ++index) {
    values.push_back(static_cast<std::int16_t>(100 + index * 7));
  }
  auto src = MakePopulatedVariable<std::int16_t>("transform_test_id_src",
                                                 "int16", {6, 4}, values);
  ASSERT_TRUE(src.ok()) << src.status();
  auto dstTransform = MakePopulatedVariable<std::int16_t>(
      "transform_test_id_dst", "int16", {6, 4}, {});
  ASSERT_TRUE(dstTransform.ok()) << dstTransform.status();
  auto dstDirect = MakePopulatedVariable<std::int16_t>(
      "transform_test_id_direct", "int16", {6, 4}, {});
  ASSERT_TRUE(dstDirect.ok()) << dstDirect.status();

  mdio::ElementTransform identity =
      [](std::string_view src_bytes,
         std::string_view dst_bytes) -> absl::Status {
    std::memcpy(WritableDestination(dst_bytes), src_bytes.data(),
                src_bytes.size());
    return absl::OkStatus();
  };
  auto transformFuture = mdio::TransformVariable(
      src.value(), dstTransform.value(), std::move(identity));
  ASSERT_TRUE(transformFuture.status().ok()) << transformFuture.status();

  // The baseline: a pure dtype-erased copy (read void, write void).
  auto copyRead = tensorstore::Read(src.value().get_store()).result();
  ASSERT_TRUE(copyRead.ok()) << copyRead.status();
  auto copyWrite =
      tensorstore::Write(copyRead.value(), dstDirect.value().get_store());
  copyWrite.commit_future.Wait();
  ASSERT_TRUE(copyWrite.copy_future.status().ok())
      << copyWrite.copy_future.status();

  auto transformRead =
      tensorstore::Read(dstTransform.value().get_store()).result();
  ASSERT_TRUE(transformRead.ok()) << transformRead.status();
  auto directRead = tensorstore::Read(dstDirect.value().get_store()).result();
  ASSERT_TRUE(directRead.ok()) << directRead.status();
  const std::size_t numBytes = values.size() * sizeof(std::int16_t);
  EXPECT_EQ(
      0, std::memcmp(transformRead.value().byte_strided_origin_pointer().get(),
                     directRead.value().byte_strided_origin_pointer().get(),
                     numBytes));
  // And the baseline copy is faithful to the source values.
  const std::int16_t* flat = static_cast<const std::int16_t*>(
      directRead.value().byte_strided_origin_pointer().get());
  for (std::size_t index = 0; index < values.size(); ++index) {
    EXPECT_EQ(values[index], flat[index]);
  }
}

TEST(TransformVariableTest, StructRecordUnitV2) {
  const std::string driver = "zarr";
  auto created =
      mdio::Variable<>::Open(MakeStructSpecV2("transform_test_struct2_src"),
                             mdio::constants::kCreateClean)
          .result();
  ASSERT_TRUE(created.ok()) << created.status();

  std::vector<std::int16_t> fieldAValues;
  std::vector<std::int32_t> fieldBValues;
  for (int index = 0; index < 12; ++index) {
    fieldAValues.push_back(static_cast<std::int16_t>(index + 1));
    fieldBValues.push_back(1000 + index);
  }
  PopulateStructRecords("transform_test_struct2_src", driver, fieldAValues,
                        fieldBValues);

  auto src = OpenVoidView("transform_test_struct2_src", driver);
  ASSERT_TRUE(src.ok()) << src.status();
  auto dstCreated =
      mdio::Variable<>::Open(MakeStructSpecV2("transform_test_struct2_dst"),
                             mdio::constants::kCreateClean)
          .result();
  ASSERT_TRUE(dstCreated.ok()) << dstCreated.status();
  auto dst = OpenVoidView("transform_test_struct2_dst", driver);
  ASSERT_TRUE(dst.ok()) << dst.status();

  // The void view must be the record view: rank 3 with a trailing 6-byte axis.
  const auto dstShape = dst.value().dimensions().shape();
  ASSERT_EQ(3, dstShape.size());
  ASSERT_EQ(6, dstShape[2]);

  auto status =
      mdio::TransformVariable(src.value(), dst.value(), MakeRecordTransform())
          .status();
  ASSERT_TRUE(status.ok()) << status;

  const std::vector<std::int16_t> gotA = ReadStructFieldValues<std::int16_t>(
      "transform_test_struct2_dst", "a", driver);
  const std::vector<std::int32_t> gotB = ReadStructFieldValues<std::int32_t>(
      "transform_test_struct2_dst", "b", driver);
  ASSERT_EQ(fieldAValues.size(), gotA.size());
  ASSERT_EQ(fieldBValues.size(), gotB.size());
  for (std::size_t index = 0; index < fieldAValues.size(); ++index) {
    EXPECT_EQ(fieldAValues[index] * 2, gotA[index]);
    EXPECT_EQ(fieldBValues[index], gotB[index]);
  }
}

TEST(TransformVariableTest, StructRecordUnitV3) {
  const std::string driver = "zarr3";
  auto created =
      mdio::Variable<>::Open(MakeStructSpecV3("transform_test_struct3_src"),
                             mdio::constants::kCreateClean)
          .result();
  ASSERT_TRUE(created.ok()) << created.status();

  std::vector<std::int16_t> fieldAValues;
  std::vector<std::int32_t> fieldBValues;
  for (int index = 0; index < 12; ++index) {
    fieldAValues.push_back(static_cast<std::int16_t>(index + 1));
    fieldBValues.push_back(1000 + index);
  }
  PopulateStructRecords("transform_test_struct3_src", driver, fieldAValues,
                        fieldBValues);

  auto src = OpenVoidView("transform_test_struct3_src", driver);
  ASSERT_TRUE(src.ok()) << src.status();
  auto dstCreated =
      mdio::Variable<>::Open(MakeStructSpecV3("transform_test_struct3_dst"),
                             mdio::constants::kCreateClean)
          .result();
  ASSERT_TRUE(dstCreated.ok()) << dstCreated.status();
  auto dst = OpenVoidView("transform_test_struct3_dst", driver);
  ASSERT_TRUE(dst.ok()) << dst.status();

  // The void view must be the record view: rank 3 with a trailing 6-byte axis.
  const auto dstShape = dst.value().dimensions().shape();
  ASSERT_EQ(3, dstShape.size());
  ASSERT_EQ(6, dstShape[2]);

  auto status =
      mdio::TransformVariable(src.value(), dst.value(), MakeRecordTransform())
          .status();
  ASSERT_TRUE(status.ok()) << status;

  const std::vector<std::int16_t> gotA = ReadStructFieldValues<std::int16_t>(
      "transform_test_struct3_dst", "a", driver);
  const std::vector<std::int32_t> gotB = ReadStructFieldValues<std::int32_t>(
      "transform_test_struct3_dst", "b", driver);
  ASSERT_EQ(fieldAValues.size(), gotA.size());
  ASSERT_EQ(fieldBValues.size(), gotB.size());
  for (std::size_t index = 0; index < fieldAValues.size(); ++index) {
    EXPECT_EQ(fieldAValues[index] * 2, gotA[index]);
    EXPECT_EQ(fieldBValues[index], gotB[index]);
  }
}

TEST(TransformVariableTest, FnErrorLeavesDstUntouched) {
  const std::vector<float> values = SequentialValues(0.0F, 20);
  auto src = MakePopulatedVariable<float>("transform_test_err_src", "float32",
                                          {4, 5}, values);
  ASSERT_TRUE(src.ok()) << src.status();
  const std::vector<float> sentinel(20, 7.0F);
  auto dst = MakePopulatedVariable<float>("transform_test_err_dst", "float32",
                                          {4, 5}, sentinel);
  ASSERT_TRUE(dst.ok()) << dst.status();

  auto calls = std::make_shared<int>(0);
  mdio::ElementTransform failing =
      [calls](std::string_view src_bytes,
              std::string_view dst_bytes) -> absl::Status {
    ++(*calls);
    if (*calls > 10) {
      return absl::InternalError("transform failed mid-way");
    }
    std::memcpy(WritableDestination(dst_bytes), src_bytes.data(),
                src_bytes.size());
    return absl::OkStatus();
  };
  auto transformFuture =
      mdio::TransformVariable(src.value(), dst.value(), std::move(failing));
  const absl::Status status = transformFuture.status();
  ASSERT_FALSE(status.ok());
  EXPECT_EQ(absl::StatusCode::kInternal, status.code());
  EXPECT_EQ(11, *calls);

  // dst keeps its prior contents: the partial buffer was never submitted.
  const std::vector<float> got = ReadFloat32Values(dst.value());
  ASSERT_EQ(sentinel.size(), got.size());
  for (std::size_t index = 0; index < sentinel.size(); ++index) {
    EXPECT_FLOAT_EQ(sentinel[index], got[index]);
  }
}

TEST(TransformVariableTest, Int16Gain) {
  std::vector<std::int16_t> values;
  for (int index = 0; index < 18; ++index) {
    values.push_back(static_cast<std::int16_t>(index - 9));
  }
  auto src = MakePopulatedVariable<std::int16_t>("transform_test_i16_src",
                                                 "int16", {3, 6}, values);
  ASSERT_TRUE(src.ok()) << src.status();
  auto dst = MakePopulatedVariable<std::int16_t>("transform_test_i16_dst",
                                                 "int16", {3, 6}, {});
  ASSERT_TRUE(dst.ok()) << dst.status();

  mdio::ElementTransform gain = [](std::string_view src_bytes,
                                   std::string_view dst_bytes) -> absl::Status {
    std::int16_t value;
    std::memcpy(&value, src_bytes.data(), sizeof(value));
    value = static_cast<std::int16_t>(value * 3);
    std::memcpy(WritableDestination(dst_bytes), &value, sizeof(value));
    return absl::OkStatus();
  };
  auto status =
      mdio::TransformVariable(src.value(), dst.value(), std::move(gain))
          .status();
  ASSERT_TRUE(status.ok()) << status;

  auto readResult = tensorstore::Read(dst.value().get_store()).result();
  ASSERT_TRUE(readResult.ok()) << readResult.status();
  const auto& array = readResult.value();
  const std::int16_t* flat = static_cast<const std::int16_t*>(
      array.byte_strided_origin_pointer().get());
  ASSERT_EQ(values.size(), array.num_elements());
  for (std::size_t index = 0; index < values.size(); ++index) {
    EXPECT_EQ(static_cast<std::int16_t>(values[index] * 3), flat[index]);
  }
}

TEST(TransformVariableTest, Float64Scale) {
  std::vector<double> values;
  for (int index = 0; index < 10; ++index) {
    values.push_back(2.0 * index);
  }
  auto src = MakePopulatedVariable<double>("transform_test_f64_src", "float64",
                                           {5, 2}, values);
  ASSERT_TRUE(src.ok()) << src.status();
  auto dst = MakePopulatedVariable<double>("transform_test_f64_dst", "float64",
                                           {5, 2}, {});
  ASSERT_TRUE(dst.ok()) << dst.status();

  mdio::ElementTransform scale =
      [](std::string_view src_bytes,
         std::string_view dst_bytes) -> absl::Status {
    double value;
    std::memcpy(&value, src_bytes.data(), sizeof(value));
    value *= 0.5;
    std::memcpy(WritableDestination(dst_bytes), &value, sizeof(value));
    return absl::OkStatus();
  };
  auto status =
      mdio::TransformVariable(src.value(), dst.value(), std::move(scale))
          .status();
  ASSERT_TRUE(status.ok()) << status;

  auto readResult = tensorstore::Read(dst.value().get_store()).result();
  ASSERT_TRUE(readResult.ok()) << readResult.status();
  const auto& array = readResult.value();
  const double* flat =
      static_cast<const double*>(array.byte_strided_origin_pointer().get());
  ASSERT_EQ(values.size(), array.num_elements());
  for (std::size_t index = 0; index < values.size(); ++index) {
    EXPECT_NEAR(values[index] * 0.5, flat[index], 1e-12);
  }
}

TEST(TransformVariableTest, CrossDtypeInt16ToFloat32) {
  std::vector<std::int16_t> values;
  for (int index = 0; index < 12; ++index) {
    values.push_back(static_cast<std::int16_t>(index - 4));
  }
  auto src = MakePopulatedVariable<std::int16_t>("transform_test_x_src",
                                                 "int16", {3, 4}, values);
  ASSERT_TRUE(src.ok()) << src.status();
  auto dst = MakePopulatedVariable<float>("transform_test_x_dst", "float32",
                                          {3, 4}, {});
  ASSERT_TRUE(dst.ok()) << dst.status();

  mdio::ElementTransform toFloat =
      [](std::string_view src_bytes,
         std::string_view dst_bytes) -> absl::Status {
    std::int16_t value;
    std::memcpy(&value, src_bytes.data(), sizeof(value));
    const float asFloat = static_cast<float>(value) * 10.0F;
    std::memcpy(WritableDestination(dst_bytes), &asFloat, sizeof(asFloat));
    return absl::OkStatus();
  };
  auto status =
      mdio::TransformVariable(src.value(), dst.value(), std::move(toFloat))
          .status();
  ASSERT_TRUE(status.ok()) << status;

  const std::vector<float> got = ReadFloat32Values(dst.value());
  ASSERT_EQ(values.size(), got.size());
  for (std::size_t index = 0; index < values.size(); ++index) {
    EXPECT_FLOAT_EQ(static_cast<float>(values[index]) * 10.0F, got[index]);
  }
}

TEST(TransformVariableTest, ShapeAndRankMismatchRejected) {
  auto src = MakePopulatedVariable<float>("transform_test_m_src", "float32",
                                          {4, 5}, SequentialValues(0.0F, 20));
  ASSERT_TRUE(src.ok()) << src.status();

  auto wrongShape = MakePopulatedVariable<float>("transform_test_m_shape",
                                                 "float32", {4, 6}, {});
  ASSERT_TRUE(wrongShape.ok()) << wrongShape.status();
  auto status = mdio::TransformVariable(src.value(), wrongShape.value(),
                                        MakeFloat32Gain(2.0F))
                    .status();
  EXPECT_FALSE(status.ok());
  EXPECT_THAT(status.message(), ::testing::HasSubstr("same shape"));

  auto wrongRank =
      MakePopulatedVariable<float>("transform_test_m_rank", "float32", {5}, {});
  ASSERT_TRUE(wrongRank.ok()) << wrongRank.status();
  status = mdio::TransformVariable(src.value(), wrongRank.value(),
                                   MakeFloat32Gain(2.0F))
               .status();
  EXPECT_FALSE(status.ok());
  EXPECT_THAT(status.message(), ::testing::HasSubstr("same rank"));
}

TEST(TransformVariableTest, ElementCountMismatchRejected) {
  // Scalar [4, 5, 6] float32: 120 elements of 4 bytes.
  auto scalar = MakePopulatedVariable<float>("transform_test_c_scalar",
                                             "float32", {4, 5, 6}, {});
  ASSERT_TRUE(scalar.ok()) << scalar.status();
  // Struct {a: int16, b: int32} [4, 5]: the void view is [4, 5, 6] — the same
  // rank and shape, but 20 records of 6 bytes.
  auto created =
      mdio::Variable<>::Open(
          MakeStructSpecV3("transform_test_c_struct", {4, 5}, {2, 5}),
          mdio::constants::kCreateClean)
          .result();
  ASSERT_TRUE(created.ok()) << created.status();
  auto structView = OpenVoidView("transform_test_c_struct", "zarr3");
  ASSERT_TRUE(structView.ok()) << structView.status();

  auto status = mdio::TransformVariable(scalar.value(), structView.value(),
                                        MakeFloat32Gain(2.0F))
                    .status();
  EXPECT_FALSE(status.ok());
  EXPECT_THAT(status.message(), ::testing::HasSubstr("same element count"));
}

TEST(TransformVariableTest, SlicedVariablesTransform) {
  const std::vector<float> values = SequentialValues(0.0F, 20);
  auto src = MakePopulatedVariable<float>("transform_test_slice_src", "float32",
                                          {4, 5}, values);
  ASSERT_TRUE(src.ok()) << src.status();
  const std::vector<float> sentinel(20, 99.0F);
  auto dst = MakePopulatedVariable<float>("transform_test_slice_dst", "float32",
                                          {4, 5}, sentinel);
  ASSERT_TRUE(dst.ok()) << dst.status();

  auto srcSlice = src.value().slice({{"dim_0", 0, 2}});
  ASSERT_TRUE(srcSlice.ok()) << srcSlice.status();
  auto dstSlice = dst.value().slice({{"dim_0", 0, 2}});
  ASSERT_TRUE(dstSlice.ok()) << dstSlice.status();

  auto status = mdio::TransformVariable(srcSlice.value(), dstSlice.value(),
                                        MakeFloat32Gain(2.0F))
                    .status();
  ASSERT_TRUE(status.ok()) << status;

  // Rows 0-1 transformed; rows 2-3 keep the sentinel.
  const std::vector<float> got = ReadFloat32Values(dst.value());
  ASSERT_EQ(values.size(), got.size());
  for (int row = 0; row < 4; ++row) {
    for (int col = 0; col < 5; ++col) {
      const float expected =
          row < 2 ? values[row * 5 + col] * 2.0F : sentinel[row * 5 + col];
      EXPECT_FLOAT_EQ(expected, got[row * 5 + col]);
    }
  }
}

TEST(TransformVariableTest, EmptyVariableTransformsToOk) {
  auto src = MakePopulatedVariable<float>("transform_test_empty_src", "float32",
                                          {0, 4}, {}, {1, 4});
  ASSERT_TRUE(src.ok()) << src.status();
  auto dst = MakePopulatedVariable<float>("transform_test_empty_dst", "float32",
                                          {0, 4}, {}, {1, 4});
  ASSERT_TRUE(dst.ok()) << dst.status();

  auto calls = std::make_shared<int>(0);
  mdio::ElementTransform counting =
      [calls](std::string_view src_bytes,
              std::string_view dst_bytes) -> absl::Status {
    ++(*calls);
    std::memcpy(WritableDestination(dst_bytes), src_bytes.data(),
                src_bytes.size());
    return absl::OkStatus();
  };
  auto status =
      mdio::TransformVariable(src.value(), dst.value(), std::move(counting))
          .status();
  EXPECT_TRUE(status.ok()) << status;
  EXPECT_EQ(0, *calls);
}

}  // namespace
