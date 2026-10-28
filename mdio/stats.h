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

#ifndef MDIO_STATS_H_
#define MDIO_STATS_H_

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/types/span.h"
#include "mdio/impl.h"
#include "tensorstore/tensorstore.h"

// clang-format off
#include <nlohmann/json.hpp>  // NOLINT
// clang-format on

/**
 * The intention of this extensible data class is to provide a representation of
 * the UserAttributes that COULD be modified. It is intended to be completely
 * immutable after construction. The reasoning is that this data is already
 * written to durable media and if the wrong values are changed the data could
 * be corrupted. This is a safety measure to prevent that. Note that
 * modifications are not durable and will need to be committed from the parent
 * Dataset object. Great care must be taken when committing the changes and
 * should only be done once. While there are multiple construction options, the
 * only one intended for user interaction is
 * `mdio::UserAttributes::FromJson(const nlohmann::json)`. The UserAttributes
 * that is associated with the Variable should be reassigned to a new
 * UserAttributes object iff the object NEEDS to be modified. A normal usecase
 * should NOT require constant modification of the UserAttributes object.
 *
 * The end-user should NEVER need to enter the internal namespace or access the
 * UserAttributes constructor directly. They should instead use the static
 * member function FromJson. If a UserAttributes object is in need of
 * modification it should be done as follows
 *
 * @code
 * // NOTE: We do not verify the status of `dataset.variables.at("variable")` in
 * this example code. This is for brevity.
 * mdio::UserAttributes userAttrs =
 *                         dataset.variables.at("variable").value().userAttrs;
 * nlohmann::json updatedAttrs = userAttrs.ToJson();
 * // Modify updatedAttrs as any normal JSON object
 * // NOTE: FromJson can take an optional template of `int32_t` default to
 * `float` to specify the type of the histogram.
 * auto updatedUserAttrsResult = mdio::UserAttributes::FromJson(updatedAttrs);
 * if (!updatedUserAttrsResult.ok()) {
 *   // Handle error
 * }
 * mdio::UserAttributes updatedUserAttrs = updatedUserAttrsResult.value();
 * dataset.variables.get("variable").value().userAttrs = updatedUserAttrs;
 * @endcode
 *
 * Another thing to note is that we historically did not supply an easy way to
 * add a histogram or attributes to an existing UserAttributes object. That
 * policy was deliberately reverted by the statsV1 computation milestone of the
 * API gap plan (see docs/api-gap-plan.md, "M4 — Statistics"): the intended
 * path is now `mdio::ComputeStats` to compute a statsV1 (histogram included),
 * merged into the variable's current attribute JSON, and published through the
 * existing `VariableBase::UpdateAttributes` + `Dataset::CommitMetadata` flow.
 * See `mdio::ComputeStats` for the full publishing example.
 */

namespace mdio {
namespace internal {

/**
 * @brief Type-checks a scalar statsV1 field before conversion.
 *
 * nlohmann converts any JSON number to any arithmetic field type (by cast),
 * but throws an uncaught type_error on anything else — and the exception
 * escapes the open path for direct SummaryStats callers. Wrong-typed fields
 * are rejected with a named error instead. Numbers keep converting exactly
 * as before: this rejects only what nlohmann would throw on, so the
 * accepted format is unchanged.
 *
 * @param value The JSON value of the field.
 * @param context The error-message prefix naming what is being parsed.
 * @param field The field name, for the error message.
 * @return OkStatus, or InvalidArgumentError naming the field and the
 *     expected vs. found type.
 */
inline absl::Status CheckScalarFieldType(const nlohmann::json& value,
                                         const std::string& context,
                                         const std::string& field) {
  if (!value.is_number()) {
    return absl::InvalidArgumentError(
        context + "\n\tField '" + field +
        "' has wrong type: expected number, got " +
        std::string(value.type_name()));
  }
  return absl::OkStatus();
}

/**
 * @brief Type-checks an array statsV1 field and its elements before
 * conversion.
 *
 * Same contract as CheckScalarFieldType, for vector fields: nlohmann's
 * implicit vector conversion throws on a non-array field or a non-number
 * element; wrong-typed fields are rejected with a named error instead, and
 * the accepted format is unchanged.
 *
 * @param value The JSON value of the field.
 * @param context The error-message prefix naming what is being parsed.
 * @param field The field name, for the error message.
 * @return OkStatus, or InvalidArgumentError naming the field and the
 *     expected vs. found type.
 */
inline absl::Status CheckArrayFieldType(const nlohmann::json& value,
                                        const std::string& context,
                                        const std::string& field) {
  if (!value.is_array()) {
    return absl::InvalidArgumentError(
        context + "\n\tField '" + field +
        "' has wrong type: expected array of numbers, got " +
        std::string(value.type_name()));
  }
  std::size_t index = 0;
  for (const auto& element : value) {
    if (!element.is_number()) {
      return absl::InvalidArgumentError(
          context + "\n\tField '" + field +
          "' has wrong type: expected array of numbers, but element " +
          std::to_string(index) + " is " + std::string(element.type_name()));
    }
    ++index;
  }
  return absl::OkStatus();
}

/**
 * @brief A Histogram can be either CenteredBinHistogram or EdgeDefinedHistogram
 * as defined by the MDIO spec
 */
class Histogram {
 public:
  virtual ~Histogram() = default;
  virtual nlohmann::json getHistogram() const = 0;
  virtual std::unique_ptr<const Histogram> clone() const = 0;
  virtual mdio::Result<std::unique_ptr<const Histogram>> FromJson(
      const nlohmann::json& j) const = 0;
  /**
   * @brief Notifier for whether or not the histogram is bindable
   * There is an instance where the histogram is not bindable, such as when we
   * need a placeholder internally.
   * @return True if the histogram is MDIO bindable, flase otherwise.
   */
  virtual bool isBindable() const = 0;

