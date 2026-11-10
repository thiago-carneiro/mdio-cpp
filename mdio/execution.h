// Copyright 2026 TGS

// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.

//    http://www.apache.org/licenses/LICENSE-2.0

// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef MDIO_EXECUTION_H_
#define MDIO_EXECUTION_H_

#include <algorithm>
#include <condition_variable>  // NOLINT
#include <cstddef>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>   // NOLINT
#include <thread>  // NOLINT
#include <utility>
#include <vector>

#include "absl/functional/any_invocable.h"
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "mdio/chunk_iterator.h"
#include "mdio/impl.h"
#include "mdio/transform.h"
#include "mdio/variable.h"
#include "tensorstore/array.h"
#include "tensorstore/box.h"
#include "tensorstore/index_space/dim_expression.h"
#include "tensorstore/tensorstore.h"
#include "tensorstore/util/future.h"

namespace mdio {

/**
 * @brief Options controlling how MapChunks schedules work.
 *
 * Defaults are justified against the measured NFS costs (see
 * docs/api-gap-m6-execution-layer.md): after M7 the metadata path is already
 * minimal, so the options only bound scheduling; they never add opens.
 */
struct ParallelOptions {
  /// Number of worker threads executing chunk tasks. 0 means
  /// std::thread::hardware_concurrency(). 1 is the explicit serial mode:
  /// tasks run sequentially on the CALLING thread in row-major order and no
  /// thread is ever spawned.
  std::size_t max_workers = 0;
  /// Maximum number of tasks executing concurrently. 0 means "follow
  /// max_workers". Effective concurrency is min(max_workers, max_in_flight);
  /// this bounds how many task read buffers are alive at once.
  std::size_t max_in_flight = 0;
  /// Region fusion: how many consecutive chunks (within one grid row) are
  /// grouped into one task that reads their union region with a single
  /// tensorstore::Read. 1 disables fusion. Must not be 0. Measured default
  /// (NFS, docs/api-gap-m6-execution-layer.md): 4 never measured worse than
  /// 1 and recovers 18% at 4 workers; a whole grid smaller than the value
  /// degenerates to one task, so set 1 for tiny grids.
  std::size_t chunks_per_task = 4;
  /// When true, each chunk whose callback returns OK is written back to the
  /// store through the existing tensorstore::Write semantics (in-place
  /// read-modify-write map). When false, the store is never written and the
  /// callback is expected to consume the chunk data itself.
  bool write_back = false;
};

/**
 * @brief The per-chunk view passed to a MapChunks callback.
 *
 * `data` is a C-contiguous, 0-origin, row-major COPY of the chunk's bytes,
 * extracted from the task's union-region read buffer; it is owned by the
 * callback frame and valid only for the duration of the call. `domain`
 * carries the chunk's ABSOLUTE indices (the same box `Variable::chunks()`
 * yields at position `index`), so a callback that needs absolute addressing
 * translates `data` by `domain.origin()` itself.
 */
struct ChunkView {
  /// Zero-based row-major position of the chunk in the variable's chunk grid.
  std::size_t index = 0;
  /// The chunk's box in absolute domain indices.
  tensorstore::Box<> domain;
  /// Size in bytes of one element: the dtype itemsize, or the whole record
  /// size for a structured dtype opened as its void byte view (the same unit
  /// TransformVariable uses).
  std::size_t element_size = 0;
  /// Mutable view of the chunk's data: origin 0, shape == domain.shape(),
  /// C-contiguous row-major, dtype == the variable's dtype. The callback may
  /// rewrite any byte of it; with write_back the rewritten bytes are what
  /// gets written back for this chunk.
  tensorstore::SharedArray<void, dynamic_rank, zero_origin> data;
};

/**
 * @brief Per-chunk callback invoked by MapChunks.
 *
 * Called once per chunk. Returning an error fails that chunk only: its
 * status is reported in MapChunks' result vector and, with write_back, its
 * data is NOT written back. Sibling chunks are unaffected. The callback may
 * run concurrently with itself for different chunks when max_workers > 1;
 * any state it captures must be synchronized by the caller (per-chunk slots
 * of a preallocated vector need no lock).
 */
using ChunkFunction =
    absl::AnyInvocable<absl::Status(const ChunkView& view) const>;

namespace internal {

/// A run of consecutive chunks of one grid row, fused into a single task.
struct ChunkTask {
  /// Row-major chunk position of chunks.front() in the chunk grid.
  std::size_t first_chunk_index = 0;
  /// Union region of the task's chunks in absolute domain indices.
  tensorstore::Box<> region;
  /// The task's chunk boxes, absolute, row-major consecutive.
  std::vector<tensorstore::Box<>> chunks;
};

/// True when `next` directly continues `prev` along the last dimension with
/// identical extents on every other dimension (i.e. both boxes lie in the
/// same grid row and are adjacent), so their union is one box.
inline bool ContiguousInRow(const tensorstore::Box<>& prev,
                            const tensorstore::Box<>& next) {
  const DimensionIndex rank = prev.rank();
  if (next.rank() != rank || rank == 0) {
    return false;
  }
  for (DimensionIndex dim = 0; dim + 1 < rank; ++dim) {
    if (prev.origin()[dim] != next.origin()[dim] ||
        prev.shape()[dim] != next.shape()[dim]) {
      return false;
    }
  }
  return next.origin()[rank - 1] ==
         prev.origin()[rank - 1] + prev.shape()[rank - 1];
}

/// The union box of a task's chunk run: identical extents on all dimensions
/// except the last, where the run is contiguous by construction.
inline tensorstore::Box<> UnionOf(
    const std::vector<tensorstore::Box<>>& boxes) {
  const tensorstore::Box<>& first = boxes.front();
  std::vector<Index> origin(first.origin().begin(), first.origin().end());
  std::vector<Index> shape(first.shape().begin(), first.shape().end());
  const DimensionIndex rank = first.rank();
  if (rank > 0) {
    const tensorstore::Box<>& last = boxes.back();
    shape[rank - 1] =
        last.origin()[rank - 1] + last.shape()[rank - 1] - origin[rank - 1];
  }
  return tensorstore::Box<>(origin, shape);
}

/// Groups the chunk grid into fused tasks: runs of at most `chunks_per_task`
/// consecutive row-major chunks that stay within one grid row (a run crossing
/// a row boundary is not a box, so it starts a new task).
inline std::vector<ChunkTask> FuseTasks(const ChunkRange& chunks,
                                        std::size_t chunks_per_task) {
  std::vector<ChunkTask> tasks;
  ChunkTask current;
  std::size_t position = 0;
  for (const auto& box : chunks) {
    if (!current.chunks.empty() && current.chunks.size() < chunks_per_task &&
        ContiguousInRow(current.chunks.back(), box)) {
      current.chunks.push_back(box);
    } else if (!current.chunks.empty()) {
      current.region = UnionOf(current.chunks);
      tasks.push_back(std::move(current));
      current = ChunkTask{};
      current.first_chunk_index = position;
      current.chunks.push_back(box);
    } else {
      current.first_chunk_index = position;
      current.chunks.push_back(box);
    }
    ++position;
  }
  if (!current.chunks.empty()) {
    current.region = UnionOf(current.chunks);
    tasks.push_back(std::move(current));
  }
  return tasks;
}

/// Extracts one chunk of a task's read buffer into a fresh, C-contiguous,
/// 0-origin buffer with shared ownership. Fused tasks read a union region;
/// a chunk inside it is a strict sub-row block, which is strided in the
/// union's C layout — the copy restores the contiguous per-chunk layout the
/// ChunkView contract promises (one memcpy per chunk, negligible next to the
/// per-chunk round-trip it batches).
inline tensorstore::SharedArray<void, dynamic_rank, zero_origin> MakeChunkView(
    const tensorstore::SharedArray<void, dynamic_rank, offset_origin>&
        task_array,
    const tensorstore::Box<>& task_region,
    const tensorstore::Box<>& chunk_box) {
  tensorstore::SharedArray<void, dynamic_rank, zero_origin> chunk_array =
      tensorstore::AllocateArray(std::vector<Index>(chunk_box.shape().begin(),
                                                    chunk_box.shape().end()),
                                 mdio::ContiguousLayoutOrder::c,
                                 tensorstore::value_init, task_array.dtype());
  const DimensionIndex rank = task_array.rank();
  if (rank == 0) {
    std::memcpy(chunk_array.byte_strided_origin_pointer().get(),
                task_array.byte_strided_origin_pointer().get(),
                static_cast<std::size_t>(task_array.dtype().size()));
    return chunk_array;
  }
  const char* src_base =
      static_cast<const char*>(task_array.byte_strided_origin_pointer().get());
  char* dst_base =
      static_cast<char*>(chunk_array.byte_strided_origin_pointer().get());
  const std::size_t element_bytes =
      static_cast<std::size_t>(task_array.byte_strides()[rank - 1]);
  Index src_base_offset = 0;
  for (DimensionIndex dim = 0; dim < rank; ++dim) {
    src_base_offset += (chunk_box.origin()[dim] - task_region.origin()[dim]) *
                       task_array.byte_strides()[dim];
  }
  // Destination contiguous byte strides; source strides are the task's.
  std::vector<Index> dst_stride(rank);
  dst_stride[rank - 1] = static_cast<Index>(element_bytes);
  for (DimensionIndex dim = rank - 1; dim-- > 0;) {
    dst_stride[dim] = dst_stride[dim + 1] * chunk_box.shape()[dim + 1];
  }
  std::vector<Index> index(rank, 0);
  bool done = false;
  while (!done) {
    Index src = src_base_offset;
    Index dst = 0;
    for (DimensionIndex dim = 0; dim < rank; ++dim) {
      src += index[dim] * task_array.byte_strides()[dim];
      dst += index[dim] * dst_stride[dim];
    }
    std::memcpy(dst_base + dst, src_base + src, element_bytes);
    done = true;
    for (DimensionIndex dim = rank; dim-- > 0;) {
      if (++index[dim] < chunk_box.shape()[dim]) {
        done = false;
        break;
      }
      index[dim] = 0;
    }
  }
  return chunk_array;
}

/// Bounds the number of concurrently executing tasks (max_in_flight).
class ConcurrencyGate {
 public:
  explicit ConcurrencyGate(std::size_t limit) : limit_(limit) {}

