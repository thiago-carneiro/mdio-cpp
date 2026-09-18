// Copyright 2024 TGS

// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at

//    http://www.apache.org/licenses/LICENSE-2.0

// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "mdio/stats.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "mdio/dataset.h"
#include "mdio/variable.h"
#include "tensorstore/tensorstore.h"
#include "tensorstore/util/status_testutil.h"

// clang-format off
#include <nlohmann/json.hpp>  // NOLINT
// clang-format on

namespace {

auto getCenterHist() {
  std::vector<float> binCenters = {1.0, 2.0, 3.0};
  std::vector<int32_t> counts = {1, 2, 3};
  return std::make_unique<mdio::internal::CenteredBinHistogram<float>>(
      binCenters, counts);
}

auto getEdgeHist() {
  std::vector<float> binEdges = {0.0, 1.0, 2.0, 3.0};
  std::vector<float> binWidths = {1.0, 1.0, 1.0};
  std::vector<int32_t> counts = {1, 2, 3};
  return std::make_unique<mdio::internal::EdgeDefinedHistogram<float>>(
      binEdges, binWidths, counts);
}

TEST(HistogramTest, constructCenterHist) {
  auto histogram = getCenterHist();
  nlohmann::json expected = {
      {"histogram", {{"binCenters", {1.0, 2.0, 3.0}}, {"counts", {1, 2, 3}}}}};
  EXPECT_EQ(histogram->getHistogram(), expected);
}

TEST(HistogramTest, constructEdgeHist) {
  auto histogram = getEdgeHist();
  nlohmann::json expected = {{"histogram",
                              {{"binEdges", {0.0, 1.0, 2.0, 3.0}},
                               {"binWidths", {1.0, 1.0, 1.0}},
                               {"counts", {1, 2, 3}}}}};
  EXPECT_EQ(histogram->getHistogram(), expected);
}

TEST(HistogramTest, centeredBinHistogramClone) {
  auto histogram = getCenterHist();
  auto clone = histogram->clone();
  EXPECT_EQ(clone->getHistogram(), histogram->getHistogram());
}

TEST(HistogramTest, edgeDefinedHistogramClone) {
  auto histogram = getEdgeHist();
  auto clone = histogram->clone();
  EXPECT_EQ(clone->getHistogram(), histogram->getHistogram());
}

TEST(HistogramTest, centeredFromJSON) {
  nlohmann::json expected = {
      {"histogram", {{"binCenters", {1.0, 2.0, 3.0}}, {"counts", {1, 2, 3}}}}};
  auto inertHist = mdio::internal::CenteredBinHistogram<float>({}, {});
  auto attrsRes = inertHist.FromJson(expected);
  ASSERT_TRUE(attrsRes.status().ok()) << attrsRes.status();
  auto histogram = std::move(attrsRes.value());
  EXPECT_EQ(histogram->getHistogram(), expected);
}

TEST(HistogramTest, edgeFromJSON) {
  nlohmann::json expected = {{"histogram",
                              {{"binEdges", {0.0, 1.0, 2.0, 3.0}},
                               {"binWidths", {1.0, 1.0, 1.0}},
                               {"counts", {1, 2, 3}}}}};
  auto inertHist = mdio::internal::EdgeDefinedHistogram<float>({}, {}, {});
  auto attrsRes = inertHist.FromJson(expected);
  ASSERT_TRUE(attrsRes.status().ok()) << attrsRes.status();
  auto histogram = std::move(attrsRes.value());
  EXPECT_EQ(histogram->getHistogram(), expected);
}

TEST(SummaryStatsTest, fromJson) {
  nlohmann::json expected = {
      {"count", 100},
      {"min", -1000.0},
      {"max", 1000.0},
      {"sum", 0.0},
      {"sumSquares", 0.0},
      {"histogram", {{"binCenters", {1.0, 2.0, 3.0}}, {"counts", {1, 2, 3}}}}};
  auto statsRes = mdio::internal::SummaryStats::FromJson(expected);
  ASSERT_TRUE(statsRes.status().ok()) << statsRes.status();
  auto stats = statsRes.value();
  EXPECT_EQ(stats.getBindable(), expected);
}

TEST(SummaryStatsTest, fromJsonInt) {
  nlohmann::json expected = {
      {"count", 100},
      {"min", -1000},
      {"max", 1000},
      {"sum", 0},
      {"sumSquares", 0},
      {"histogram", {{"binCenters", {1.0, 2.0, 3.0}}, {"counts", {1, 2, 3}}}}};
  auto statsRes = mdio::internal::SummaryStats::FromJson<int32_t>(expected);
  ASSERT_TRUE(statsRes.status().ok()) << statsRes.status();
  auto stats = statsRes.value();
  EXPECT_EQ(stats.getBindable(), expected);
}

TEST(SummaryStatsTest, fromJsonMissing) {
  nlohmann::json expected = {
      {"count", 100},
      {"min", -1000.0},
      // {"max", 1000.0},  // User forgot to add max field (required)
      {"sum", 0.0},
      {"sumSquares", 0.0},
      {"histogram", {{"binCenters", {1.0, 2.0, 3.0}}, {"counts", {1, 2, 3}}}}};
  auto statsRes = mdio::internal::SummaryStats::FromJson(expected);
  ASSERT_FALSE(statsRes.status().ok()) << statsRes.status();
}

TEST(SummaryStatsTest, fromJsonRejectsWrongTypedScalarFields) {
  // Each scalar field with a wrong-typed value must be rejected with an
  // error naming the field and the expected type, not an uncaught nlohmann
  // exception escaping the open path.
  const std::vector<std::pair<std::string, nlohmann::json>> wrongTyped = {
      {"count", "100"},
      {"max", {1.0, 2.0}},
      {"min", nullptr},
      {"sum", true},
      {"sumSquares", "0.0"}};
  for (const auto& [field, wrongValue] : wrongTyped) {
    nlohmann::json expected = {
        {"count", 100},
        {"min", -1000.0},
        {"max", 1000.0},
        {"sum", 0.0},
        {"sumSquares", 0.0},
        {"histogram",
         {{"binCenters", {1.0, 2.0, 3.0}}, {"counts", {1, 2, 3}}}}};
    expected[field] = wrongValue;
    auto statsRes = mdio::internal::SummaryStats::FromJson(expected);
    ASSERT_FALSE(statsRes.status().ok()) << field;
    EXPECT_THAT(statsRes.status().message(), ::testing::HasSubstr(field));
    EXPECT_THAT(statsRes.status().message(),
                ::testing::HasSubstr("expected number"));
  }
}

TEST(UserAttributesTest, fromJsonNoStats) {
  nlohmann::json expected = {{"attributes",
                              {{"foo", "bar"},
                               {"life", 42},
                               {"pi", 3.14159},
                               {"truth", true},
                               {"lies", false},
                               {"nothing", nullptr}}}};
  auto attrsRes = mdio::UserAttributes::FromJson(expected);
  ASSERT_TRUE(attrsRes.status().ok()) << attrsRes.status();
  auto attrs = attrsRes.value();
  EXPECT_EQ(attrs.ToJson(), expected);
}

TEST(UserAttributesTest, fromJsonNoAttrs) {
  nlohmann::json expected = {
      {"statsV1",
       {{"histogram", {{"binCenters", {1.0, 2.0, 3.0}}, {"counts", {1, 2, 3}}}},
        {"count", 100},
        {"min", -1000.0},
        {"max", 1000.0},
        {"sum", 0.0},
        {"sumSquares", 0.0}}}};
  auto attrsRes = mdio::UserAttributes::FromJson(expected);
  ASSERT_TRUE(attrsRes.status().ok()) << attrsRes.status();
  auto attrs = attrsRes.value();
  EXPECT_EQ(attrs.ToJson(), expected);
}

TEST(UserAttributesTest, nothing) {
  nlohmann::json expected = nlohmann::json::object();
  auto attrs = mdio::UserAttributes::FromJson(expected);
  EXPECT_EQ(attrs.value().ToJson(), expected);
  auto none = mdio::UserAttributes::FromJson(expected);
  ASSERT_TRUE(none.status().ok()) << none.status();
  EXPECT_EQ(none.value().ToJson(), expected);
}

TEST(UserAttributesTest, fromJsonWithAttrs) {
  nlohmann::json expected = {
      {"statsV1",
       {{"histogram", {{"binCenters", {1.0, 2.0, 3.0}}, {"counts", {1, 2, 3}}}},
        {"count", 100},
        {"min", -1000.0},
        {"max", 1000.0},
        {"sum", 0.0},
        {"sumSquares", 0.0}}},
      {"attributes",
       {{"foo", "bar"},
        {"life", 42},
        {"pi", 3.14159},
        {"truth", true},
        {"lies", false},
        {"nothing", nullptr}}}};
  auto attrsRes = mdio::UserAttributes::FromJson(expected);
  ASSERT_TRUE(attrsRes.status().ok()) << attrsRes.status();
  auto attrs = attrsRes.value();
  EXPECT_EQ(attrs.ToJson(), expected);
}

TEST(UserAttributesTest, fromJsonStatsList) {
  nlohmann::json expected = {
      {"statsV1",
       {{{"histogram",
          {{"binCenters", {1.0, 2.0, 3.0}}, {"counts", {1, 2, 3}}}},
         {"count", 100},
         {"min", -1000.0},
         {"max", 1000.0},
         {"sum", 2000.0},
         {"sumSquares", 20000.0}},
        {{"histogram",
          {{"binEdges", {0.5, 1.5, 2.5, 3.5}},
           {"binWidths", {1.5, 3.0, 9.0}},
           {"counts", {3, 2, 1}}}},
         {"count", 789},
         {"min", -500.0},
         {"max", 500.0},
         {"sum", 1000.0},
         {"sumSquares", 15000.0}}}},
      {"attributes",
       {{"foo", "bar"},
        {"life", 42},
        {"pi", 3.14159},
        {"truth", true},
        {"lies", false},
        {"nothing", nullptr}}}};
  auto attrsRes = mdio::UserAttributes::FromJson(expected);
  ASSERT_TRUE(attrsRes.status().ok()) << attrsRes.status();
  auto attrs = attrsRes.value();
  EXPECT_EQ(attrs.ToJson(), expected);
}

TEST(UserAttributesTest, fromDataset) {
  std::string schema = R"(
        {
  "metadata": {
    "name": "campos_3d",
    "apiVersion": "1.0.0",
    "createdOn": "2023-12-12T15:02:06.413469-06:00",    
    "attributes": {
      "textHeader": [
        "C01 .......................... ",
        "C02 .......................... ",
        "C03 .......................... "
      ],
      "foo": "bar"
    }
  },
  "variables": [
    {
      "name": "image",
      "dataType": "float32",
      "dimensions": [
        {"name": "inline", "size": 256},
        {"name": "crossline", "size": 512},
        {"name": "depth", "size": 384}
      ],
      "metadata": {
        "chunkGrid": {
          "name": "regular",
          "configuration": { "chunkShape": [128, 128, 128] }
        },
        "statsV1": {
          "count": 100,
          "sum": 1215.1,
          "sumSquares": 125.12,
          "min": 5.61,
          "max": 10.84,
          "histogram": {"binCenters":  [1, 2], "counts":  [10, 15]}
        },
        "attributes": {
          "fizz": "buzz"
        }
    },
      "coordinates": ["inline", "crossline", "depth", "cdp-x", "cdp-y"],
      "compressor": {"name": "blosc", "algorithm": "zstd"}
    },
    {
      "name": "velocity",
      "dataType": "float16",
      "dimensions": ["inline", "crossline", "depth"],
      "metadata": {
        "chunkGrid": {
          "name": "regular",
          "configuration": { "chunkShape": [128, 128, 128] }
        },
        "unitsV1": {"speed": "m/s"}
      },
      "coordinates": ["inline", "crossline", "depth", "cdp-x", "cdp-y"]
    },
    {
      "name": "image_inline",
      "dataType": "float32",
      "dimensions": ["inline", "crossline", "depth"],
      "longName": "inline optimized version of 3d_stack",
      "compressor": {"name": "blosc", "algorithm": "zstd"},
      "metadata": {
        "chunkGrid": {
          "name": "regular",
          "configuration": { "chunkShape": [4, 512, 512] }
        }
      },
      "coordinates": ["inline", "crossline", "depth", "cdp-x", "cdp-y"]
    },
    {
      "name": "image_headers",
      "dataType": {
        "fields": [
          {"name": "cdp-x", "format": "int32"},
          {"name": "cdp-y", "format": "int32"},
          {"name": "elevation", "format": "float16"},
          {"name": "some_scalar", "format": "float16"}
        ]
      },
      "dimensions": ["inline", "crossline"],
      "metadata": {
        "chunkGrid": {
          "name": "regular",
          "configuration": { "chunkShape": [128, 128] }
        }
      },
      "coordinates": ["inline", "crossline", "cdp-x", "cdp-y"]
    },
    {
      "name": "inline",
      "dataType": "uint32",
      "dimensions": [{"name": "inline", "size": 256}]
    },
    {
      "name": "crossline",
      "dataType": "uint32",
      "dimensions": [{"name": "crossline", "size": 512}]
    },
    {
      "name": "depth",
      "dataType": "uint32",
      "dimensions": [{"name": "depth", "size": 384}],
      "metadata": {
        "unitsV1": { "length": "m" }
      }
    },
    {
      "name": "cdp-x",
      "dataType": "float32",
      "dimensions": [
        {"name": "inline", "size": 256},
        {"name": "crossline", "size": 512}
      ],
      "metadata": {
        "unitsV1": { "length": "m" }
      }
    },
    {
      "name": "cdp-y",
      "dataType": "float32",
      "dimensions": [
        {"name": "inline", "size": 256},
        {"name": "crossline", "size": 512}
      ],
      "metadata": {
        "unitsV1": { "length": "m" }
      }
    }
  ]
}
    )";
  auto j = nlohmann::json::parse(schema);

