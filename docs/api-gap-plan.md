# MDIO-C++ API Gap Plan

Status: draft (Sep 2026) · Base: `fcbfb85` (v0.2.0-pre-release)

This is the library-side plan for closing the API gaps between mdio-cpp and
mdio-python. The gaps were cataloged by a downstream consumer suite: 22
distinct example programs by the end of the evaluation (10 at the close of
its Phase 0), where an estimated 60–70% of the C++ code exists only to work
around these gaps. Every item below lists the API to add or complete, where
it lands, and a measurable acceptance criterion expressed as downstream code
shrinkage. Downstream line counts quoted per item come from the evaluation
corpus and are not independently audited in this repo.

Reference implementation: mdio-python 1.x (API names, semantics, and on-disk
interop must match it wherever possible).

## Gap summary

| # | Gap | mdio-python reference | Downstream cost | Plan |
|---|---|---|---|---|
| 1 | No serialization round-trip (`to_json`) | `to_mdio(open_mdio(...))` | ~700 lines (hand-built creation JSON) | M1 |
| 2 | No chunk iteration | xarray/dask lazy chunks | triple nested loops in every reader | M2 |
| 3 | `sel`: `ListDescriptor` blocked; range endpoints require exact value match | `dataset.sel(inline=slice(a, b))` | full coordinate reads + manual index math | M3 |
| 4 | No statistics computation | `statsV1` on ingest | manual accumulators per trace/slice | M4 |
| 5 | No dtype-erased transfer | dtype-generic xarray ops | hand-rolled dtype dispatch tables | M5 |
| 6 | No execution layer | dask schedulers | external orchestration (kept, for now) | M6 |
| 7 | Per-task I/O cost scales with chunk-file count (metadata path) | — (measured, not a parity gap) | distributed jobs: 56 min vs 0.75 s single-task | M7 |

## M1 — Serialization round-trip: `Dataset::to_json()`

The library has `Dataset::from_json()` (dataset_factory.h) but no inverse.
Every downstream copy program rebuilds the creation JSON by hand from
TensorStore specs — including heuristics the library would not need (units
guessing, invented dimension names), because it lacks access to library
internals.

```cpp
// mdio/dataset.h
/// Serializes an open Dataset back to the creation JSON consumed by
/// from_json(). Round-trip guarantee: from_json(to_json(ds)) opens an
/// equivalent dataset.
Result<nlohmann::json> Dataset::to_json(
    IncludeDefaults defaults = {}) const;
```

Design notes:

- Build on `Variable::get_spec()` (already returns the TensorStore spec JSON)
  and translate to the MDIO creation schema: dtype names, compressor
  normalization, `chunkGrid` from chunks, `dimension_names` from domain
  labels, `UserAttributes` round-tripped from stored array metadata.
- Output must validate against `dataset_schema.h` and open in mdio-python.
- No heuristics: the library has the real metadata; the downstream prototype
  guessed because it did not.

Tests: round-trip open → to_json → from_json → sampling validation; v2 and
v3 stores; struct arrays.

Acceptance: downstream dataset copy ≤60 lines (from 224); the downstream
`variable_spec_builder` (~460 lines) is deleted.

Size: ~300–400 lines + tests. Strongest single PR candidate.

## M2 — Chunk iteration: `Variable::chunks()`

Every downstream reader hand-rolls the same triple loop with edge clamping.
A 985-line draft iterator exists downstream and can be ported.

```cpp
// mdio/variable.h (or mdio/chunk_iterator.h)
/// STL-compatible forward iteration over the variable's chunk grid.
/// Yields Box<> with absolute domain indices; edge chunks yield partial
/// boxes. Range-for support.
class ChunkRange { ChunkIterator begin() const; ChunkIterator end() const; };
ChunkRange Variable::chunks() const;
```

Design notes:

- Derive the grid from `domain()` + `get_chunk_shape()`.
- Boxes are absolute w.r.t. the index domain (see "Domain origin semantics"
  below — domains are 0-based today).
- Edge chunks: partial boxes, never out-of-bounds indices.

Tests: shapes not divisible by the chunk shape; rank 1–4; 0-sized dims
rejected.

Acceptance: downstream trace reader ≤120 lines (from 358) with no manual
chunk loops.

Size: medium-large (draft exists).

## M3 — Value-based selection: `Dataset::sel()`

`CoordinateSelector::query()` returns `UnimplementedError` for
`RangeDescriptor`/`ListDescriptor` (coordinate_selector.h). Downstream code
reads entire coordinate variables to compute extents and converts values to
indices by hand.

```cpp
// mdio/dataset.h
/// Selection by coordinate VALUE (not index). Mirrors mdio-python sel().
Result<Dataset> Dataset::sel(std::variant<RangeDescriptor<Index>,
                                           ListDescriptor<Index>>... descs);
```

Design notes:

- Implement `CoordinateSelector::query` for Range/List: read the 1-D
  coordinate variable, validate monotonicity, binary-search value → index,
  delegate to `isel`.