  const std::string HIST_KEY = "histogram";

 private:
  /**
   * @brief Implementation specific method to determine if the JSON object is a
   * histogram
   * @param j The JSON object that may be a histogram
   * @return True if the JSON is a histogram, false otherwise
   */
  virtual bool isHist(const nlohmann::json& j) const = 0;
};

template <typename T = float>
class CenteredBinHistogram : public Histogram {
 public:
  CenteredBinHistogram(const std::vector<T>& binCenters,
                       const std::vector<int32_t>& counts)
      : binCenters(binCenters), counts(counts) {
    static_assert(std::is_same_v<T, float> || std::is_same_v<T, int32_t>,
                  "Histograms may only be float32 or int32_t.");
  }

  mdio::Result<std::unique_ptr<const Histogram>> FromJson(
      const nlohmann::json& j) const override {
    if (isHist(j)) {
      auto histogram = j[HIST_KEY];
      if (histogram.contains("binCenters") && histogram.contains("counts")) {
        // Type-check before converting: nlohmann's implicit vector
        // conversion throws an uncaught exception on a non-array field or a
        // non-number element, which escapes the open path.
        const std::string context =
            "Error parsing histogram:\n\tType detected: CenteredBinHistogram";
        for (const auto& field : {"binCenters", "counts"}) {
          auto status = CheckArrayFieldType(histogram[field], context, field);
          if (!status.ok()) {
            return status;
          }
        }
        std::vector<T> binCenters = j[HIST_KEY]["binCenters"];
        std::vector<int32_t> counts = j[HIST_KEY]["counts"];
        auto hist =
            std::make_unique<CenteredBinHistogram<T>>(binCenters, counts);
        return mdio::Result<std::unique_ptr<const Histogram>>(std::move(hist));
      }
      return absl::InvalidArgumentError(
          "Error parsing histogram:\n\tType detected: "
          "CenteredBinHistogram\n\tMissing child key: 'binCenters' or "
          "'counts'");
    }
    return absl::InvalidArgumentError(
        "Error parsing histogram:\n\tType detected: "
        "CenteredBinHistogram\n\tMissing parent key: '" +
        HIST_KEY + ";");
  }

  nlohmann::json getHistogram() const override {
    nlohmann::json histogram;
    histogram[HIST_KEY]["binCenters"] = this->binCenters;
    histogram[HIST_KEY]["counts"] = this->counts;
    return histogram;
  }

  std::unique_ptr<const Histogram> clone() const override {
    return std::make_unique<CenteredBinHistogram>(*this);
  }

  bool isBindable() const override { return true; }

 private:
  const std::vector<T> binCenters;
  const std::vector<int32_t> counts;

  bool isHist(const nlohmann::json& j) const override {
    return j.contains(HIST_KEY);
  }
};

template <typename T = float>
class EdgeDefinedHistogram : public Histogram {
 public:
  EdgeDefinedHistogram(const std::vector<T>& binEdges,
                       const std::vector<T>& binWidths,
                       const std::vector<int32_t>& counts)
      : binEdges(binEdges), binWidths(binWidths), counts(counts) {
    static_assert(std::is_same_v<T, float> || std::is_same_v<T, int32_t>,
                  "Histograms may only be float32 or int32_t.");
  }

  /**
   * @brief Attempts to construct a new EdgeDefinedHistogram from a JSON
   * representation
   * @param j The JSON representation of the histogram
   * @return A new EdgeDefinedHistogram if the input JSON is valid, otherwise an
   * error
   */
  mdio::Result<std::unique_ptr<const Histogram>> FromJson(
      const nlohmann::json& j) const override {
    if (isHist(j)) {
      auto histogram = j[HIST_KEY];
      if (histogram.contains("binEdges") && histogram.contains("binWidths") &&
          histogram.contains("counts")) {
        // Type-check before converting: nlohmann's implicit vector
        // conversion throws an uncaught exception on a non-array field or a
        // non-number element, which escapes the open path.
        const std::string context =
            "Error parsing histogram:\n\tType detected: EdgeDefinedHistogram";
        for (const auto& field : {"binEdges", "binWidths", "counts"}) {
          auto status = CheckArrayFieldType(histogram[field], context, field);
          if (!status.ok()) {
            return status;
          }
        }
        std::vector<T> binEdges = j[HIST_KEY]["binEdges"];
        std::vector<T> binWidths = j[HIST_KEY]["binWidths"];
        std::vector<int32_t> counts = j[HIST_KEY]["counts"];
        auto hist = std::make_unique<EdgeDefinedHistogram<T>>(
            binEdges, binWidths, counts);
        return mdio::Result<std::unique_ptr<const Histogram>>(std::move(hist));
      }
      // TODO(BrianMichell): Provide better descriptive error message here.
      return absl::InvalidArgumentError(
          "Error parsing histogram:\n\tType detected: "
          "EdgeDefinedHistogram\n\tMissing child key: 'binEdges', "
          "'binWidths', or 'counts'");
    }
    return absl::InvalidArgumentError(
        "Error parsing histogram:\n\tType detected: "
        "EdgeDefinedHistogram\n\tMissing parent key: 'histogram'");
  }

