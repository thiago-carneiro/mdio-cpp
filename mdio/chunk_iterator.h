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

#ifndef MDIO_CHUNK_ITERATOR_H_
#define MDIO_CHUNK_ITERATOR_H_

#include <algorithm>
#include <cstddef>
#include <limits>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "mdio/impl.h"
#include "tensorstore/box.h"

namespace mdio {

/**
 * @brief STL-compatible forward iterator over a variable's chunk grid.
 *
 * Dereferencing yields a tensorstore::Box with ABSOLUTE indices into the
 * variable's index domain: chunk k along dimension i starts at
 * `domain_origin[i] + k * chunk_shape[i]`, clamped at the domain edge so edge
 * chunks yield partial boxes and no index ever falls outside the domain.
 *
 * The grid is anchored at the domain origin. A freshly opened variable's
 * domain is 0-based, so the boxes coincide with the stored format's physical
 * chunk grid; a sliced variable's domain carries an offset (see the
 * `Variable::slice` note on domain origin semantics), and the grid is
 * tessellated over that offset domain starting at its origin.
 *
 * Traversal is row-major over the grid (the last dimension varies fastest).
 * The order is an implementation detail, not a contract.
 */
class ChunkIterator {
 public:
  using iterator_category = std::forward_iterator_tag;
  using value_type = tensorstore::Box<>;
  using reference = const value_type&;
  using pointer = const value_type*;
  using difference_type = std::ptrdiff_t;

  ChunkIterator() = default;

  ChunkIterator& operator++() {
    Advance();
    return *this;
  }

  ChunkIterator operator++(int) {
    ChunkIterator tmp = *this;
    Advance();
    return tmp;
  }

  reference operator*() const { return box_; }
  pointer operator->() const { return &box_; }

  bool operator==(const ChunkIterator& other) const {
    return linear_index_ == other.linear_index_ &&
           total_chunks_ == other.total_chunks_;
  }

  bool operator!=(const ChunkIterator& other) const {
    return !(*this == other);
  }

  /// Total number of chunks in the grid, partial edge chunks included.
  std::size_t total_chunks() const { return total_chunks_; }

  /// Zero-based row-major position of the current chunk in the grid.
  std::size_t current_chunk_index() const { return linear_index_; }

 private:
  friend class ChunkRange;

  ChunkIterator(std::vector<Index> domain_origin,
                std::vector<Index> domain_shape,
                std::vector<Index> chunk_shape);

  /// Iterator positioned one past the last chunk of a grid.
  static ChunkIterator EndFor(std::size_t total_chunks) {
    ChunkIterator end;
    end.linear_index_ = total_chunks;
    end.total_chunks_ = total_chunks;
    return end;
  }

  void Advance();

  /// Recomputes box_ from current_indices_, clamping at the domain edge.
  void SetBoxForCurrentIndices();

  std::vector<Index> domain_origin_;
  std::vector<Index> domain_shape_;
  std::vector<Index> chunk_shape_;
  /// Number of chunks along each dimension (ceil(shape / chunk_shape)).
  std::vector<std::size_t> chunks_per_dimension_;
  /// Row-major chunk indices of the current position, one per dimension.
  std::vector<std::size_t> current_indices_;
  value_type box_;
  std::size_t linear_index_ = 0;
  std::size_t total_chunks_ = 0;
};

/**
 * @brief A range over the chunk grid of a variable's index domain.
 *
 * Supports range-based for loops and the STL range protocols:
 *
 * @code
 * MDIO_ASSIGN_OR_RETURN(auto chunks, velocity.chunks());
 * for (const auto& chunk : chunks) {
 *   // chunk.origin() and chunk.shape() are absolute domain indices.
 * }
 * @endcode
 */
class ChunkRange {
 public:
  ChunkIterator begin() const { return begin_; }
  ChunkIterator end() const { return ChunkIterator::EndFor(total_chunks_); }

  /// Total number of chunks in the grid, partial edge chunks included.
  std::size_t size() const { return total_chunks_; }

  /**
   * @brief Builds a chunk grid over the half-open domain
   * [domain_origin, domain_origin + domain_shape).
   *
   * The grid is anchored at the domain origin: chunk k along dimension i
   * covers [domain_origin[i] + k * chunk_shape[i], ...), clamped at the domain
   * edge so edge chunks yield partial boxes.
   * @param domain_origin The absolute origin of the domain, one entry per
   * dimension.
   * @param domain_shape The size of the domain per dimension; every entry must
   * be positive.
   * @param chunk_shape The chunk size per dimension; every entry must be
   * positive.
   * @return An `mdio::Result` object containing the range, or
   * InvalidArgumentError if the entries per dimension do not match, any
   * domain/chunk size is not positive, or the domain extent or chunk-grid
   * size exceeds the range the grid math can address.
   */
  static Result<ChunkRange> Create(std::vector<Index> domain_origin,
                                   std::vector<Index> domain_shape,
                                   std::vector<Index> chunk_shape);

 private:
  ChunkRange(std::vector<Index> domain_origin, std::vector<Index> domain_shape,
             std::vector<Index> chunk_shape);