  void Acquire() {
    std::unique_lock<std::mutex> lock(mutex_);
    available_.wait(lock, [this] { return active_ < limit_; });
    ++active_;
  }

  void Release() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      --active_;
    }
    available_.notify_one();
  }

 private:
  std::mutex mutex_;
  std::condition_variable available_;
  std::size_t limit_;
  std::size_t active_ = 0;
};

/**
 * @brief Minimal fixed-size worker pool that dies with its last task.
 *
 * Every submitted closure holds a shared_ptr to the pool, so the pool is
 * destroyed exactly when the LAST task's closure is destroyed. That
 * destruction happens on a worker thread, so the destructor joins every
 * OTHER worker and detaches the current one (a thread may detach itself; it
 * must never join itself). The worker loop is structured so that after the
 * last closure dies it touches no pool member: the closure is destroyed as
 * the final statement before returning, and no member access follows.
 * Submitted tasks must never throw; the completion callback runs exactly
 * once, when the last task finishes, before the pool is destroyed.
 */
class TaskRunnerPool {
 public:
  explicit TaskRunnerPool(std::size_t worker_count) {
    workers_.reserve(worker_count);
    for (std::size_t i = 0; i < worker_count; ++i) {
      workers_.emplace_back([this] { WorkerLoop(); });
    }
  }