  nlohmann::json getHistogram() const override {
    nlohmann::json histogram;
    histogram[HIST_KEY]["binEdges"] = this->binEdges;
    histogram[HIST_KEY]["binWidths"] = this->binWidths;
    histogram[HIST_KEY]["counts"] = this->counts;
    return histogram;
  }

  std::unique_ptr<const Histogram> clone() const override {
    return std::make_unique<EdgeDefinedHistogram>(*this);
  }

  bool isBindable() const override { return true; }

 private:
  const std::vector<T> binEdges;
  const std::vector<T> binWidths;
  const std::vector<int32_t> counts;

  bool isHist(const nlohmann::json& j) const override {
    return j.contains(HIST_KEY);
  }
};

class SummaryStats {
 public:
  SummaryStats(const SummaryStats& other)
      : count(other.count),
        max(other.max),
        min(other.min),
        sum(other.sum),
        sumSquares(other.sumSquares),
        histogram(other.histogram->clone()) {}

  /**
   * @brief Constructs a SummaryStats from computed values.
   * This is the construction path for freshly computed statistics (e.g.
   * `mdio::ComputeStats` and `mdio::MergeStats`); parsing persisted statsV1
   * JSON goes through `FromJson` instead.
   * @param count The number of data points.
   * @param max The largest value in the variable.
   * @param min The smallest value in the variable.
   * @param sum The total of all data values.
   * @param sumSquares The total of all data values squared.
   * @param histogram The binned frequency distribution. Must not be null.
   * @return A SummaryStats, or an error if the histogram is null.
   */
  static mdio::Result<SummaryStats> Create(
      const int32_t count, const float max, const float min, const float sum,
      const float sumSquares, std::unique_ptr<const Histogram> histogram) {
    if (histogram == nullptr) {
      return absl::InvalidArgumentError(
          "SummaryStats requires a histogram (may be empty, but not null).");
    }
    return mdio::Result<SummaryStats>(SummaryStats(
        count, max, min, sum, sumSquares, std::move(histogram)));
  }

  /// @brief The number of data points.
  int32_t get_count() const { return count; }
  /// @brief The largest value in the variable.
  float get_max() const { return max; }
  /// @brief The smallest value in the variable.
  float get_min() const { return min; }
  /// @brief The total of all data values.
  float get_sum() const { return sum; }
  /// @brief The total of all data values squared.
  float get_sum_squares() const { return sumSquares; }
  /// @brief The binned frequency distribution.
  const std::unique_ptr<const Histogram>& get_histogram() const {
    return histogram;
  }

  const nlohmann::json getBindable() const {
    nlohmann::json stats = this->histogram->getHistogram();
    stats["count"] = this->count;
    stats["max"] = this->max;
    stats["min"] = this->min;
    stats["sum"] = this->sum;
    stats["sumSquares"] = this->sumSquares;
    return stats;
  }

  /**
   * @brief Constructs a statsV1 object from JSON
   * It is assumed to be a singleton of a statsV1 object.
   * This should only be used internally
   * @tparam T Type of the summary histogram
   * @param j The JSON of a statsV1 object
   * @return A result of the constructed summary stats
   */
  template <typename T = float>
  static mdio::Result<SummaryStats> FromJson(const nlohmann::json j) {
    auto histRes = constructHist<T>(j);
    if (!histRes.status().ok()) {
      return histRes.status();
    }
    auto histogram = std::move(histRes.value());
    const std::array<std::string, 5> keys = {"count", "max", "min", "sum",
                                             "sumSquares"};
    for (const auto& key : keys) {
      if (!j.contains(key)) {
        return absl::InvalidArgumentError(
            "Error parsing statsV1:\n\tMissing key: '" + key + "'");
      }
    }
    // Type-check before converting: get<int32_t>/get<float> throw an
    // uncaught exception on a non-number, which escapes the open path.
    for (const auto& key : keys) {
      auto status = CheckScalarFieldType(j[key], "Error parsing statsV1:", key);
      if (!status.ok()) {
        return status;
      }
    }
    auto stats =
        SummaryStats(j["count"].get<int32_t>(), j["max"].get<float>(),
                     j["min"].get<float>(), j["sum"].get<float>(),
                     j["sumSquares"].get<float>(), std::move(histogram));
    return mdio::Result<SummaryStats>(stats);
  }

