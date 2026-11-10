# M6 — Execution layer: `MapChunks` design spec

Status: implemented per this spec · Parent plan: `docs/api-gap-plan.md` (M6)
Base: `9612b5b` (post M7 / M1–M5)

## M7 refactor premise

M7 removed the measured metadata bottleneck: the store-wide `kvstore::List`
is gone (variable index in the root `zarr.json`), so a read no longer walks
the store's directories. The remaining per-chunk cost is the chunk-file
round-trip itself (~25 ms per 16-chunk read on this NFS backend, i.e. ~1.5
ms per chunk, dominated by the `openat`+read of each chunk file). **M6
therefore addresses per-chunk latency only — overlapping chunk round-trips
across workers and batching consecutive chunks per task — not metadata
syscall counts**, which are already minimal (~1 dataset open + 1 read per
chunk file) and stay invariant under parallelism.

## API

New header `mdio/execution.h` (sibling of `transform.h`; `variable.h` is
~2000 lines and must not grow further).

```cpp
struct ParallelOptions {
  std::size_t max_workers    = 0;  // 0 = hardware_concurrency; 1 = serial
  std::size_t max_in_flight  = 0;  // 0 = follows max_workers
  std::size_t chunks_per_task = 4; // fusion: consecutive chunks per task
  bool        write_back     = false; // write mutated chunk data back
};

struct ChunkView {
  std::size_t index;        // row-major position in the chunks() grid
  tensorstore::Box<> domain; // ABSOLUTE domain indices of the chunk
  std::size_t element_size;  // dtype itemsize (record size for struct views)
  SharedArray<void, dynamic_rank, zero_origin> data; // 0-origin, mutable
};

using ChunkFunction =
    absl::AnyInvocable<absl::Status(const ChunkView& view) const>;

Future<std::vector<absl::Status>> MapChunks(
    const Variable<>& variable, ChunkFunction fn,
    ParallelOptions options = {});
```

Shape justification: the plan draft's `Future<vector<Result<...>>>`
materializes as `Future<std::vector<absl::Status>>` — one status per chunk,
in row-major `chunks()` order. The future's own `status()` is an error only
for upfront validation failures; per-chunk outcomes live in the vector, and
callers must scan the vector (documented at the call site). A typed
per-chunk return channel (template on `R` + `AnyInvocable` deduction) was
rejected: per-chunk VALUES flow through caller-owned aggregation state
captured by `fn` (the compute-stats flow), exactly as dask-style block maps
aggregate; statuses are all the scheduler itself owes the caller.

`fn` contract: invoked once per chunk with a C-contiguous, 0-origin view of
the chunk bytes (`view.data`) — a fresh contiguous COPY extracted from the
task's union-region read (a chunk inside a fused region is strided there;
the one-memcpy-per-chunk extraction restores the contiguous layout the
contract promises, and its cost is negligible next to the per-chunk
round-trip it batches). The view is valid only for the call. `fn` may mutate
the bytes in place; with `write_back = true` each chunk whose `fn` returned
OK is written back through `tensorstore::Write` — the existing write
semantics (same `Write(array, store)` path as
`Variable::Write`/`TransformVariable`), so output bytes are identical to the
serial path by construction. With `write_back = false` the store is never
touched (compute-stats flow). `fn` runs concurrently for different chunks
when `max_workers > 1`; caller-owned captured state must be synchronized by
the caller (per-chunk slots of a preallocated vector need no lock).

## ParallelOptions — defaults and measured justification

- `max_workers = 0` → `std::thread::hardware_concurrency()`. The measured
  per-chunk cost is latency (NFS RTT), not CPU, so worker overlap is the
  lever; K=8 did not degrade in the evaluation. `max_workers = 1` is the
  explicit serial mode: tasks execute sequentially on the CALLING thread, in
  row-major order, and no thread is ever spawned (deterministic, trivially
  race-free, and the equivalence baseline).
- `max_in_flight = 0` → follows `max_workers`. Bounds tasks executing
  concurrently (each task = its read → fn → write pipeline); effective
  concurrency is `min(max_workers, max_in_flight)`. It caps memory: at most
  `min(...)` task buffers are alive.
- `chunks_per_task = 4` → mild default fusion (measured: never worse than 1,
  −18% at 4 workers on the NFS gate; a whole grid smaller than the value
  degenerates to one task, so callers with tiny grids pass 1). Fusion groups
  consecutive chunks **within one grid row** into one task: a task reads the
  UNION box with a single `tensorstore::Read` (tensorstore then fetches the
  k underlying chunk files, concurrently on its own I/O executor), extracts
  contiguous per-chunk buffers, calls `fn` per chunk, and writes each chunk
  back individually. Tasks never span grid rows (a row-major run crossing a
  row boundary is not a box).
- **Open-handle reuse: fixed policy, not a knob.** `MapChunks` takes an
  already-open `Variable` and issues every read/write through its single
  `TensorStore` handle. There is one handle per call, never per chunk or per
  worker — the substrate (post-M2/M7) already reuses the open handle, so
  building anything else would reintroduce the per-task dataset open cost
  (~25 ms per 16-chunk read) that M7 removed.