  /// Must be called before the first Submit (no locking needed: the write
  /// happens-before every worker's read through the Submit mutex).
  void SetCompletion(absl::AnyInvocable<void()> completion) {
    completion_ = std::move(completion);
  }

  void Submit(absl::AnyInvocable<void()> task) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      pending_ += 1;
      tasks_.push_back(std::move(task));
    }
    has_tasks_.notify_one();
  }

  ~TaskRunnerPool() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      shutting_down_ = true;
    }
    has_tasks_.notify_all();
    const std::thread::id current = std::this_thread::get_id();
    for (std::thread& worker : workers_) {
      if (worker.get_id() == current) {
        // The last task's closure is being destroyed on its own worker;
        // detach it instead of joining ourselves.
        worker.detach();
      } else {
        worker.join();
      }
    }
  }

 private:
  void WorkerLoop() {
    for (;;) {
      absl::AnyInvocable<void()> task;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        has_tasks_.wait(lock,
                        [this] { return shutting_down_ || !tasks_.empty(); });
        if (tasks_.empty()) {
          return;  // shutting_down_ is set and no work remains.
        }
        task = std::move(tasks_.front());
        tasks_.pop_front();
      }
      task();
      bool finished_all;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_ -= 1;
        finished_all = pending_ == 0;
      }
      if (finished_all && completion_) {
        completion_();
      }
      if (!finished_all) {
        // Not the last task: the closure is destroyed here and the pool is
        // still alive (the remaining closures hold references).
        task = nullptr;
        continue;
      }
      // Last task: destroy the closure as the FINAL action; this may run the
      // pool destructor on this very worker. No member access after this.
      {
        absl::AnyInvocable<void()> doomed = std::move(task);
      }
      return;
    }
  }

  std::mutex mutex_;
  std::condition_variable has_tasks_;
  std::deque<absl::AnyInvocable<void()>> tasks_;
  std::vector<std::thread> workers_;
  std::size_t pending_ = 0;
  bool shutting_down_ = false;
  absl::AnyInvocable<void()> completion_;
};