 private:
  SummaryStats(const int32_t count, const float max, const float min,
               const float sum, const float sumSquares,
               std::unique_ptr<const Histogram> histogram)
      : count(count),
        max(max),
        min(min),
        sum(sum),
        sumSquares(sumSquares),
        histogram(std::move(histogram)) {}
  /**
   * @brief A type agnostic Histogram factory method
   * @tparam T Type of the histogram (Default: float)
   * @param stats The statsV1 JSON. Expects a root of "statsV1"
   * @return A unique pointer to a Histogram object or error result if the JSON
   * is invalid
   */
  template <typename T = float>
  static mdio::Result<std::unique_ptr<const Histogram>> constructHist(
      const nlohmann::json& stats) {
    if (!stats.contains("histogram")) {
      return absl::InvalidArgumentError(
          "Error parsing histogram:\n\tMissing parent key: 'histogram'");
    }
    std::unique_ptr<const Histogram> histogram;
    if (stats["histogram"].contains("binCenters") &&
        stats["histogram"].contains("counts")) {
      auto inertHist = CenteredBinHistogram<T>({}, {});
      auto res = inertHist.FromJson(stats);
      if (!res.status().ok()) {
        return res.status();
      }
      histogram = std::move(res.value());
    } else if (stats["histogram"].contains("binEdges") &&
               stats["histogram"].contains("binWidths") &&
               stats["histogram"].contains("counts")) {
      auto inertHist = EdgeDefinedHistogram<T>({}, {}, {});
      auto res = inertHist.FromJson(stats);
      if (!res.status().ok()) {
        return res.status();
      }
      histogram = std::move(res.value());
    } else {
      // This should never be true
      return absl::InvalidArgumentError(
          "Could not deduce the type of the provided histogram.");
    }
    return histogram;
  }

  const int32_t count;
  const float max;
  const float min;
  const float sum;
  const float sumSquares;
  const std::unique_ptr<const Histogram> histogram;
};

/**
 * @brief Incremental accumulator for the scalar fields of a statsV1.
 *
 * Accumulates in double precision regardless of the variable's dtype so the
 * per-partial float32 rounding happens exactly once, at the end. Shared by
 * the ComputeStats dtype dispatch paths.
 */
struct StatsAccumulator {
  int64_t count = 0;
  double sum = 0.0;
  double sumSquares = 0.0;
  double min = 0.0;
  double max = 0.0;

  void Add(const double value) {
    if (count == 0) {
      min = value;
      max = value;
    } else {
      min = std::min(min, value);
      max = std::max(max, value);
    }
    sum += value;
    sumSquares += value * value;
    ++count;
  }
};

/**
 * @brief Converts a double accumulator to the float32 statsV1 field type.
 *
 * The statsV1 schema stores float32 fields; a non-finite or out-of-range
 * value would serialize as null (invalid JSON for the schema), so it is
 * rejected here instead of being corrupted at persistence time.
 * @param value The accumulated value.
 * @param field The statsV1 field name, used in the error message.
 * @return The float32 value, or an error if it is not representable.
 */
inline mdio::Result<float> CheckedFloat(const double value,
                                        const std::string& field) {
  if (!std::isfinite(value) ||
      std::fabs(value) >
          static_cast<double>(std::numeric_limits<float>::max())) {
    return absl::InvalidArgumentError(
        "statsV1 field '" + field +
        "' is not representable in the float32 range required by the "
        "schema: " +
        std::to_string(value));
  }
  return static_cast<float>(value);
}

/// The number of histogram bins ComputeStats derives from the data when the
/// caller does not supply explicit bin centers.
inline constexpr std::size_t kDefaultHistogramBinCount = 10;

/**
 * @brief Bin centers for `binCount` equal-width bins spanning
 * [minValue, maxValue].
 * @pre minValue < maxValue (a constant variable is binned into a single bin
 * by the caller instead).
 */
inline std::vector<float> UniformBinCenters(const double minValue,
                                            const double maxValue,
                                            const std::size_t binCount) {
  std::vector<float> centers;
  centers.reserve(binCount);
  const double width = (maxValue - minValue) / static_cast<double>(binCount);
  for (std::size_t index = 0; index < binCount; ++index) {
    centers.push_back(static_cast<float>(
        minValue + (static_cast<double>(index) + 0.5) * width));
  }
  return centers;
}

/**
 * @brief Upper edges (midpoints between adjacent bin centers) used for bin
 * assignment.
 *
 * With equal-width bins these midpoints are the bin edges, so a value exactly
 * on a midpoint belongs to the higher bin — matching half-open
 * [edge, next_edge) bins over the data range.
 */
inline std::vector<double> BinUpperEdges(
    const absl::Span<const float> centers) {
  std::vector<double> edges;
  if (centers.size() < 2) {
    return edges;  // A single bin holds every value.
  }
  edges.reserve(centers.size() - 1);
  for (std::size_t index = 0; index + 1 < centers.size(); ++index) {
    edges.push_back((static_cast<double>(centers[index]) +
                     static_cast<double>(centers[index + 1])) /
                    2.0);
  }
  return edges;
}

/**
 * @brief Index of the bin a value belongs to, given BinUpperEdges output.
 *
 * Values below the first edge land in bin 0, values at or above the last edge
 * land in the last bin, and a value exactly on an edge goes to the higher
 * bin.
 */
inline std::size_t BinIndexForValue(const double value,
                                    const std::vector<double>& upperEdges) {
  const auto edge =
      std::upper_bound(upperEdges.begin(), upperEdges.end(), value);
  return static_cast<std::size_t>(edge - upperEdges.begin());
}

}  // namespace internal

class UserAttributes {
 public:
  /**
   * @brief Copy constructor
   * @param other The UserAttributes object to copy
   * @note This constructor is intended for internal use only. Please use the
   * static member function `FromJson(nlohmann::json)`
   * @code
   * // j is some valid nlohmann::json object
   * auto attrs = UserAttributes::FromJson(j).value();
   * nlohmann::json newAttrs = attrs.ToJson();
   * newAttrs["attributes"]["newKey"] = "newValue";
   * auto newAttrsRes = UserAttributes::FromJson(newAttrs).value();
   * @endcode
   */
  UserAttributes(const UserAttributes& other)
      : stats(other.stats), units(other.units), attrs(other.attrs) {}