  ChunkIterator begin_;
  std::size_t total_chunks_ = 0;
};

inline ChunkIterator::ChunkIterator(std::vector<Index> domain_origin,
                                    std::vector<Index> domain_shape,
                                    std::vector<Index> chunk_shape)
    : domain_origin_(std::move(domain_origin)),
      domain_shape_(std::move(domain_shape)),
      chunk_shape_(std::move(chunk_shape)) {
  chunks_per_dimension_.reserve(domain_shape_.size());
  current_indices_.assign(domain_shape_.size(), 0);
  total_chunks_ = 1;
  for (std::size_t i = 0; i < domain_shape_.size(); ++i) {
    // Ceil division without the overflow the (shape + chunk - 1) form risks
    // for extreme shapes; both operands are positive (validated in Create).
    const std::size_t chunks_in_dim =
        static_cast<std::size_t>(domain_shape_[i] / chunk_shape_[i]) +
        (domain_shape_[i] % chunk_shape_[i] != 0 ? 1 : 0);
    chunks_per_dimension_.push_back(chunks_in_dim);
    total_chunks_ *= chunks_in_dim;
  }
  SetBoxForCurrentIndices();
}

inline void ChunkIterator::Advance() {
  if (linear_index_ >= total_chunks_) {
    return;
  }
  // Row-major increment: the last dimension varies fastest.
  for (std::size_t i = current_indices_.size(); i-- > 0;) {
    if (++current_indices_[i] < chunks_per_dimension_[i]) {
      ++linear_index_;
      SetBoxForCurrentIndices();
      return;
    }
    current_indices_[i] = 0;
  }
  // Wrapped past the last chunk: become the end iterator.
  linear_index_ = total_chunks_;
}

inline void ChunkIterator::SetBoxForCurrentIndices() {
  std::vector<Index> box_origin(domain_origin_.size());
  std::vector<Index> box_shape(domain_origin_.size());
  for (std::size_t i = 0; i < current_indices_.size(); ++i) {
    // Overflow-free clamping: for k < chunks_in_dim, k * chunk_shape stays
    // below domain_shape, so start cannot pass the domain edge, and adding
    // the remaining chunk size in the min keeps stop within the domain end
    // (Create guarantees origin + shape does not overflow).
    const Index domain_end = domain_origin_[i] + domain_shape_[i];
    const Index start = domain_origin_[i] +
                        static_cast<Index>(current_indices_[i]) *
                            chunk_shape_[i];
    const Index stop = start + std::min(chunk_shape_[i], domain_end - start);
    box_origin[i] = start;
    box_shape[i] = stop - start;
  }
  box_ = value_type(box_origin, box_shape);
}

inline Result<ChunkRange> ChunkRange::Create(std::vector<Index> domain_origin,
                                             std::vector<Index> domain_shape,
                                             std::vector<Index> chunk_shape) {
  const std::size_t rank = domain_shape.size();
  if (domain_origin.size() != rank || chunk_shape.size() != rank) {
    return absl::InvalidArgumentError(absl::StrCat(
        "Chunk grid requires one entry per dimension: got origin rank ",
        domain_origin.size(), ", domain shape rank ", rank,
        ", chunk shape rank ", chunk_shape.size(), "."));
  }
  for (std::size_t i = 0; i < rank; ++i) {
    if (domain_shape[i] <= 0) {
      return absl::InvalidArgumentError(absl::StrCat("Domain dimension ", i,
                                                     " has non-positive size ",
                                                     domain_shape[i], "."));
    }
    if (chunk_shape[i] <= 0) {
      return absl::InvalidArgumentError(absl::StrCat("Chunk shape dimension ",
                                                     i, " is non-positive (",
                                                     chunk_shape[i], ")."));
    }
    // The iterator computes domain_origin[i] + domain_shape[i] (the domain
    // end) and derives boxes from it; a domain extent beyond the Index range
    // would overflow that arithmetic.
    if (domain_origin[i] >
        std::numeric_limits<Index>::max() - domain_shape[i]) {
      return absl::InvalidArgumentError(absl::StrCat(
          "Domain dimension ", i, " extent (origin ", domain_origin[i],
          " + size ", domain_shape[i],
          ") exceeds the index range used by the chunk grid."));
    }
  }

  // Reject grids whose chunk count would exceed the iterator's size_t range:
  // the per-dimension ceil division is overflow-free, but the row-major
  // product must be checked dimension by dimension.
  std::size_t total_chunks = 1;
  for (std::size_t i = 0; i < rank; ++i) {
    const std::size_t chunks_in_dim =
        static_cast<std::size_t>(domain_shape[i] / chunk_shape[i]) +
        (domain_shape[i] % chunk_shape[i] != 0 ? 1 : 0);
    if (chunks_in_dim != 0 &&
        total_chunks > std::numeric_limits<std::size_t>::max() / chunks_in_dim) {
      return absl::InvalidArgumentError(absl::StrCat(
          "Chunk grid has too many chunks: dimension ", i, " alone needs ",
          chunks_in_dim, " chunks and the total would exceed the addressable ",
          "range."));
    }
    total_chunks *= chunks_in_dim;
  }

  return ChunkRange(std::move(domain_origin), std::move(domain_shape),
                    std::move(chunk_shape));
}

inline ChunkRange::ChunkRange(std::vector<Index> domain_origin,
                              std::vector<Index> domain_shape,
                              std::vector<Index> chunk_shape)
    : begin_(std::move(domain_origin), std::move(domain_shape),
             std::move(chunk_shape)) {
  total_chunks_ = begin_.total_chunks();
}

}  // namespace mdio

#endif  // MDIO_CHUNK_ITERATOR_H_