inline std::size_t ResolveWorkerCount(std::size_t requested) {
  if (requested != 0) {
    return requested;
  }
  const unsigned int hardware = std::thread::hardware_concurrency();
  return hardware == 0 ? 1 : static_cast<std::size_t>(hardware);
}

/// Executes one fused task: reads the union region, extracts each chunk into
/// a contiguous buffer, invokes the callback per chunk, and writes
/// successful chunks back when requested. Returns one status per chunk of
/// the task, in task order. Never throws: every failure mode becomes a
/// status.
inline std::vector<absl::Status> RunChunkTask(const Variable<>& variable,
                                              const ChunkTask& task,
                                              const ChunkFunction& fn,
                                              const ParallelOptions& options,
                                              std::size_t element_size) {
  std::vector<absl::Status> chunk_statuses(task.chunks.size());
  tensorstore::SharedArray<void, dynamic_rank, offset_origin> task_array;
  try {
    auto task_store = variable.get_store() |
                      tensorstore::AllDims().TranslateBoxSlice(task.region);
    if (!task_store.ok()) {
      chunk_statuses.assign(task.chunks.size(), std::move(task_store).status());
      return chunk_statuses;
    }
    auto read_result = tensorstore::Read(task_store.value()).result();
    if (!read_result.ok()) {
      chunk_statuses.assign(task.chunks.size(), read_result.status());
      return chunk_statuses;
    }
    task_array = std::move(read_result).value();
  } catch (const std::exception& exception) {
    chunk_statuses.assign(task.chunks.size(),
                          absl::InternalError(absl::StrCat(
                              "MapChunks task failed: ", exception.what())));
    return chunk_statuses;
  } catch (...) {
    chunk_statuses.assign(task.chunks.size(),
                          absl::InternalError("MapChunks task failed with a "
                                              "non-standard exception."));
    return chunk_statuses;
  }

  for (std::size_t local = 0; local < task.chunks.size(); ++local) {
    const tensorstore::Box<>& chunk_box = task.chunks[local];
    ChunkView view;
    view.index = task.first_chunk_index + local;
    view.domain = chunk_box;
    view.element_size = element_size;
    view.data = MakeChunkView(task_array, task.region, chunk_box);
    absl::Status status;
    try {
      status = fn(view);
    } catch (const std::exception& exception) {
      status = absl::InternalError(
          absl::StrCat("MapChunks callback for chunk ", view.index,
                       " threw an exception: ", exception.what()));
    } catch (...) {
      status = absl::InternalError(
          absl::StrCat("MapChunks callback for chunk ", view.index,
                       " threw a non-standard exception."));
    }
    if (!status.ok() || !options.write_back) {
      chunk_statuses[local] = std::move(status);
      continue;
    }
    // In-place write-back through the existing write semantics: the
    // contiguous chunk buffer onto the 0-origin box view of the store aligns
    // by translation, exactly like TransformVariable's write. The chunk
    // buffer stays alive in this frame until every chunk's write has
    // committed, satisfying Write's requirement that the source array
    // outlive copy_future.
    try {
      auto chunk_store = variable.get_store() |
                         tensorstore::AllDims().TranslateBoxSlice(chunk_box);
      if (chunk_store.ok()) {
        auto commit_result = tensorstore::Write(view.data, chunk_store.value())
                                 .commit_future.result();
        chunk_statuses[local] = commit_result.status();
      } else {
        chunk_statuses[local] = std::move(chunk_store).status();
      }
    } catch (const std::exception& exception) {
      chunk_statuses[local] = absl::InternalError(
          absl::StrCat("MapChunks write of chunk ", view.index,
                       " threw an exception: ", exception.what()));
    } catch (...) {
      chunk_statuses[local] = absl::InternalError(
          absl::StrCat("MapChunks write of chunk ", view.index,
                       " threw a non-standard exception."));
    }
  }
  return chunk_statuses;
}

}  // namespace internal