  /**
   * @brief Constructs a UserAttributes object from a JSON representation of a
   * Dataset. This is intended to function as an automated factory method for
   * Dataset construction. It should NEVER be invoked manually. Instead use the
   * static member function `FromJson(nlohmann::json)`.
   * @tparam T The type of the histogram (May either be flaot or int32_t)
   * (Default: float)
   * @param j The JSON representation of the dataset
   * @param varible The name of the Variable which may contain the user
   * attributes
   * @return A UserAttributes object if the variable is a Variable in the
   * Dataset, otherwise an error
   * @pre The input JSON must be validated according to the MDIO schema. This is
   * unchecked and may have undefined behavior if not followed.
   */
  static mdio::Result<UserAttributes> FromDatasetJson(
      const nlohmann::json& dataset, const std::string& variable) {
    for (auto& var : dataset["variables"]) {
      if (var["name"] == variable) {
        auto param = var.contains("metadata") ? var["metadata"]
                                              : nlohmann::json::object();
        if (inferIsFloat(param)) {
          return FromJson<float>(param);
        } else {
          return FromJson<int32_t>(param);
        }
      }
    }
    return absl::InvalidArgumentError("Variable " + variable +
                                      " not found in Dataset");
  }

  static mdio::Result<UserAttributes> FromVariableJson(
      const nlohmann::json& variable) {
    auto param = variable.contains("metadata") ? variable["metadata"]
                                               : nlohmann::json::object();
    if (inferIsFloat(param)) {
      return FromJson<float>(param);
    } else {
      return FromJson<int32_t>(param);
    }
  }

  /**
   * @brief Constructs a UserAttributes object from the JSON representation of a
   * UserAttributes
   * @tparam T The type of the histogram (May either be flaot or int32_t)
   * (Default: float)
   * @param j The JSON representation of the UserAttributes
   * @return A UserAttributes object matching the input JSON
   */
  template <typename T = float>
  static mdio::Result<UserAttributes> FromJson(const nlohmann::json& j) {
    // Because the user can supply JSON here, there's a chance that the JSON is
    // malformed.
    try {
      if (j.contains("statsV1") || j.contains("unitsV1")) {
        std::vector<internal::SummaryStats> statsCollection;
        if (j.contains("statsV1")) {
          auto statsJson = j["statsV1"];
          // statsV1 may be stored as a serialized JSON string in some
          // third-party Zarr stores; parse it if needed.
          auto parse_if_string =
              [](const nlohmann::json& val) -> mdio::Result<nlohmann::json> {
            if (val.is_string()) {
              try {
                return nlohmann::json::parse(val.get<std::string>());
              } catch (const std::exception& e) {
                return absl::InvalidArgumentError(
                    std::string("Error parsing statsV1 string: ") + e.what());
              }
            }
            return val;
          };
          if (statsJson.is_array()) {
            for (auto& s : statsJson) {
              MDIO_ASSIGN_OR_RETURN(auto parsed, parse_if_string(s));
              auto statsRes = internal::SummaryStats::FromJson<T>(parsed);
              if (!statsRes.status().ok()) {
                return statsRes.status();
              }
              statsCollection.emplace_back(statsRes.value());
            }
          } else {
            MDIO_ASSIGN_OR_RETURN(auto parsed, parse_if_string(statsJson));
            auto statsRes = internal::SummaryStats::FromJson<T>(parsed);
            if (!statsRes.status().ok()) {
              return statsRes.status();
            }
            statsCollection.emplace_back(statsRes.value());
          }
        }
        std::vector<std::string> unitsCollection;
        if (j.contains("unitsV1")) {
          auto unitsJson = j["unitsV1"];
          if (unitsJson.is_array()) {
            for (auto& s : unitsJson) {
              if (s.is_object()) {
                // If the element is an object, iterate its key-value pairs.
                for (auto& kv : s.items()) {
                  unitsCollection.push_back(kv.value().get<std::string>());
                }
              } else {
                unitsCollection.push_back(s.get<std::string>());
              }
            }
          } else if (unitsJson.is_object()) {
            // If unitsV1 itself is an object, iterate its key-value pairs.
            for (auto& kv : unitsJson.items()) {
              unitsCollection.push_back(kv.value().get<std::string>());
            }
          } else {
            unitsCollection.push_back(unitsJson.get<std::string>());
          }
        }
        auto attrs =
            UserAttributes(statsCollection, unitsCollection,
                           j.contains("attributes") ? j["attributes"]
                                                    : nlohmann::json::object());
        return mdio::Result<UserAttributes>(attrs);
      } else if (j.contains("attributes")) {
        auto attrs = UserAttributes(j["attributes"]);
        return mdio::Result<UserAttributes>(attrs);
      }
      auto attrs = UserAttributes(nlohmann::json::object());
      return mdio::Result<UserAttributes>(attrs);
    } catch (const nlohmann::json::exception& e) {
      return absl::InvalidArgumentError(
          "There appeared to be some malformed JSON" + std::string(e.what()));
    } catch (const std::exception& e) {
      return absl::InternalError("An unexpected error occurred: " +
                                 std::string(e.what()));
    }
  }

