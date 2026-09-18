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

#ifndef MDIO_TRANSFORM_H_
#define MDIO_TRANSFORM_H_

#include <cstddef>
#include <memory>
#include <string_view>
#include <utility>

#include "absl/functional/any_invocable.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "mdio/impl.h"
#include "mdio/variable.h"
#include "mdio/zarr/zarr.h"
#include "tensorstore/array.h"
#include "tensorstore/tensorstore.h"
#include "tensorstore/util/future.h"

// clang-format off
#include <nlohmann/json.hpp>  // NOLINT
// clang-format on

namespace mdio {

/**
 * @brief Element-wise transform applied during a dtype-erased transfer.
 *
 * Receives one element of the source variable and the buffer for the
 * corresponding element of the destination variable, both as raw bytes:
 * @p src_bytes spans the source element (the dtype itemsize, or the whole
 * record for a structured dtype opened as its void byte view) and @p dst_bytes
 * spans the destination element. The transform reads @p src_bytes and writes
 * the result through @p dst_bytes (its @c data() points at a mutable buffer
 * of @c dst_bytes.size() bytes).
 *
 * The two spans may have different sizes: source and destination dtypes may
 * differ, and the transform defines the mapping between them.
 *
 * @param src_bytes The source element, as raw bytes.
 * @param dst_bytes The destination element buffer, as raw bytes.
 * @return The number of bytes written to @p dst_bytes, which must equal
 *     @c dst_bytes.size(); or an error that aborts the transfer. A reported
 *     count that does not match the destination element size fails the
 *     transfer with an error naming the destination variable, so a wrong
 *     count cannot overflow or under-fill the destination buffer silently.
 */
using ElementTransform = absl::AnyInvocable<absl::StatusOr<std::size_t>(
    std::string_view src_bytes, std::string_view dst_bytes) const>;

namespace internal {

/// The flat element layout an ElementTransform sees for one variable.
struct ElementLayout {
  /// Bytes per element: the dtype itemsize, or the whole record size for a
  /// structured dtype opened as its void byte view.
  std::size_t element_size = 0;
  /// Number of elements in the variable's domain.
  std::size_t num_elements = 0;
};

/**
 * @brief Derives the element layout of a variable's data as a flat buffer.
 *
 * A scalar-dtype variable maps element-wise: each element is one dtype item
 * (`dtype().size()` bytes). A structured dtype opened as the void byte view
 * (rank N+1, trailing byte axis) maps record-wise: each element is the whole
 * record, `itemsize` bytes. A field view of a structured dtype (a field was
 * selected at open) is scalar over the field's dtype.
 *
 * @param var The variable to describe.
 * @return The element layout, or an error if the spec carries no metadata.
 */
inline Result<ElementLayout> GetElementLayout(const Variable<>& var) {
  auto spec_result = var.get_spec();
  if (!spec_result.ok()) {
    return spec_result.status();
  }
  const nlohmann::json& spec = spec_result.value();
  if (!spec.contains("metadata")) {
    return absl::InvalidArgumentError(absl::StrCat(
        "Could not derive the element layout of variable ",
        var.get_variable_name(), ": its spec carries no metadata."));
  }
  const nlohmann::json& metadata = spec["metadata"];
  // The void byte view of a structured dtype has one more dimension (the
  // byte axis) than the metadata shape; a field view keeps the metadata rank.
  const bool structured_view =
      zarr::IsStructuredDType(zarr::GetVersionFromSpec(spec), metadata) &&
      var.dtype() == constants::kByte && metadata.contains("shape") &&
      var.rank() == metadata["shape"].size() + 1;
  ElementLayout layout;
  if (structured_view) {
    // One element is the whole record: the trailing dimension's size.
    const auto shape = var.dimensions().shape();
    layout.element_size = static_cast<std::size_t>(shape[shape.size() - 1]);
    layout.num_elements =
        layout.element_size == 0
            ? 0
            : static_cast<std::size_t>(var.num_samples()) / layout.element_size;
  } else {
    layout.element_size = var.dtype().size();
    layout.num_elements = static_cast<std::size_t>(var.num_samples());
  }
  return layout;
}

/**
 * @brief Checks that src and dst pair element-wise.
 *
 * Requires the same rank and the same size on every dimension (origins may
 * differ: the write aligns the two domains by translation) and the same
 * element count. The element count is what rejects pairing a scalar variable
 * with a structured variable of the same shape: [4, 5, 8] float32 is 160
 * elements, [4, 5, 8] bytes over a 4-byte record is 40.
 *
 * @param src The source variable.
 * @param dst The destination variable.
 * @param src_layout The source element layout.
 * @param dst_layout The destination element layout.
 * @return OkStatus, or an InvalidArgumentError naming the mismatch.
 */
inline absl::Status CheckTransformCompatibility(
    const Variable<>& src, const Variable<>& dst,
    const ElementLayout& src_layout, const ElementLayout& dst_layout) {
  if (src.rank() != dst.rank()) {
    return absl::InvalidArgumentError(absl::StrCat(
        "TransformVariable requires the same rank: variable ",
        src.get_variable_name(), " has rank ", src.rank(), " but variable ",
        dst.get_variable_name(), " has rank ", dst.rank(), "."));
  }
  const auto src_shape = src.dimensions().shape();
  const auto dst_shape = dst.dimensions().shape();
  for (std::size_t dim = 0; dim < src_shape.size(); ++dim) {
    if (src_shape[dim] != dst_shape[dim]) {
      return absl::InvalidArgumentError(absl::StrCat(
          "TransformVariable requires the same shape: dimension ", dim,
          " of variable ", src.get_variable_name(), " has size ",
          src_shape[dim], " but dimension ", dim, " of variable ",
          dst.get_variable_name(), " has size ", dst_shape[dim], "."));
    }
  }
  if (src_layout.num_elements != dst_layout.num_elements) {
    return absl::InvalidArgumentError(absl::StrCat(
        "TransformVariable requires the same element count: variable ",
        src.get_variable_name(), " has ", src_layout.num_elements,
        " elements of ", src_layout.element_size, " bytes but variable ",
        dst.get_variable_name(), " has ", dst_layout.num_elements,
        " elements of ", dst_layout.element_size,
        " bytes; a scalar variable and a structured variable of the same "
        "shape do not pair element-wise."));
  }
  return absl::OkStatus();
}

/**
 * @brief Applies the transform to every element of the flat source buffer.
 *
 * Elements are visited sequentially in row-major order. The first error
 * aborts the pass; the destination buffer may then hold partial output, but
 * TransformVariable never submits it to the store. The byte count each call
 * reports is validated against the destination element size: a transform
 * reporting a different count than the destination element holds would
 * overflow or under-fill the buffer, so it fails the pass.
 *
 * @param src_data The flat source buffer (C-contiguous).
 * @param dst_data The flat destination buffer (C-contiguous).
 * @param src_layout The source element layout.
 * @param dst_layout The destination element layout.
 * @param dst_name The destination variable name, for error messages.
 * @param fn The element transform.
 * @return OkStatus, or the first error: an error from @p fn, or the
 *     byte-count contract violation above.
 */
inline absl::Status ApplyElementTransform(const void* src_data, void* dst_data,
                                          const ElementLayout& src_layout,
                                          const ElementLayout& dst_layout,
                                          const std::string& dst_name,
                                          const ElementTransform& fn) {
  const char* src_cursor = static_cast<const char*>(src_data);
  char* dst_cursor = static_cast<char*>(dst_data);
  for (std::size_t index = 0; index < src_layout.num_elements; ++index) {
    absl::StatusOr<std::size_t> written =
        fn(std::string_view(src_cursor, src_layout.element_size),
           std::string_view(dst_cursor, dst_layout.element_size));
    if (!written.ok()) {
      return written.status();
    }
    if (written.value() != dst_layout.element_size) {
      return absl::InternalError(
          absl::StrCat("The element transform for variable '", dst_name,
                       "' reported writing ", written.value(),
                       " bytes but the destination element holds ",
                       dst_layout.element_size, " bytes."));
    }
    src_cursor += src_layout.element_size;
    dst_cursor += dst_layout.element_size;
  }
  return absl::OkStatus();
}

/**
 * @brief Transforms a completed read result into the destination variable.
 *
 * Allocates the destination array (the destination's domain and dtype),
 * applies the transform element-wise, and submits the write. The promise is
 * completed with the transform's first error — in which case no write is
 * submitted and the destination keeps its prior contents — or with the
 * write's commit result. The destination array is captured by the commit
 * callback and stays alive until the write commits.
 *
 * @param dst The destination variable.
 * @param src_layout The source element layout.
 * @param dst_layout The destination element layout.
 * @param fn The element transform.
 * @param promise The promise completing TransformVariable's future.
 * @param src_array The completed read of the source variable.
 */
inline void TransformAndWrite(
    const Variable<>& dst, const ElementLayout& src_layout,
    const ElementLayout& dst_layout, const ElementTransform& fn,
    tensorstore::Promise<absl::Status> promise,
    const SharedArray<void, dynamic_rank, offset_origin>& src_array) {
  auto dst_array = tensorstore::AllocateArray(
      dst.get_store().domain().box(), mdio::ContiguousLayoutOrder::c,
      tensorstore::value_init, dst.dtype());
  absl::Status transformed = ApplyElementTransform(
      src_array.byte_strided_origin_pointer().get(),
      dst_array.byte_strided_origin_pointer().get(), src_layout, dst_layout,
      dst.get_variable_name(), fn);
  if (!transformed.ok()) {
    // fn failed: no write is submitted, so dst keeps its prior contents.
    promise.SetResult(std::move(transformed));
    return;
  }
  tensorstore::Write(dst_array, dst.get_store())
      .commit_future.ExecuteWhenReady(
          [promise = std::move(promise), dst_array = std::move(dst_array)](
              tensorstore::ReadyFuture<void> readyWrite) {
            auto write_result = readyWrite.result();
            if (!write_result.ok()) {
              promise.SetResult(write_result.status());
              return;
            }
            // Success is the future's VALUE: Result<absl::Status> rejects an
            // ok Status in its error-argument form, so build it in place.
            promise.SetResult(tensorstore::Result<absl::Status>(
                std::in_place, absl::OkStatus()));
          });
}

}  // namespace internal

/**
 * @brief Transfers src into dst, transforming one element at a time,
 * dtype-erased.
 *
 * Reads all of src, applies @p fn to every element (sequentially, in
 * row-major order, on the thread that completes the read), and writes the
 * result to dst. Source and destination dtypes may differ; @p fn defines the
 * mapping.
 *
 * For a structured dtype opened as its void byte view, the unit is the whole
 * record: @p fn receives the record's `itemsize` bytes and may rewrite any
 * field. For a scalar dtype (including a field view of a structured dtype),
 * the unit is one dtype item.
 *
 * The whole region is transferred in one read-transform-write pass, so memory
 * use scales with the variable's size. For a bounded-memory chunked
 * transfer, slice both variables first (slices are lvalues; dst is taken by
 * non-const reference):
 *
 * @code{.cpp}
 * auto src_slice = src.slice({{"x", 0, 2}});
 * auto dst_slice = dst.slice({{"x", 0, 2}});
 * auto status = TransformVariable(src_slice, dst_slice, fn).status();
 * @endcode
 *
 * The returned future is a tensorstore future: it composes with
 * ExecuteWhenReady like any other. Callers observe the outcome through
 * @c status() (which waits).
 *
 * @param src The source variable (read in full).
 * @param dst The destination variable (written in full).
 * @param fn The element transform, called once per element. Each call must
 *     report the number of bytes it wrote, which must equal the destination
 *     element size.
 * @return A future carrying OkStatus, or the first error: a validation error
 *     (rank, shape, or element count mismatch), a read error, the first
 *     @p fn error or byte-count contract violation (in which case dst is not
 *     written at all), or the write's commit error.
 */
inline Future<absl::Status> TransformVariable(const Variable<>& src,
                                              Variable<>& dst,
                                              ElementTransform fn) {
  auto src_layout_result = internal::GetElementLayout(src);
  if (!src_layout_result.ok()) {
    return Future<absl::Status>(src_layout_result.status());
  }
  auto dst_layout_result = internal::GetElementLayout(dst);
  if (!dst_layout_result.ok()) {
    return Future<absl::Status>(dst_layout_result.status());
  }
  const internal::ElementLayout src_layout = src_layout_result.value();
  const internal::ElementLayout dst_layout = dst_layout_result.value();
  absl::Status compatibility =
      internal::CheckTransformCompatibility(src, dst, src_layout, dst_layout);
  if (!compatibility.ok()) {
    return Future<absl::Status>(std::move(compatibility));
  }

  // The transfer outlives this call. The read future keeps src's store
  // alive; dst must be kept alive until the write commits, so it is moved
  // into a shared_ptr captured by the callback (the Variable::Read
  // keepalive pattern).
  auto dst_keepalive = std::make_shared<Variable<>>(dst);
  auto pair = tensorstore::PromiseFuturePair<absl::Status>::Make();
  tensorstore::Read(src.get_store())
      .ExecuteWhenReady([dst_keepalive, src_layout, dst_layout,
                         fn = std::move(fn), promise = pair.promise](
                            tensorstore::ReadyFuture<
                                SharedArray<void, dynamic_rank, offset_origin>>
                                readyRead) mutable {
        auto read_result = readyRead.result();
        if (!read_result.ok()) {
          promise.SetResult(read_result.status());
          return;
        }
        internal::TransformAndWrite(*dst_keepalive, src_layout, dst_layout, fn,
                                    std::move(promise), read_result.value());
      });
  return pair.future;
}

}  // namespace mdio

#endif  // MDIO_TRANSFORM_H_