/**
 * @brief Applies a callback to every chunk of a variable's chunk grid,
 * scheduling the work across fused tasks and worker threads.
 *
 * The variable's chunk grid (Variable::chunks()) is grouped by
 * ParallelOptions::chunks_per_task: each task covers consecutive chunks of
 * one grid row, reads their union region with a single tensorstore::Read
 * through the variable's existing store handle (one open per call, never per
 * chunk), extracts each chunk into a contiguous buffer, and invokes @p fn
 * once per chunk with a zero-origin ChunkView. With
 * ParallelOptions::write_back, each chunk whose callback returns OK is
 * written back through tensorstore::Write — the same write semantics as
 * Variable::Write and TransformVariable, so output bytes are identical to a
 * serial execution.
 *
 * The returned future's RESULT is one status per chunk, indexed by the
 * chunk's row-major position (the same position Variable::chunks() and
 * ChunkView::index use). The future's own status() is an error only for
 * upfront validation failures; per-chunk outcomes must be read from the
 * vector: call result(), check it, then inspect every element for ok().
 *
 * Failure semantics: a callback error (or a write error) fails its chunk
 * only — that chunk is not written back and its status lands in the vector;
 * sibling chunks are independent and still complete, so partial results are
 * reported per chunk and the set of written chunks does not depend on the
 * worker count. A failed read of a fused task fails every chunk of that
 * task.
 *
 * Write constraints: with write_back, chunks are written through the same
 * tensorstore::Write path Variable::Write uses, so the variable's effective
 * writability is whatever Variable::Write would see (an explicitly
 * read-opened store converted to the dynamic-mode Variable<> does not carry
 * the read-only tag — measured — and neither API detects it).
 *
 * Thread safety: all tasks share the variable's single open store handle;
 * concurrent reads/writes of disjoint regions are safe. The callback runs
 * concurrently with itself for different chunks when more than one worker is
 * used; synchronize any shared captured state. Running a second MapChunks on
 * the same variable concurrently is the caller's responsibility.
 *
 * @param variable The variable to iterate; it must outlive the returned
 *     future's resolution (the tasks hold a keepalive copy).
 * @param fn The per-chunk callback; must be non-empty.
 * @param options Scheduling options; ParallelOptions() is valid.
 * @return A future resolving to the per-chunk status vector, or to an
 *     upfront validation error (empty callback, chunks_per_task == 0, a
 *     chunk-grid error, or no spec metadata) before any task runs.
 */