  /**
   * @brief Extracts just the statsV1 JSON
   * @return The statsV1 JSON representation of the data
   */
  const nlohmann::json getStatsV1() const { return statsBindable(); }

  /**
   * @brief Extracts just the unitsV1 JSON
   * @return The unitsV1 JSON representation of the data
   */
  const nlohmann::json getUnitsV1() const { return unitsBindable(); }

  /**
   * @brief Extracts just the attributes JSON
   * @return The attributes JSON representation of the data
   */
  const nlohmann::json getAttrs() const { return attrsBindable(); }

  /**
   * @brief Gets the JSON representation of a UserAttributes object
   * @return An nlohamnn json object
   */
  const nlohmann::json ToJson() const {
    nlohmann::json j = nlohmann::json::object();
    if (stats.size() >= 1) {
      j["statsV1"] = statsBindable();
    }
    if (units.size() >= 1) {
      j["unitsV1"] = unitsBindable();
    }
    auto attrs = attrsBindable();
    if (attrs.empty()) {
      return j;
    }
    j["attributes"] = attrs;
    return j;
  }

 private:
  /**
   * @brief A case where there are attributes but no statsV1 object.
   * @param attrs User specified attributes
   * @note This constructor is intended for internal use only. Please use the
   * static member function `FromJson(nlohmann::json)`
   */
  explicit UserAttributes(const nlohmann::json& attrs)
      : attrs(attrs), stats({}), units({}) {}

  /**
   * @brief A case where there are statsV1 objects but no attributes
   * @param stats A collection of SummaryStats objects
   * @param attrs User specified attributes
   * @note This constructor is intended for internal use only. Please use the
   * static member function `FromJson(nlohmann::json)`
   */
  UserAttributes(const std::vector<internal::SummaryStats>& stats,
                 const nlohmann::json attrs)
      : stats(stats), units({}), attrs(attrs) {}

  /**
   * @brief A case where there are unitsV1 objects but no attributes
   * @param units A collection of SummaryStats objects
   * @param attrs User specified attributes
   * @note This constructor is intended for internal use only. Please use the
   * static member function `FromJson(nlohmann::json)`
   */
  UserAttributes(const std::vector<std::string>& units,
                 const nlohmann::json attrs)
      : units(units), attrs(attrs) {}

  /**
   * @brief A case where there are both statsV1 and unitsV1 objects
   * @param stats A collection of SummaryStats objects
   * @param units A collection of SummaryStats objects
   * @param attrs User specified attributes
   * @note This constructor is intended for internal use only. Please use the
   * static member function `FromJson(nlohmann::json)`
   */
  UserAttributes(const std::vector<internal::SummaryStats>& stats,
                 const std::vector<std::string>& units,
                 const nlohmann::json attrs)
      : stats(stats), units(units), attrs(attrs) {}

  /**
   * @brief Binds the existing statsV1 data to a JSON object
   * @return A bindable statsV1 JSON object
   */
  const nlohmann::json statsBindable() const {
    if (stats.size() == 0) {
      return nlohmann::json::object();
    } else if (stats.size() == 1) {
      return stats[0].getBindable();
    }
    nlohmann::json statsRet = nlohmann::json::array();
    for (auto& stat : stats) {
      statsRet.emplace_back(stat.getBindable());
    }
    return statsRet;
  }

  /**
   * @brief Binds the existing unitsV1 data to a JSON object
   * @return A bindable unitsV1 JSON object
   */
  const nlohmann::json unitsBindable() const {
    if (units.empty()) {
      return nlohmann::json::object();
    } else if (units.size() == 1) {
      return units[0];
    }
    nlohmann::json unitsRet = nlohmann::json::array();
    for (const auto& unit : units) {
      unitsRet.push_back(unit);
    }
    return unitsRet;
  }

  /**
   * @brief Binds the existing attributes data to a JSON object
   * @return A bindable attributes JSON object
   */
  const nlohmann::json attrsBindable() const {
    if (!(attrs.empty())) {
      return attrs;
    }
    return nlohmann::json::object();
  }

  /**
   * @brief Attempts to infer if the JSON object should be a float or int32_t
   * Assumption: A Histogram will be float and can only be an integer if no
   * decimal values are detected anywhere inside of it. Assumption: If there is
   * a statsV1 array then the histogram is a float.
   * @param j The JSON object to infer the type of
   * @return True if the JSON object is infered to be a float
   */
  static bool inferIsFloat(const nlohmann::json& j) {
    if (j.contains("statsV1")) {
      if (j["statsV1"].is_array()) {
        return true;  // Assumption #2
      }
      if (j["statsV1"].contains("histogram")) {
        for (const auto& val : j["statsV1"]["histogram"]) {
          if (val.is_number_float() ||
              (val.is_number_integer() && val != static_cast<int>(val))) {
            return true;  // Assumption #1
          }
        }
        return false;
      }
    }
    return true;  // Default to float if no histogram is provided
  }