  auto imgRes = mdio::UserAttributes::FromDatasetJson(j, "image");
  ASSERT_TRUE(imgRes.status().ok()) << imgRes.status();
  nlohmann::json expectedImage = nlohmann::json::object();
  expectedImage["statsV1"] = j["variables"][0]["metadata"]["statsV1"];
  expectedImage["attributes"] = j["variables"][0]["metadata"]["attributes"];

  auto boundUserAttrs = imgRes.value().ToJson();
  ASSERT_TRUE(boundUserAttrs.contains("statsV1"));
  ASSERT_TRUE(boundUserAttrs.contains("attributes"));

  // Floating point error is expected but gross.
  EXPECT_EQ(boundUserAttrs["attributes"], expectedImage["attributes"]);
  EXPECT_NEAR(boundUserAttrs["statsV1"]["count"],
              expectedImage["statsV1"]["count"], 1e-4);
  EXPECT_NEAR(boundUserAttrs["statsV1"]["min"], expectedImage["statsV1"]["min"],
              1e-4);
  EXPECT_NEAR(boundUserAttrs["statsV1"]["max"], expectedImage["statsV1"]["max"],
              1e-4);
  EXPECT_NEAR(boundUserAttrs["statsV1"]["sum"], expectedImage["statsV1"]["sum"],
              1e-4);
  EXPECT_NEAR(boundUserAttrs["statsV1"]["sumSquares"],
              expectedImage["statsV1"]["sumSquares"], 1e-4);
  // These should be ints
  EXPECT_EQ(boundUserAttrs["statsV1"]["histogram"]["binCenters"][0],
            expectedImage["statsV1"]["histogram"]["binCenters"][0]);
  EXPECT_EQ(boundUserAttrs["statsV1"]["histogram"]["binCenters"][1],
            expectedImage["statsV1"]["histogram"]["binCenters"][1]);
  EXPECT_EQ(boundUserAttrs["statsV1"]["histogram"]["counts"][0],
            expectedImage["statsV1"]["histogram"]["counts"][0]);
  EXPECT_EQ(boundUserAttrs["statsV1"]["histogram"]["counts"][1],
            expectedImage["statsV1"]["histogram"]["counts"][1]);