- Document the value-vs-index distinction explicitly (see "Domain origin
  semantics").

Tests: monotonic coords; half-open value ranges; values outside the range
(error, not clamp); descending coords (reject or support — decide).

Acceptance: downstream ROI selection program ≤80 lines (from 246), no full
coordinate reads.

Size: ~3–5 days.

## M4 — Statistics: `statsV1` computation

The serialization target already exists (`dataset_schema.h` defines
`CenteredBinHistogram` with `counts`/`binCenters`; `UserAttributes` already
scrubs `statsV1`), but nothing computes it.

```cpp
// mdio/stats.h
struct SummaryStats { Index count; double sum, sumSquares, min, max;
                      Histogram histogram; };
/// Incremental (chunk-composable) computation over a variable.
Result<SummaryStats> ComputeStats(const Variable<>& var);
/// Persists as statsV1 user attributes (mdio-python compatible).
absl::Status Variable::UpdateAttributes(const SummaryStats&);
```

Design notes:

- Incremental accumulation so distributed callers can reduce partials.
- Schema-compatible with mdio-python `builder/schemas/v1/stats.py`.

Tests: known distributions; empty variables; float32 vs float64; histogram
edges.

Acceptance: downstream il/xl reader ≤150 lines (from 532); statsV1 written
by C++ reads back in mdio-python.

Size: ~1 week.

## M5 — Dtype-erased transfer

Largely subsumed by M1 (copy round-trip) + existing Read/Write. The residual
need is element-wise transforms without per-dtype switch statements
(downstream keeps a 628-line dispatch table for this).

```cpp
/// Applies fn element-wise during src → dst transfer, dtype-erased.
absl::Status TransformVariable(const Variable<>& src, Variable<>& dst,
                               ElementTransform fn);
```

Decide after M1 lands how much of this is still needed.

## M6 — Execution layer (proposal stage)

`MapChunks(variable, fn, ParallelOptions)` over TensorStore futures. Not
scheduled: downstream keeps external orchestration (Parsl/Slurm) until the
core gaps are closed. Draft proposal exists downstream.

## Small fixes (verified against `fcbfb85`)

Good first PRs, independent of the milestones:

1. **Slice error reports the wrong descriptor.** `Variable::slice` prints
   the ORIGINAL descriptor (`start=1004 > stop=1304` — a false comparison)
   while the check failed on the CLAMPED one. Report the clamped values
   (variable.h, slice loop).
2. **Missing store misreported as `.zmetadata` parse error.** Opening a
   non-existent store yields "Failed to parse .zmetadata. Try adding a
   trailing slash" because version detection fails silently and the flow
   falls back to v2. Detect the missing `zarr.json` first and say "not an
   MDIO store or path does not exist".
3. **Interop: mdio-python 1.x stores lack required metadata.** mdio-python
   `to_mdio` writes neither the root dataset metadata
   (`name`/`apiVersion`/`createdOn`) nor per-variable `dimension_names`,
   both required by the C++ v3 reader. Either tolerate missing metadata
   with defaults in C++, or contribute the metadata writing to mdio-python.
4. **Domain origin semantics.** Open-variable index domains are 0-based
   `[0, shape)` — zarr has no origin concept, and this holds on both the
   previous brian-michell fork pin (branch `v0.1.63_latest` @ `457285c`,
   July 2024, unmoved; 0-based `GetChunkGridBounds`) and
   google/tensorstore. Sliced variables carry offset domains (slicing
   `[0,383)` with `{83,383}` yields domain `[83,383)`) — the real
   non-zero-origin scenario behind the `b5e42fc` clamp fix. The gap is that
   consumer code conflates coordinate VALUES with domain INDICES; `sel` by
   value (M3) is the supported fix. Document the value-vs-index semantics
   explicitly.
 5. **(Withdrawn — not a tensorstore bug.)** Initially filed here as an
    "(External) tensorstore `IterateOverArrays` regression" at `917edaf34`
    (one callback per 1-D array; 2-D segfault). Refuted on Sep/8/2026 while
    preparing the upstream issue: the "minimal reproducer" had replicated the
    consumer program's own undefined behavior — binding an `ArrayView` (which
    holds an **unowned reference** to its layout) to a **temporary**
    `StridedLayout`, leaving the view dangling after the declaration
    statement. With a named layout, `IterateOverArrays` passes all 1-D/2-D
    void/bool cases at -O2; the consumer program was fixed by naming its
    layout (formato-dados `trace_reader_il_xl`, now exit=0). No upstream
    issue; no action in this repo. Lesson: a reproducer that copies the
    pattern under test copies its bugs too — the control must eliminate the
    consumer's UB before blaming the library.

## Sequencing

| Wave | Items | Unlocks downstream |
|---|---|---|
| 0 | Small fixes 1–3 | trust in error messages; mdio-python interop |
| 1 | M1 `to_json` | ~700 lines deleted; copy programs collapse |
| 2 | M2 chunks | all readers collapse |
| 3 | M3 `sel` | ROI selection collapses |
| 4 | M4 stats | readers lose accumulators |
| 5 | M5/M6 | transfer/execution (re-evaluate after M1) |

## Principles

1. **mdio-python parity**: same operation names, same semantics, same
   on-disk results (a store written by either implementation must round-trip
   in the other).
2. **Every PR ships tests** plus a stated downstream acceptance criterion
   (program line-count target or deleted workaround).
3. **No breaking changes without a deprecation note** — the API is pre-1.0
   but downstream pins exist.
4. **Interop tests are two-directional** (mdio-python-written stores open in
   C++; C++-written stores open in mdio-python).