  std::vector<internal::SummaryStats> stats;
  std::vector<std::string> units;
  const nlohmann::json attrs;
};

// Variable is defined in variable.h, which includes this header; it can only
// be forward declared here, so the ComputeStats declarations spell out the
// dtype-erased Variable<void, dynamic_rank, ReadWriteMode::dynamic> instead
// of using its default template arguments.
template <typename T, DimensionIndex R, ReadWriteMode M>
struct Variable;

/**
 * @brief Computes the statsV1 summary statistics of a variable.
 *
 * Reads the whole variable and produces the on-disk statsV1 contract: an
 * int32 count, float32 sum/sumSquares/min/max, and a CenteredBinHistogram
 * with `internal::kDefaultHistogramBinCount` bins derived from the data
 * range. NaN values are skipped (they are "missing", not values); a variable
 * with no data yields the canonical empty statsV1 (count 0, zeroed fields,
 * empty histogram), matching the mdio-python convention.
 *
 * Supported dtypes are float32, float64, and the signed/unsigned integer
 * types; anything else (e.g. float16, complex, struct arrays) is rejected.
 *
 * \b Publishing: the result is not persisted by this call. The flow is
 * ComputeStats, merge the statsV1 JSON into the variable's current
 * attributes, update, then commit for durability:
 * @code
 * MDIO_ASSIGN_OR_RETURN(auto stats, mdio::ComputeStats(variable));
 * nlohmann::json attrs = variable.GetAttributes();
 * attrs["statsV1"] = stats.getBindable();
 * MDIO_RETURN_IF_ERROR(variable.UpdateAttributes(attrs));
 * MDIO_RETURN_IF_ERROR(dataset.CommitMetadata().status());
 * @endcode
 * `UpdateAttributes` replaces the whole UserAttributes, so start from
 * `GetAttributes()` to preserve any existing attributes/units. Caveat
 * (measured downstream, api-gap-plan M4 acceptance): for some store
 * layouts this round-trip nests the attributes key one level deeper per
 * commit; for such layouts use the narrow form
 * `UpdateAttributes({{"statsV1", stats.getBindable()}})` — it still
 * replaces the whole UserAttributes, so include `unitsV1`/`attributes`
 * alongside when they must survive.
 *
 * @param var The variable to compute statistics over.
 * @return The summary statistics, or an error for an unsupported dtype or a
 * value that the float32 statsV1 fields cannot represent.
 */
Result<internal::SummaryStats> ComputeStats(
    const Variable<void, dynamic_rank, ReadWriteMode::dynamic>& var);

/**
 * @brief Computes the statsV1 summary statistics of a variable with explicit
 * histogram bin centers.
 *
 * Identical to the single-argument overload, except the histogram uses the
 * supplied bin centers instead of deriving them from the data range. Values
 * are assigned to the nearest bin center; a value exactly between two
 * centers goes to the higher one, and values outside the center range are
 * clamped to the first/last bin.
 *
 * This is the overload distributed callers should use: partials computed
 * with the same explicit bin centers can be combined with MergeStats, while
 * partials that each derive bins from their own sub-range cannot (their bin
 * centers will not agree).
 *
 * @param var The variable to compute statistics over.
 * @param binCenters The bin centers of the histogram. Must be non-empty and
 * strictly increasing.
 * @return The summary statistics, or an error for invalid bin centers, an
 * unsupported dtype, or an unrepresentable value.
 */
Result<internal::SummaryStats> ComputeStats(
    const Variable<void, dynamic_rank, ReadWriteMode::dynamic>& var,
    absl::Span<const float> binCenters);

/**
 * @brief Order-independent combination of partial statsV1 results.
 *
 * Combines partials computed over disjoint subsets of a variable (e.g. by
 * distributed callers) into the statsV1 of the whole: counts and sums add up,
 * min/max fold, and histograms merge by summing per-bin counts.
 *
 * Histogram compatibility: every partial that carries data must either carry
 * no histogram at all (empty binning, the mdio-python import convention) or
 * carry a histogram with exactly the same binning (bin centers for centered
 * histograms, bin edges and widths for edge-defined ones). Mixing histogram
 * and non-histogram partials, or mismatched binnings, is an error — partials
 * cannot be rebinned from counts alone. Use the explicit-bin-centers
 * ComputeStats overload to produce compatible partials.
 *
 * Partials with a count of zero are neutral and contribute nothing (their
 * zeroed min/max would otherwise poison the fold). If every partial is
 * neutral, the result is the canonical empty statsV1.
 *
 * @param partials The partial results to combine, in any order.
 * @return The combined summary statistics, or an error if no partial is
 * supplied, the binnings are incompatible, or a total does not fit the
 * statsV1 field types.
 */
inline Result<internal::SummaryStats> MergeStats(
    absl::Span<const internal::SummaryStats> partials) {
  if (partials.empty()) {
    return absl::InvalidArgumentError(
        "MergeStats requires at least one partial.");
  }

  int64_t count = 0;
  double sum = 0.0;
  double sumSquares = 0.0;
  bool haveData = false;
  double minValue = 0.0;
  double maxValue = 0.0;

  // The first data-carrying partial with a non-empty histogram defines the
  // reference binning; every other data-carrying partial must match it.
  nlohmann::json referenceHistogram;
  std::vector<int64_t> mergedCounts;
  bool haveHistogram = false;

  for (const auto& partial : partials) {
    if (partial.get_count() == 0) {
      continue;  // Neutral partial: contributes nothing.
    }

    count += partial.get_count();
    sum += partial.get_sum();
    sumSquares += partial.get_sum_squares();
    if (!haveData) {
      minValue = partial.get_min();
      maxValue = partial.get_max();
      haveData = true;
    } else {
      minValue = std::min(minValue, static_cast<double>(partial.get_min()));
      maxValue = std::max(maxValue, static_cast<double>(partial.get_max()));
    }

    const auto& histogram = partial.get_histogram();
    if (histogram == nullptr) {
      return absl::InvalidArgumentError(
          "MergeStats: partial carries no histogram.");
    }
    nlohmann::json histogramJson = histogram->getHistogram()["histogram"];

    const bool isCentered = histogramJson.contains("binCenters");
    const bool isEdge = histogramJson.contains("binEdges");
    if (!isCentered && !isEdge) {
      return absl::InvalidArgumentError(
          "MergeStats: partial carries an unrecognized histogram shape.");
    }

    // A histogram is "empty" when all of its arrays are empty (the
    // mdio-python convention for "no histogram"); some-but-not-all empty
    // arrays are malformed.
    const nlohmann::json& countsJson = histogramJson["counts"];
    bool allEmpty = countsJson.empty();
    bool anyEmpty = countsJson.empty();
    const std::vector<std::string> binningKeys =
        isCentered ? std::vector<std::string>{"binCenters"}
                   : std::vector<std::string>{"binEdges", "binWidths"};
    for (const auto& key : binningKeys) {
      allEmpty = allEmpty && histogramJson[key].empty();
      anyEmpty = anyEmpty || histogramJson[key].empty();
    }
    if (anyEmpty && !allEmpty) {
      return absl::InvalidArgumentError(
          "MergeStats: partial carries a malformed histogram (some binning "
          "arrays are empty and others are not).");
    }

    if (allEmpty) {
      if (haveHistogram) {
        return absl::InvalidArgumentError(
            "MergeStats: cannot merge a partial without a histogram into "
            "partials that have one.");
      }
      continue;
    }

    if (!haveHistogram) {
      referenceHistogram = histogramJson;
      mergedCounts.reserve(countsJson.size());
      for (const auto& binCount : countsJson) {
        mergedCounts.push_back(binCount.get<int64_t>());
      }
      haveHistogram = true;
      continue;
    }

    const bool sameKind =
        isCentered == referenceHistogram.contains("binCenters");
    bool sameBinning = sameKind;
    if (sameKind && isCentered) {
      sameBinning =
          histogramJson["binCenters"] == referenceHistogram["binCenters"];
    } else if (sameKind) {
      sameBinning = histogramJson["binEdges"] ==
                        referenceHistogram["binEdges"] &&
                    histogramJson["binWidths"] ==
                        referenceHistogram["binWidths"];
    }
    if (!sameBinning) {
      return absl::InvalidArgumentError(
          "MergeStats: partials have incompatible histogram binnings; all "
          "partials must share identical bin centers (or edges and widths). "
          "Use the ComputeStats overload with explicit bin centers to "
          "produce compatible partials.");
    }
    if (countsJson.size() != mergedCounts.size()) {
      return absl::InvalidArgumentError(
          "MergeStats: partial histogram counts length does not match the "
          "reference binning.");
    }
    for (std::size_t index = 0; index < mergedCounts.size(); ++index) {
      mergedCounts[index] += countsJson[index].get<int64_t>();
    }
  }

  if (!haveData) {
    // Every partial was neutral: the canonical empty statsV1.
    return internal::SummaryStats::Create(
        0, 0.0F, 0.0F, 0.0F, 0.0F,
        std::make_unique<internal::CenteredBinHistogram<float>>(
            std::vector<float>(), std::vector<int32_t>()));
  }

  if (count > static_cast<int64_t>(std::numeric_limits<int32_t>::max())) {
    return absl::InvalidArgumentError(
        "MergeStats: total count " + std::to_string(count) +
        " exceeds the int32 statsV1 count field.");
  }
  MDIO_ASSIGN_OR_RETURN(const float sumValue,
                        internal::CheckedFloat(sum, "sum"));
  MDIO_ASSIGN_OR_RETURN(const float sumSquaresValue,
                        internal::CheckedFloat(sumSquares, "sumSquares"));
  MDIO_ASSIGN_OR_RETURN(const float minValueF,
                        internal::CheckedFloat(minValue, "min"));
  MDIO_ASSIGN_OR_RETURN(const float maxValueF,
                        internal::CheckedFloat(maxValue, "max"));

  std::unique_ptr<const internal::Histogram> histogram;
  if (haveHistogram) {
    // Per-bin counts cannot overflow int32: each is at most the total count.
    const std::vector<int32_t> counts(mergedCounts.begin(), mergedCounts.end());
    if (referenceHistogram.contains("binCenters")) {
      const std::vector<float> binCenters =
          referenceHistogram["binCenters"].get<std::vector<float>>();
      histogram = std::make_unique<internal::CenteredBinHistogram<float>>(
          binCenters, counts);
    } else {
      const std::vector<float> binEdges =
          referenceHistogram["binEdges"].get<std::vector<float>>();
      const std::vector<float> binWidths =
          referenceHistogram["binWidths"].get<std::vector<float>>();
      histogram = std::make_unique<internal::EdgeDefinedHistogram<float>>(
          binEdges, binWidths, counts);
    }
  } else {
    histogram = std::make_unique<internal::CenteredBinHistogram<float>>(
        std::vector<float>(), std::vector<int32_t>());
  }

  return internal::SummaryStats::Create(static_cast<int32_t>(count),
                                        maxValueF, minValueF, sumValue,
                                        sumSquaresValue, std::move(histogram));
}

}  // namespace mdio
#endif  // MDIO_STATS_H_