  auto missingVar = mdio::UserAttributes::FromDatasetJson(j, "xline");
  ASSERT_FALSE(missingVar.status().ok());
  EXPECT_EQ(missingVar.status().message(),
            "Variable xline not found in Dataset");
}

TEST(UserAttributes, locationAndReassignment) {
  nlohmann::json expected = {
      {"statsV1",
       {{"histogram", {{"binCenters", {1.0, 2.0, 3.0}}, {"counts", {1, 2, 3}}}},
        {"count", 100},
        {"min", -1000.0},
        {"max", 1000.0},
        {"sum", 0.0},
        {"sumSquares", 0.0}}},
      {"attributes",
       {{"foo", "bar"},
        {"life", 42},
        {"pi", 3.14159},
        {"truth", true},
        {"lies", false},
        {"nothing", nullptr}}}};

  auto attrsRes = mdio::UserAttributes::FromJson(expected);
  ASSERT_TRUE(attrsRes.status().ok()) << attrsRes.status();

  // This is the way I would like to do it. However, the copy constructor gets
  // deleted by the compiler and pivoting to a unique_ptr should be safer from
  // memory leaks and dangling pointers. auto attrs = attrsRes.value();
  // ASSERT_EQ(attrs.ToJson(), expected);
  // const void* attrsAddress = static_cast<const void*>(&attrs);
  // auto dittoAttrsRes = mdio::UserAttributes::FromJson(attrs.ToJson());
  // ASSERT_TRUE(dittoAttrsRes.status().ok()) << dittoAttrsRes.status();
  // auto dittoAttrs = dittoAttrsRes.value();
  // ASSERT_EQ(dittoAttrs.ToJson(), attrs.ToJson());
  // const void* dittoAttrsAddress = static_cast<const void*>(&dittoAttrs);
  // EXPECT_NE(attrsAddress, dittoAttrsAddress) << "Expected a different address
  // but got the same one!";

  // expected["attributes"]["foo"] = "baz";
  // dittoAttrs = mdio::UserAttributes::FromJson(expected).value();
  // EXPECT_EQ(dittoAttrs.ToJson(), expected);

  std::unique_ptr<mdio::UserAttributes> attrs =
      std::make_unique<mdio::UserAttributes>(attrsRes.value());
  const void* attrsAddress = static_cast<const void*>(attrs.get());
  auto dittoAttrsRes = mdio::UserAttributes::FromJson(attrs->ToJson());
  ASSERT_TRUE(dittoAttrsRes.status().ok()) << dittoAttrsRes.status();
  std::unique_ptr<mdio::UserAttributes> dittoAttrs =
      std::make_unique<mdio::UserAttributes>(dittoAttrsRes.value());
  const void* dittoAttrsAddress = static_cast<const void*>(dittoAttrs.get());
  EXPECT_NE(attrsAddress, dittoAttrsAddress)
      << "Expected a different address but got the same one!";

  expected["attributes"]["foo"] = "baz";
  dittoAttrs = std::make_unique<mdio::UserAttributes>(
      mdio::UserAttributes::FromJson(expected).value());
  EXPECT_EQ(dittoAttrs->ToJson(), expected);
  const void* newDittoAttrsAddress = static_cast<const void*>(dittoAttrs.get());
  EXPECT_NE(dittoAttrsAddress, newDittoAttrsAddress)
      << "Expected a different address but got the same one!";

  // auto newAttrs = std::move(attr)
}

TEST(Units, unitsFromJsonObject) {
  // Test when unitsV1 is provided as an object.
  nlohmann::json json_input = {{"unitsV1", {{"length", "m"}}}};
  auto uaRes = mdio::UserAttributes::FromJson(json_input);
  ASSERT_TRUE(uaRes.status().ok()) << uaRes.status();
  mdio::UserAttributes ua = uaRes.value();
  // When an object is provided, the FromJson implementation pushes back the
  // unit value, so with one element it will return directly (not wrapped in an
  // array)
  nlohmann::json ua_json = ua.ToJson();
  EXPECT_TRUE(ua_json.contains("unitsV1"));
  EXPECT_EQ(ua_json["unitsV1"], "m");
}

TEST(Units, unitsFromJsonArrayOfObjects) {
  // Test when unitsV1 is provided as an array of objects.
  nlohmann::json json_input = {
      {"unitsV1", {{{"length", "m"}}, {{"time", "s"}}}}};
  auto uaRes = mdio::UserAttributes::FromJson(json_input);
  ASSERT_TRUE(uaRes.status().ok()) << uaRes.status();
  mdio::UserAttributes ua = uaRes.value();
  nlohmann::json ua_json = ua.ToJson();
  EXPECT_TRUE(ua_json.contains("unitsV1"));
  // With more than one unit, the units-bindable returns an array.
  nlohmann::json expected = {"m", "s"};
  EXPECT_EQ(ua_json["unitsV1"], expected);
}

TEST(Units, unitsFromJsonString) {
  // Test when unitsV1 is provided as a plain string.
  nlohmann::json json_input = {{"unitsV1", "rad"}};
  auto uaRes = mdio::UserAttributes::FromJson(json_input);
  ASSERT_TRUE(uaRes.status().ok()) << uaRes.status();
  mdio::UserAttributes ua = uaRes.value();
  nlohmann::json ua_json = ua.ToJson();
  EXPECT_TRUE(ua_json.contains("unitsV1"));
  EXPECT_EQ(ua_json["unitsV1"], "rad");
}

TEST(HistogramTest, isBindable) {
  EXPECT_TRUE(getCenterHist()->isBindable());
  EXPECT_TRUE(getEdgeHist()->isBindable());
}

TEST(HistogramTest, fromJsonErrorPaths) {
  nlohmann::json noParent = {{"binCenters", {1.0, 2.0}}, {"counts", {1, 2}}};
  auto centered = mdio::internal::CenteredBinHistogram<float>({}, {});
  EXPECT_FALSE(centered.FromJson(noParent).status().ok());

  nlohmann::json noChild = {{"histogram", {{"counts", {1, 2}}}}};
  EXPECT_FALSE(centered.FromJson(noChild).status().ok());

  nlohmann::json edgeNoParent = {
      {"binEdges", {0.0, 1.0}}, {"binWidths", {1.0}}, {"counts", {1}}};
  auto edge = mdio::internal::EdgeDefinedHistogram<float>({}, {}, {});
  EXPECT_FALSE(edge.FromJson(edgeNoParent).status().ok());

  nlohmann::json edgeNoChild = {
      {"histogram", {{"binEdges", {0.0, 1.0}}, {"counts", {1}}}}};
  EXPECT_FALSE(edge.FromJson(edgeNoChild).status().ok());
}