- **Metadata cache: fixed policy, not a knob.** All I/O shares the
  variable's existing `Context` (kvstore + metadata caches). The first task
  warms them; later tasks hit the cache. A per-worker `Context` would reopen
  the store per worker — rejected for the reason above.

## Semantics that must hold

1. **Byte-identical output to the serial path.** Same chunk grid, same
   per-chunk reads/writes through the same store semantics; parallelism
   changes only the schedule. Gated by an equivalence test (below) and by
   checksum equality in the NFS measurement.
2. **Deterministic result ordering.** The returned vector is indexed by the
   chunk's row-major position — the same position `chunks()` yields it at,
   and the same position `fn` sees in `view.index` — regardless of worker
   count. fn INVOCATION order is row-major only in serial mode; in parallel
   mode it is unspecified (documented).
3. **fn failure.** The failed chunk gets no write (its slot holds the
   error); sibling chunks are independent and still processed — continue-
   on-error, not fail-fast, so the set of written chunks is identical in
   serial and parallel modes. A task-level read failure assigns the read
   error to every chunk of the task. A write commit failure is that chunk's
   status. Partial results are thus reported per chunk in the vector.
4. **Thread safety.** One store handle shared by all tasks: concurrent
   reads of disjoint regions and concurrent writes of disjoint regions are
   safe (tensorstore's cache pool is internally synchronized). Tasks are
   disjoint by construction; concurrent `MapChunks` calls on the SAME
   variable, or `fn` state shared across chunks, are the caller's
   responsibility (documented). Write-back captures the task buffer in each
   commit callback so the bytes stay alive until the write commits (the
   `TransformAndWrite` keepalive pattern).
5. **Fail fast.** Empty `fn`, `chunks_per_task == 0`, a `chunks()` error,
   or `write_back` on an explicitly read-only store return an errored
   future before any task runs; nothing is read or written.

## Test spec

1. **API shape / serial equivalence**: a float32 variable, `fn` = in-place
   gain, `write_back = true`: `MapChunks(max_workers=1)` output equals the
   plain per-chunk serial loop byte-for-byte (full read + compare).
2. **Fusion invariants**: `chunks_per_task = k` invokes `fn` exactly once
   per chunk, `view.index` values are the row-major sequence 0..N-1 with
   `view.domain` equal to the `chunks()` box at that position (asserted by
   recording `(index, domain)` pairs), and aggregation (statuses) is correct
   for k that does not divide the chunk count and for k spanning a partial
   edge chunk.
3. **Parallel/serial equivalence**: `max_workers = 4` with `write_back`
   produces byte-identical output to `max_workers = 1` on the same store.
   Race exercise: `fn` writes into per-chunk slots of a preallocated vector
   (disjoint, lock-free); every slot filled exactly once.
4. **Failure paths**: `fn` error on one chunk → that chunk's status is the
   error, NO write for it (verified: its bytes unchanged), siblings
   written; read-level failure simulated by an invalid... (not injectable —
   covered by fn-error path; store-level read errors are out of scope for
   unit tests). `write_back` on a read-only store → errored future, no I/O.
   `chunks_per_task = 0` and empty `fn` → errored future.
5. **Parallelism-off mode**: `max_workers = 1` never spawns a thread beyond
   the calling one (behavioral: results still correct; and it is the
   equivalence baseline of test 1).
6. **Structured dtype** (void byte view): record-size `element_size` is
   honored per chunk (parity with the M5 record contract).

## Acceptance measurement spec (NFS, node gr15sdes01)

Harness: new `mdio_m6_map_chunks_bench` (reuses the M7 dataset builder and
the `elapsed_ms=... checksum=...` output contract, so the M7 strace census
pattern applies verbatim). Dataset: the M7 "few" grid — (383,304,256)
float32, chunks (48,76,64) = 128 chunk files, ~119 MB, on NFS
(`home2/tmp`). Per-chunk operation: in-place ×2 gain with `write_back =
true` (read-modify-write per chunk), restored via exact ÷2 after the run
(×2/÷2 are lossless in float32; checksum equality gates every run).

Arms, one fresh process each, median of 5 (wall) / 3 (traced):

| Arm | Execution |
|---|---|
| `slice-serial` | hand loop over `chunks()`: per-chunk box read → gain → write (today's downstream per-chunk pattern; baseline) |
| `map-serial` | `MapChunks(workers=1, k=1, write_back)` |
| `map-fused` | `MapChunks(workers=1, k=K)` — isolates fusion from parallelism |
| `map-par` | `MapChunks(workers=W, k=K)` — the parallel layer |

Metrics: wall time (median), `openat`/`getdents64` counts under `strace -f
-tt -T -y -e trace=openat,getdents64` (same census as M7), checksum
equality across arms. Expected (per the M7 premise): syscall counts ~equal
across arms (one read per chunk file is irreducible); the win, if any, is
wall-time latency overlap. The report states where it wins, loses, and why:
`/tgdesenv/src/proj0/u4w7/home2/tmp/py-enforcer/panel/m6-measurements.md`.