inline Future<std::vector<absl::Status>> MapChunks(
    const Variable<>& variable, ChunkFunction fn,
    ParallelOptions options = {}) {
  using ResultVector = std::vector<absl::Status>;
  if (!fn) {
    return Future<ResultVector>(absl::InvalidArgumentError(
        "MapChunks requires a non-empty per-chunk callback."));
  }
  if (options.chunks_per_task == 0) {
    return Future<ResultVector>(
        absl::InvalidArgumentError("MapChunks requires chunks_per_task >= 1."));
  }
  auto chunk_range = variable.chunks();
  if (!chunk_range.ok()) {
    return Future<ResultVector>(std::move(chunk_range).status());
  }
  auto layout = internal::GetElementLayout(variable);
  if (!layout.ok()) {
    return Future<ResultVector>(std::move(layout).status());
  }
  // NOTE: there is deliberately no upfront read-only check. mdio's
  // dynamic-mode Variable<> conversion drops the read-only tag (measured:
  // a store opened with ReadWriteMode::read reports read_write through
  // get_store().read_write_mode()), so MapChunks cannot detect it; its
  // write_back path therefore has exactly the constraint profile of
  // Variable::Write on the same variable.

  std::vector<internal::ChunkTask> tasks =
      internal::FuseTasks(chunk_range.value(), options.chunks_per_task);
  const std::size_t total_chunks = chunk_range.value().size();

  auto pair = tensorstore::PromiseFuturePair<ResultVector>::Make();
  auto results = std::make_shared<ResultVector>(total_chunks);
  auto keepalive = std::make_shared<Variable<>>(variable);
  auto shared_fn = std::make_shared<ChunkFunction>(std::move(fn));

  auto run_task = [keepalive, shared_fn, options,
                   element_size = layout.value().element_size](
                      const internal::ChunkTask& task) {
    return internal::RunChunkTask(*keepalive, task, *shared_fn, options,
                                  element_size);
  };

  const std::size_t workers = internal::ResolveWorkerCount(options.max_workers);
  if (workers <= 1 || tasks.size() <= 1) {
    // Serial mode: the calling thread executes every task in row-major
    // order; no thread is spawned.
    for (internal::ChunkTask& task : tasks) {
      std::vector<absl::Status> chunk_statuses = run_task(task);
      const std::size_t base = task.first_chunk_index;
      for (std::size_t local = 0; local < chunk_statuses.size(); ++local) {
        (*results)[base + local] = std::move(chunk_statuses[local]);
      }
    }
    pair.promise.SetResult(std::move(*results));
    return pair.future;
  }

  // Parallel mode: the pool is owned by the task closures themselves, so it
  // is destroyed by whichever worker finishes the last task; the completion
  // callback publishes the aggregated vector before that teardown.
  auto pool = std::make_shared<internal::TaskRunnerPool>(workers);
  pool->SetCompletion(
      [pair, results] { pair.promise.SetResult(std::move(*results)); });

  std::shared_ptr<internal::ConcurrencyGate> gate;
  if (options.max_in_flight != 0) {
    gate = std::make_shared<internal::ConcurrencyGate>(options.max_in_flight);
  }
  for (internal::ChunkTask& task : tasks) {
    pool->Submit(
        [pool, gate, run_task, results, task = std::move(task)]() mutable {
          if (gate) {
            gate->Acquire();
          }
          std::vector<absl::Status> chunk_statuses = run_task(task);
          const std::size_t base = task.first_chunk_index;
          for (std::size_t local = 0; local < chunk_statuses.size(); ++local) {
            (*results)[base + local] = std::move(chunk_statuses[local]);
          }
          if (gate) {
            gate->Release();
          }
        });
  }
  return pair.future;
}

}  // namespace mdio

#endif  // MDIO_EXECUTION_H_