TEST(HistogramTest, fromJsonRejectsWrongTypedFields) {
  auto centered = mdio::internal::CenteredBinHistogram<float>({}, {});

  // A non-array binCenters field.
  nlohmann::json binCentersNotArray = {
      {"histogram", {{"binCenters", "1.0, 2.0"}, {"counts", {1, 2}}}}};
  auto res = centered.FromJson(binCentersNotArray);
  ASSERT_FALSE(res.status().ok());
  EXPECT_THAT(res.status().message(), ::testing::HasSubstr("binCenters"));
  EXPECT_THAT(res.status().message(),
              ::testing::HasSubstr("expected array of numbers"));

  // A non-array counts field.
  nlohmann::json countsNotArray = {
      {"histogram", {{"binCenters", {1.0, 2.0}}, {"counts", 3}}}};
  res = centered.FromJson(countsNotArray);
  ASSERT_FALSE(res.status().ok());
  EXPECT_THAT(res.status().message(), ::testing::HasSubstr("counts"));

  // A non-number element inside binCenters.
  nlohmann::json badElement = {
      {"histogram", {{"binCenters", {1.0, "2.0"}}, {"counts", {1, 2}}}}};
  res = centered.FromJson(badElement);
  ASSERT_FALSE(res.status().ok());
  EXPECT_THAT(res.status().message(), ::testing::HasSubstr("binCenters"));

  auto edge = mdio::internal::EdgeDefinedHistogram<float>({}, {}, {});

  // A non-array binWidths field.
  nlohmann::json binWidthsNotArray = {
      {"histogram",
       {{"binEdges", {0.0, 1.0}}, {"binWidths", 1.0}, {"counts", {1}}}}};
  res = edge.FromJson(binWidthsNotArray);
  ASSERT_FALSE(res.status().ok());
  EXPECT_THAT(res.status().message(), ::testing::HasSubstr("binWidths"));

  // A non-number element inside counts.
  nlohmann::json badCountElement = {
      {"histogram",
       {{"binEdges", {0.0, 1.0}}, {"binWidths", {1.0}}, {"counts", {1, "2"}}}}};
  res = edge.FromJson(badCountElement);
  ASSERT_FALSE(res.status().ok());
  EXPECT_THAT(res.status().message(), ::testing::HasSubstr("counts"));
}

TEST(SummaryStatsTest, fromJsonEdgeHistogramAndMissingHistogram) {
  nlohmann::json edgeStats = {{"count", 50},
                              {"min", -500.0},
                              {"max", 500.0},
                              {"sum", 100.0},
                              {"sumSquares", 5000.0},
                              {"histogram",
                               {{"binEdges", {0.0, 1.0, 2.0}},
                                {"binWidths", {1.0, 1.0}},
                                {"counts", {10, 20}}}}};
  auto statsRes = mdio::internal::SummaryStats::FromJson(edgeStats);
  ASSERT_TRUE(statsRes.status().ok()) << statsRes.status();
  EXPECT_TRUE(statsRes.value().getBindable()["histogram"].contains("binEdges"));

  nlohmann::json noHist = {{"count", 100},
                           {"min", 0.0},
                           {"max", 100.0},
                           {"sum", 50.0},
                           {"sumSquares", 500.0}};
  EXPECT_FALSE(mdio::internal::SummaryStats::FromJson(noHist).status().ok());
}

TEST(UserAttributesTest, fromJsonWrongTypedStatsV1NamesTheField) {
  // The open path parses statsV1 through UserAttributes; a wrong-typed field
  // must surface as an error naming the field, not a generic malformed-JSON
  // message (or an uncaught exception for direct SummaryStats callers).
  nlohmann::json expected = {
      {"statsV1",
       {{"count", "100"},
        {"min", -1000.0},
        {"max", 1000.0},
        {"sum", 0.0},
        {"sumSquares", 0.0},
        {"histogram",
         {{"binCenters", {1.0, 2.0, 3.0}}, {"counts", {1, 2, 3}}}}}}};
  auto attrsRes = mdio::UserAttributes::FromJson(expected);
  ASSERT_FALSE(attrsRes.status().ok());
  EXPECT_THAT(attrsRes.status().message(), ::testing::HasSubstr("count"));
  EXPECT_THAT(attrsRes.status().message(),
              ::testing::HasSubstr("expected number"));
}

TEST(UserAttributesTest, fromVariableJson) {
  nlohmann::json centered = {
      {"name", "test"},
      {"dataType", "float32"},
      {"metadata",
       {{"statsV1",
         {{"count", 100},
          {"min", -10.0},
          {"max", 10.0},
          {"sum", 50.0},
          {"sumSquares", 500.0},
          {"histogram",
           {{"binCenters", {-5.0, 0.0, 5.0}}, {"counts", {30, 40, 30}}}}}}}}};
  auto centeredRes = mdio::UserAttributes::FromVariableJson(centered);
  ASSERT_TRUE(centeredRes.status().ok()) << centeredRes.status();
  EXPECT_EQ(centeredRes.value().ToJson()["statsV1"]["count"], 100);

  nlohmann::json edge = {{"name", "test"},
                         {"dataType", "float32"},
                         {"metadata",
                          {{"statsV1",
                            {{"count", 200},
                             {"min", 0.0},
                             {"max", 100.0},
                             {"sum", 5000.0},
                             {"sumSquares", 250000.0},
                             {"histogram",
                              {{"binEdges", {0.0, 50.0, 100.0}},
                               {"binWidths", {50.0, 50.0}},
                               {"counts", {100, 100}}}}}}}}};
  auto edgeRes = mdio::UserAttributes::FromVariableJson(edge);
  ASSERT_TRUE(edgeRes.status().ok()) << edgeRes.status();
  EXPECT_TRUE(
      edgeRes.value().ToJson()["statsV1"]["histogram"].contains("binEdges"));

  nlohmann::json noMeta = {{"name", "simple"}, {"dataType", "int32"}};
  auto noMetaRes = mdio::UserAttributes::FromVariableJson(noMeta);
  ASSERT_TRUE(noMetaRes.status().ok()) << noMetaRes.status();
  EXPECT_EQ(noMetaRes.value().ToJson(), nlohmann::json::object());
}

TEST(UserAttributesTest, accessors) {
  nlohmann::json full = {
      {"statsV1",
       {{"count", 100},
        {"min", -1000.0},
        {"max", 1000.0},
        {"sum", 0.0},
        {"sumSquares", 0.0},
        {"histogram", {{"binCenters", {1.0, 2.0}}, {"counts", {50, 50}}}}}},
      {"unitsV1", {{"length", "m"}, {"time", "s"}}},
      {"attributes", {{"foo", "bar"}, {"count", 42}}}};
  auto fullRes = mdio::UserAttributes::FromJson(full);
  ASSERT_TRUE(fullRes.status().ok()) << fullRes.status();
  auto attrs = fullRes.value();
  EXPECT_EQ(attrs.getStatsV1()["count"], 100);
  EXPECT_EQ(attrs.getUnitsV1().size(), 2);
  EXPECT_EQ(attrs.getAttrs()["foo"], "bar");

  nlohmann::json empty = nlohmann::json::object();
  auto emptyRes = mdio::UserAttributes::FromJson(empty);
  ASSERT_TRUE(emptyRes.status().ok());
  auto emptyAttrs = emptyRes.value();
  EXPECT_TRUE(emptyAttrs.getStatsV1().empty());
  EXPECT_TRUE(emptyAttrs.getUnitsV1().empty());
  EXPECT_TRUE(emptyAttrs.getAttrs().empty());
}

TEST(UserAttributesTest, statsV1AsArray) {
  nlohmann::json arrayStats = {
      {"name", "test"},
      {"metadata",
       {{"statsV1",
         {{{"count", 10},
           {"min", 0},
           {"max", 100},
           {"sum", 500},
           {"sumSquares", 25000},
           {"histogram", {{"binCenters", {1, 2, 3}}, {"counts", {3, 4, 3}}}}},
          {{"count", 20},
           {"min", 0},
           {"max", 200},
           {"sum", 1000},
           {"sumSquares", 50000},
           {"histogram",
            {{"binCenters", {1, 2, 3}}, {"counts", {6, 8, 6}}}}}}}}}};
  auto res = mdio::UserAttributes::FromVariableJson(arrayStats);
  ASSERT_TRUE(res.status().ok()) << res.status();
  EXPECT_EQ(res.value().ToJson()["statsV1"].size(), 2);
}

// ---------------------------------------------------------------------------
// ComputeStats / MergeStats
// ---------------------------------------------------------------------------

constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();

/// Builds a Zarr V3 variable spec (the format mdio-python defaults to).
/// `chunks` defaults to `shape`; Zarr V3 requires chunk entries >= 1, so
/// zero-length dimensions must pass an explicit unit chunk.
::nlohmann::json MakeVariableSpec(
    const std::string& name, const std::string& dataType,
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
  spec["attributes"]["long_name"] = "stats test variable";
  return spec;
}

/// Creates a variable at `name` and fills it with `values` in row-major order.
/// `values.size()` must equal the product of `shape`.
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
std::vector<float> SequentialValues(const float first, const std::size_t count) {
  std::vector<float> values;
  values.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    values.push_back(first + static_cast<float>(index));
  }
  return values;
}

std::unique_ptr<mdio::internal::CenteredBinHistogram<float>> MakeCenteredHist(
    const std::vector<float>& binCenters, const std::vector<int32_t>& counts) {
  return std::make_unique<mdio::internal::CenteredBinHistogram<float>>(
      binCenters, counts);
}

mdio::Result<mdio::internal::SummaryStats> MakePartial(
    const int32_t count, const float min, const float max, const float sum,
    const float sumSquares, const std::vector<float>& binCenters,
    const std::vector<int32_t>& counts) {
  return mdio::internal::SummaryStats::Create(count, max, min, sum, sumSquares,
                                              MakeCenteredHist(binCenters, counts));
}

TEST(ComputeStatsTest, KnownDistributionFloat32) {
  // Values 1..20: count 20, sum 210, sumSquares 2870, min 1, max 20. Ten
  // equal-width bins over [1, 20] hold exactly two values each.
  TENSORSTORE_ASSERT_OK_AND_ASSIGN(
      auto variable,
      MakePopulatedVariable<float>("stats_test_known_f32", "float32", {4, 5},
                                   SequentialValues(1.0F, 20)));
  auto statsRes = mdio::ComputeStats(variable);
  ASSERT_TRUE(statsRes.status().ok()) << statsRes.status();
  const auto stats = statsRes.value();

  EXPECT_EQ(stats.get_count(), 20);
  EXPECT_NEAR(stats.get_sum(), 210.0F, 1e-5F);
  EXPECT_NEAR(stats.get_sum_squares(), 2870.0F, 1e-3F);
  EXPECT_NEAR(stats.get_min(), 1.0F, 1e-6F);
  EXPECT_NEAR(stats.get_max(), 20.0F, 1e-6F);

  const nlohmann::json bindable = stats.getBindable();
  const std::vector<int32_t> expectedCounts(10, 2);
  EXPECT_EQ(bindable["histogram"]["counts"], expectedCounts);
  EXPECT_EQ(bindable["histogram"]["binCenters"].size(),
            mdio::internal::kDefaultHistogramBinCount);
}

TEST(ComputeStatsTest, KnownDistributionInt32) {
  TENSORSTORE_ASSERT_OK_AND_ASSIGN(
      auto variable,
      MakePopulatedVariable<int32_t>("stats_test_known_i32", "int32", {3},
                                     {10, 20, 30}));
  auto statsRes = mdio::ComputeStats(variable);
  ASSERT_TRUE(statsRes.status().ok()) << statsRes.status();
  const auto stats = statsRes.value();

  EXPECT_EQ(stats.get_count(), 3);
  EXPECT_NEAR(stats.get_sum(), 60.0F, 1e-6F);
  EXPECT_NEAR(stats.get_sum_squares(), 1400.0F, 1e-4F);
  EXPECT_NEAR(stats.get_min(), 10.0F, 1e-6F);
  EXPECT_NEAR(stats.get_max(), 30.0F, 1e-6F);
}

TEST(ComputeStatsTest, Float32AndFloat64AgreeOnExactValues) {
  const std::vector<float> values = {0.5F, 1.25F, -2.5F, 3.75F, 0.5F};
  TENSORSTORE_ASSERT_OK_AND_ASSIGN(
      auto f32Var,
      MakePopulatedVariable<float>("stats_test_f32", "float32", {5}, values));
  TENSORSTORE_ASSERT_OK_AND_ASSIGN(
      auto f64Var,
      MakePopulatedVariable<double>("stats_test_f64", "float64", {5},
                                    {0.5, 1.25, -2.5, 3.75, 0.5}));
  auto f32Res = mdio::ComputeStats(f32Var);
  ASSERT_TRUE(f32Res.status().ok()) << f32Res.status();
  auto f64Res = mdio::ComputeStats(f64Var);
  ASSERT_TRUE(f64Res.status().ok()) << f64Res.status();

  EXPECT_EQ(f32Res.value().get_count(), f64Res.value().get_count());
  EXPECT_NEAR(f32Res.value().get_sum(), f64Res.value().get_sum(), 1e-6F);
  EXPECT_NEAR(f32Res.value().get_sum_squares(),
              f64Res.value().get_sum_squares(), 1e-5F);
  EXPECT_NEAR(f32Res.value().get_min(), f64Res.value().get_min(), 1e-6F);
  EXPECT_NEAR(f32Res.value().get_max(), f64Res.value().get_max(), 1e-6F);
  EXPECT_EQ(f32Res.value().getBindable()["histogram"]["counts"],
            f64Res.value().getBindable()["histogram"]["counts"]);
}

TEST(ComputeStatsTest, EmptyVariableYieldsCanonicalEmptyStats) {
  // Zarr V3 forbids zero chunk entries, so the zero-length dimension uses a
  // unit chunk.
  TENSORSTORE_ASSERT_OK_AND_ASSIGN(
      auto variable,
      MakePopulatedVariable<float>("stats_test_empty", "float32", {0, 4}, {},
                                   {1, 4}));
  auto statsRes = mdio::ComputeStats(variable);
  ASSERT_TRUE(statsRes.status().ok()) << statsRes.status();
  const auto stats = statsRes.value();

  EXPECT_EQ(stats.get_count(), 0);
  EXPECT_EQ(stats.get_sum(), 0.0F);
  EXPECT_EQ(stats.get_sum_squares(), 0.0F);
  EXPECT_EQ(stats.get_min(), 0.0F);
  EXPECT_EQ(stats.get_max(), 0.0F);
  const nlohmann::json bindable = stats.getBindable();
  EXPECT_TRUE(bindable["histogram"]["binCenters"].empty());
  EXPECT_TRUE(bindable["histogram"]["counts"].empty());
}

TEST(ComputeStatsTest, AllNaNVariableYieldsCanonicalEmptyStats) {
  TENSORSTORE_ASSERT_OK_AND_ASSIGN(
      auto variable,
      MakePopulatedVariable<float>("stats_test_all_nan", "float32", {3},
                                   {kNaN, kNaN, kNaN}));
  auto statsRes = mdio::ComputeStats(variable);
  ASSERT_TRUE(statsRes.status().ok()) << statsRes.status();
  EXPECT_EQ(statsRes.value().get_count(), 0);
  EXPECT_EQ(statsRes.value().get_sum(), 0.0F);
}

TEST(ComputeStatsTest, NaNValuesAreSkipped) {
  TENSORSTORE_ASSERT_OK_AND_ASSIGN(
      auto variable,
      MakePopulatedVariable<float>("stats_test_nan", "float32", {4},
                                   {1.0F, kNaN, 3.0F, kNaN}));
  auto statsRes = mdio::ComputeStats(variable);
  ASSERT_TRUE(statsRes.status().ok()) << statsRes.status();
  const auto stats = statsRes.value();

  EXPECT_EQ(stats.get_count(), 2);
  EXPECT_NEAR(stats.get_sum(), 4.0F, 1e-6F);
  EXPECT_NEAR(stats.get_sum_squares(), 10.0F, 1e-5F);
  EXPECT_NEAR(stats.get_min(), 1.0F, 1e-6F);
  EXPECT_NEAR(stats.get_max(), 3.0F, 1e-6F);
}

TEST(ComputeStatsTest, ConstantVariableGetsSingleBin) {
  TENSORSTORE_ASSERT_OK_AND_ASSIGN(
      auto variable,
      MakePopulatedVariable<float>("stats_test_constant", "float32", {2, 2},
                                   {7.0F, 7.0F, 7.0F, 7.0F}));
  auto statsRes = mdio::ComputeStats(variable);
  ASSERT_TRUE(statsRes.status().ok()) << statsRes.status();
  const auto stats = statsRes.value();

  EXPECT_EQ(stats.get_count(), 4);
  EXPECT_NEAR(stats.get_sum(), 28.0F, 1e-6F);
  EXPECT_NEAR(stats.get_min(), 7.0F, 1e-6F);
  EXPECT_NEAR(stats.get_max(), 7.0F, 1e-6F);
  const nlohmann::json bindable = stats.getBindable();
  const std::vector<float> expectedCenters = {7.0F};
  const std::vector<int32_t> expectedCounts = {4};
  EXPECT_EQ(bindable["histogram"]["binCenters"], expectedCenters);
  EXPECT_EQ(bindable["histogram"]["counts"], expectedCounts);
}

TEST(ComputeStatsTest, DefaultBinsPutEdgeValuesInHigherBin) {
  // Values 0..10 with ten bins over [0, 10]: bin width is 1 and the bin
  // centers are 0.5 .. 9.5, so each value v lands in bin v — including the
  // edges: 1.0 sits exactly on the first midpoint and goes to the higher
  // bin, and the maximum 10 is clamped into the last bin.
  TENSORSTORE_ASSERT_OK_AND_ASSIGN(
      auto variable,
      MakePopulatedVariable<float>("stats_test_edges_default", "float32", {11},
                                   SequentialValues(0.0F, 11)));
  auto statsRes = mdio::ComputeStats(variable);
  ASSERT_TRUE(statsRes.status().ok()) << statsRes.status();

  const nlohmann::json bindable = statsRes.value().getBindable();
  const std::vector<float> expectedCenters = {0.5F, 1.5F, 2.5F, 3.5F, 4.5F,
                                              5.5F, 6.5F, 7.5F, 8.5F, 9.5F};
  const std::vector<int32_t> expectedCounts = {1, 1, 1, 1, 1, 1, 1, 1, 1, 2};
  EXPECT_EQ(bindable["histogram"]["binCenters"], expectedCenters);
  EXPECT_EQ(bindable["histogram"]["counts"], expectedCounts);
}

TEST(ComputeStatsTest, ExplicitBinCentersAssignByNearestCenter) {
  // Centers {0.5, 1.5, 2.5} put the midpoints at 1.0 and 2.0: values on a
  // midpoint go to the higher bin, values beyond the last center are
  // clamped into it.
  TENSORSTORE_ASSERT_OK_AND_ASSIGN(
      auto variable,
      MakePopulatedVariable<float>("stats_test_explicit", "float32", {6},
                                   {0.0F, 0.9F, 1.0F, 2.0F, 2.1F, 5.0F}));
  const std::vector<float> centers = {0.5F, 1.5F, 2.5F};
  auto statsRes = mdio::ComputeStats(variable, centers);
  ASSERT_TRUE(statsRes.status().ok()) << statsRes.status();
  const auto stats = statsRes.value();

  EXPECT_EQ(stats.get_count(), 6);
  EXPECT_NEAR(stats.get_sum(), 11.0F, 1e-6F);
  EXPECT_NEAR(stats.get_sum_squares(), 35.22F, 1e-4F);
  EXPECT_NEAR(stats.get_min(), 0.0F, 1e-6F);
  EXPECT_NEAR(stats.get_max(), 5.0F, 1e-6F);
  const nlohmann::json bindable = stats.getBindable();
  EXPECT_EQ(bindable["histogram"]["binCenters"], centers);
  const std::vector<int32_t> expectedCounts = {2, 1, 3};
  EXPECT_EQ(bindable["histogram"]["counts"], expectedCounts);
}

TEST(ComputeStatsTest, NonIncreasingBinCentersAreRejected) {
  TENSORSTORE_ASSERT_OK_AND_ASSIGN(
      auto variable,
      MakePopulatedVariable<float>("stats_test_bad_centers", "float32", {2},
                                   {1.0F, 2.0F}));
  const std::vector<float> decreasing = {2.0F, 1.0F};
  auto statsRes = mdio::ComputeStats(variable, decreasing);
  EXPECT_FALSE(statsRes.status().ok()) << statsRes.status();
}

TEST(ComputeStatsTest, UnsupportedDtypeIsRejected) {
  const std::string spec = R"(
      {
          "driver": "zarr",
          "kvstore": {"driver": "file", "path": "stats_test_struct"},
          "metadata": {
              "compressor": {"id": "blosc"},
              "dtype": [["a", "<i2"], ["b", "<i4"]],
              "shape": [10, 10],
              "chunks": [3, 2],
              "dimension_separator": "/",
              "zarr_format": 2
          },
          "attributes": {
              "dimension_names": ["x", "y"],
              "long_name": "struct"
          }
      }
  )";
  TENSORSTORE_ASSERT_OK_AND_ASSIGN(
      auto variable,
      mdio::Variable<>::Open(::nlohmann::json::parse(spec),
                             mdio::constants::kCreateClean)
          .result());
  auto statsRes = mdio::ComputeStats(variable);
  EXPECT_FALSE(statsRes.status().ok()) << statsRes.status();
}

TEST(MergeStatsTest, PartialsEqualSinglePassComputation) {
  // Whole variable holds 1..24; the partials hold disjoint partitions
  // (1..12, 13..18, 19..24) binned with the whole's bin centers.
  TENSORSTORE_ASSERT_OK_AND_ASSIGN(
      auto whole,
      MakePopulatedVariable<float>("stats_test_merge_whole", "float32", {4, 6},
                                   SequentialValues(1.0F, 24)));
  TENSORSTORE_ASSERT_OK_AND_ASSIGN(
      auto partA,
      MakePopulatedVariable<float>("stats_test_merge_a", "float32", {2, 6},
                                   SequentialValues(1.0F, 12)));
  TENSORSTORE_ASSERT_OK_AND_ASSIGN(
      auto partB,
      MakePopulatedVariable<float>("stats_test_merge_b", "float32", {1, 6},
                                   SequentialValues(13.0F, 6)));
  TENSORSTORE_ASSERT_OK_AND_ASSIGN(
      auto partC,
      MakePopulatedVariable<float>("stats_test_merge_c", "float32", {1, 6},
                                   SequentialValues(19.0F, 6)));

  auto singleRes = mdio::ComputeStats(whole);
  ASSERT_TRUE(singleRes.status().ok()) << singleRes.status();
  const auto single = singleRes.value();
  ASSERT_EQ(single.get_count(), 24);
  ASSERT_NEAR(single.get_sum(), 300.0F, 1e-5F);

  const std::vector<float> centers =
      single.getBindable()["histogram"]["binCenters"]
          .get<std::vector<float>>();

  auto partialARes = mdio::ComputeStats(partA, centers);
  ASSERT_TRUE(partialARes.status().ok()) << partialARes.status();
  auto partialBRes = mdio::ComputeStats(partB, centers);
  ASSERT_TRUE(partialBRes.status().ok()) << partialBRes.status();
  auto partialCRes = mdio::ComputeStats(partC, centers);
  ASSERT_TRUE(partialCRes.status().ok()) << partialCRes.status();

  const std::vector<mdio::internal::SummaryStats> partials = {
      partialARes.value(), partialBRes.value(), partialCRes.value()};
  auto mergedRes = mdio::MergeStats(absl::MakeConstSpan(partials));
  ASSERT_TRUE(mergedRes.status().ok()) << mergedRes.status();
  const auto merged = mergedRes.value();

  EXPECT_EQ(merged.get_count(), single.get_count());
  EXPECT_NEAR(merged.get_sum(), single.get_sum(), 1e-5F);
  EXPECT_NEAR(merged.get_sum_squares(), single.get_sum_squares(), 1e-3F);
  EXPECT_NEAR(merged.get_min(), single.get_min(), 1e-6F);
  EXPECT_NEAR(merged.get_max(), single.get_max(), 1e-6F);
  EXPECT_EQ(merged.getBindable()["histogram"]["binCenters"],
            single.getBindable()["histogram"]["binCenters"]);
  EXPECT_EQ(merged.getBindable()["histogram"]["counts"],
            single.getBindable()["histogram"]["counts"]);
}

TEST(MergeStatsTest, PermutationsOfPartialsGiveSameResult) {
  TENSORSTORE_ASSERT_OK_AND_ASSIGN(
      auto partA,
      MakePopulatedVariable<float>("stats_test_perm_a", "float32", {2, 6},
                                   SequentialValues(1.0F, 12)));
  TENSORSTORE_ASSERT_OK_AND_ASSIGN(
      auto partB,
      MakePopulatedVariable<float>("stats_test_perm_b", "float32", {1, 6},
                                   SequentialValues(13.0F, 6)));
  TENSORSTORE_ASSERT_OK_AND_ASSIGN(
      auto partC,
      MakePopulatedVariable<float>("stats_test_perm_c", "float32", {1, 6},
                                   SequentialValues(19.0F, 6)));
  const std::vector<float> centers = {2.15F, 4.45F, 6.75F, 9.05F, 11.35F,
                                      13.65F, 15.95F, 18.25F, 20.55F, 22.85F};

  auto partialARes = mdio::ComputeStats(partA, centers);
  ASSERT_TRUE(partialARes.status().ok()) << partialARes.status();
  auto partialBRes = mdio::ComputeStats(partB, centers);
  ASSERT_TRUE(partialBRes.status().ok()) << partialBRes.status();
  auto partialCRes = mdio::ComputeStats(partC, centers);
  ASSERT_TRUE(partialCRes.status().ok()) << partialCRes.status();

  const std::vector<mdio::internal::SummaryStats> order123 = {
      partialARes.value(), partialBRes.value(), partialCRes.value()};
  const std::vector<mdio::internal::SummaryStats> order321 = {
      partialCRes.value(), partialBRes.value(), partialARes.value()};
  const std::vector<mdio::internal::SummaryStats> order213 = {
      partialBRes.value(), partialARes.value(), partialCRes.value()};

  auto merged123Res = mdio::MergeStats(absl::MakeConstSpan(order123));
  ASSERT_TRUE(merged123Res.status().ok()) << merged123Res.status();
  auto merged321Res = mdio::MergeStats(absl::MakeConstSpan(order321));
  ASSERT_TRUE(merged321Res.status().ok()) << merged321Res.status();
  auto merged213Res = mdio::MergeStats(absl::MakeConstSpan(order213));
  ASSERT_TRUE(merged213Res.status().ok()) << merged213Res.status();

  // Integer-valued data keeps every partial sum exact in float32, so the
  // merged results are bitwise identical in any order.
  EXPECT_EQ(merged123Res.value().getBindable(),
            merged321Res.value().getBindable());
  EXPECT_EQ(merged123Res.value().getBindable(),
            merged213Res.value().getBindable());
}

TEST(MergeStatsTest, DefaultBinnedPartialsCannotMerge) {
  // Each partition derives its own bin centers from its own range, so the
  // binnings disagree and the merge must fail loudly.
  TENSORSTORE_ASSERT_OK_AND_ASSIGN(
      auto partA,
      MakePopulatedVariable<float>("stats_test_mismatch_a", "float32", {2, 6},
                                   SequentialValues(1.0F, 12)));
  TENSORSTORE_ASSERT_OK_AND_ASSIGN(
      auto partB,
      MakePopulatedVariable<float>("stats_test_mismatch_b", "float32", {1, 6},
                                   SequentialValues(13.0F, 6)));
  auto partialARes = mdio::ComputeStats(partA);
  ASSERT_TRUE(partialARes.status().ok()) << partialARes.status();
  auto partialBRes = mdio::ComputeStats(partB);
  ASSERT_TRUE(partialBRes.status().ok()) << partialBRes.status();

  const std::vector<mdio::internal::SummaryStats> partials = {
      partialARes.value(), partialBRes.value()};
  auto mergedRes = mdio::MergeStats(absl::MakeConstSpan(partials));
  EXPECT_FALSE(mergedRes.status().ok()) << mergedRes.status();
}

TEST(MergeStatsTest, EmptySpanIsRejected) {
  const std::vector<mdio::internal::SummaryStats> noPartials;
  auto mergedRes = mdio::MergeStats(absl::MakeConstSpan(noPartials));
  EXPECT_FALSE(mergedRes.status().ok()) << mergedRes.status();
}

TEST(MergeStatsTest, NeutralPartialsContributeNothing) {
  auto realRes = MakePartial(5, 1.0F, 10.0F, 25.0F, 225.0F, {2.0F, 4.0F},
                             {3, 2});
  ASSERT_TRUE(realRes.status().ok()) << realRes.status();
  auto neutralRes = MakePartial(0, 0.0F, 0.0F, 0.0F, 0.0F, {}, {});
  ASSERT_TRUE(neutralRes.status().ok()) << neutralRes.status();

  const std::vector<mdio::internal::SummaryStats> withNeutral = {
      neutralRes.value(), realRes.value()};
  auto mergedRes = mdio::MergeStats(absl::MakeConstSpan(withNeutral));
  ASSERT_TRUE(mergedRes.status().ok()) << mergedRes.status();
  EXPECT_EQ(mergedRes.value().getBindable(), realRes.value().getBindable());

  // A merge of only neutral partials is the canonical empty statsV1.
  const std::vector<mdio::internal::SummaryStats> onlyNeutral = {
      neutralRes.value(), neutralRes.value()};
  auto emptyRes = mdio::MergeStats(absl::MakeConstSpan(onlyNeutral));
  ASSERT_TRUE(emptyRes.status().ok()) << emptyRes.status();
  EXPECT_EQ(emptyRes.value().get_count(), 0);
  EXPECT_TRUE(emptyRes.value().getBindable()["histogram"]["binCenters"]
                  .empty());
}

TEST(MergeStatsTest, EmptyHistogramPartialsMergeScalarsOnly) {
  // mdio-python imports carry empty histograms; merging them keeps the
  // histogram empty while the scalars combine.
  auto firstRes = MakePartial(4, 1.0F, 8.0F, 18.0F, 130.0F, {}, {});
  ASSERT_TRUE(firstRes.status().ok()) << firstRes.status();
  auto secondRes = MakePartial(6, 2.0F, 12.0F, 42.0F, 394.0F, {}, {});
  ASSERT_TRUE(secondRes.status().ok()) << secondRes.status();

  const std::vector<mdio::internal::SummaryStats> partials = {
      firstRes.value(), secondRes.value()};
  auto mergedRes = mdio::MergeStats(absl::MakeConstSpan(partials));
  ASSERT_TRUE(mergedRes.status().ok()) << mergedRes.status();
  const auto merged = mergedRes.value();

  EXPECT_EQ(merged.get_count(), 10);
  EXPECT_NEAR(merged.get_sum(), 60.0F, 1e-5F);
  EXPECT_NEAR(merged.get_min(), 1.0F, 1e-6F);
  EXPECT_NEAR(merged.get_max(), 12.0F, 1e-6F);
  EXPECT_TRUE(merged.getBindable()["histogram"]["binCenters"].empty());
}

TEST(MergeStatsTest, MixedHistogramAndNonHistogramIsRejected) {
  auto withHistRes = MakePartial(5, 1.0F, 10.0F, 25.0F, 225.0F, {2.0F, 4.0F},
                                 {3, 2});
  ASSERT_TRUE(withHistRes.status().ok()) << withHistRes.status();
  auto withoutHistRes = MakePartial(4, 1.0F, 8.0F, 18.0F, 130.0F, {}, {});
  ASSERT_TRUE(withoutHistRes.status().ok()) << withoutHistRes.status();

  const std::vector<mdio::internal::SummaryStats> partials = {
      withHistRes.value(), withoutHistRes.value()};
  auto mergedRes = mdio::MergeStats(absl::MakeConstSpan(partials));
  EXPECT_FALSE(mergedRes.status().ok()) << mergedRes.status();
}

TEST(MergeStatsTest, EdgeDefinedHistogramsMergeCounts) {
  const std::vector<float> binEdges = {0.0F, 10.0F, 20.0F};
  const std::vector<float> binWidths = {10.0F, 10.0F};
  auto firstRes = mdio::internal::SummaryStats::Create(
      5, 18.0F, 2.0F, 50.0F, 700.0F,
      std::make_unique<mdio::internal::EdgeDefinedHistogram<float>>(
          binEdges, binWidths, std::vector<int32_t>{3, 2}));
  ASSERT_TRUE(firstRes.status().ok()) << firstRes.status();
  auto secondRes = mdio::internal::SummaryStats::Create(
      4, 12.0F, 1.0F, 30.0F, 400.0F,
      std::make_unique<mdio::internal::EdgeDefinedHistogram<float>>(
          binEdges, binWidths, std::vector<int32_t>{1, 3}));
  ASSERT_TRUE(secondRes.status().ok()) << secondRes.status();

  const std::vector<mdio::internal::SummaryStats> partials = {
      firstRes.value(), secondRes.value()};
  auto mergedRes = mdio::MergeStats(absl::MakeConstSpan(partials));
  ASSERT_TRUE(mergedRes.status().ok()) << mergedRes.status();
  const auto merged = mergedRes.value();

  EXPECT_EQ(merged.get_count(), 9);
  const nlohmann::json bindable = merged.getBindable();
  EXPECT_TRUE(bindable["histogram"].contains("binEdges"));
  const std::vector<int32_t> expectedCounts = {4, 5};
  EXPECT_EQ(bindable["histogram"]["counts"], expectedCounts);
}

TEST(ComputeStatsTest, PublishingFlowRoundTrip) {
  const std::string path = "stats_test_publishing";
  std::filesystem::remove_all(path);
  const std::string schema = R"(
      {
        "metadata": {
          "name": "statsPublishingTest",
          "apiVersion": "1.0.0",
          "createdOn": "2026-09-16T00:00:00.000000+00:00"
        },
        "variables": [
          {
            "name": "data",
            "dataType": "float32",
            "dimensions": [
              {"name": "inline", "size": 4},
              {"name": "crossline", "size": 3}
            ]
          },
          {
            "name": "inline",
            "dataType": "int32",
            "dimensions": [{"name": "inline", "size": 4}]
          },
          {
            "name": "crossline",
            "dataType": "int32",
            "dimensions": [{"name": "crossline", "size": 3}]
          }
        ]
      })";
  ::nlohmann::json schemaJson = ::nlohmann::json::parse(schema);
  auto datasetRes = mdio::Dataset::from_json(schemaJson, path,
                                             mdio::constants::kCreateClean);
  ASSERT_TRUE(datasetRes.status().ok()) << datasetRes.status();
  auto dataset = datasetRes.value();

  TENSORSTORE_ASSERT_OK_AND_ASSIGN(
      auto dataVar, dataset.variables.get<mdio::dtypes::float32_t>("data"));
  TENSORSTORE_ASSERT_OK_AND_ASSIGN(auto dataData,
                        mdio::from_variable<mdio::dtypes::float32_t>(dataVar));
  auto accessor = dataData.get_data_accessor();
  // Values 1..12: count 12, sum 78, sumSquares 650, min 1, max 12.
  for (int i = 0; i < 4; ++i) {
    for (int j = 0; j < 3; ++j) {
      accessor({i, j}) = static_cast<float>(i * 3 + j + 1);
    }
  }
  auto writeFutures = dataVar.Write(dataData);
  writeFutures.commit_future.Wait();
  ASSERT_TRUE(writeFutures.copy_future.status().ok())
      << writeFutures.copy_future.status();

  // Publishing flow: compute, merge into current attributes, update, commit.
  auto statsRes = mdio::ComputeStats(dataVar);
  ASSERT_TRUE(statsRes.status().ok()) << statsRes.status();
  const auto stats = statsRes.value();
  ASSERT_EQ(stats.get_count(), 12);

  nlohmann::json attrs = dataVar.GetAttributes();
  attrs["statsV1"] = stats.getBindable();
  auto updateRes = dataVar.UpdateAttributes(attrs);
  ASSERT_TRUE(updateRes.status().ok()) << updateRes.status();

  auto commitRes = dataset.CommitMetadata();
  ASSERT_TRUE(commitRes.status().ok()) << commitRes.status();

  // Reopen and read the statsV1 back from durable media.
  auto reopenedRes = mdio::Dataset::Open(path, mdio::constants::kOpen);
  ASSERT_TRUE(reopenedRes.status().ok()) << reopenedRes.status();
  TENSORSTORE_ASSERT_OK_AND_ASSIGN(auto reopenedVar,
                        reopenedRes.value().variables.at("data"));
  nlohmann::json reopenedAttrs = reopenedVar.GetAttributes();
  ASSERT_TRUE(reopenedAttrs.contains("statsV1")) << reopenedAttrs;
  const nlohmann::json& statsV1 = reopenedAttrs["statsV1"];

  EXPECT_EQ(statsV1["count"], 12);
  EXPECT_NEAR(statsV1["sum"].get<float>(), 78.0F, 1e-4F);
  EXPECT_NEAR(statsV1["sumSquares"].get<float>(), 650.0F, 1e-3F);
  EXPECT_NEAR(statsV1["min"].get<float>(), 1.0F, 1e-6F);
  EXPECT_NEAR(statsV1["max"].get<float>(), 12.0F, 1e-6F);
  const std::vector<int32_t> expectedCounts = {2, 1, 1, 1, 1, 1, 1, 1, 1, 2};
  EXPECT_EQ(statsV1["histogram"]["counts"], expectedCounts);
}

}  // namespace
