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
| 3 | `sel`: range endpoints require exact value match (nearest-value selection absent) | `dataset.sel(inline=slice(a, b))` | full coordinate reads + manual index math | M3 |
| 4 | No statistics computation | `statsV1` on ingest | manual accumulators per trace/slice | M4 |
| 5 | No dtype-erased transfer | dtype-generic xarray ops | hand-rolled dtype dispatch tables | M5 |
| 6 | No execution layer | dask schedulers | external orchestration (kept, for now) | M6 |
| 7 | Per-task I/O cost scales with chunk-file count (metadata path) | — (measured, not a parity gap) | distributed jobs: 56 min vs 0.75 s single-task | M7 |

## M1 — Serialization round-trip: `Dataset::to_json()`

The library has `Dataset::from_json()` (dataset.h:292) but no inverse —
`to_json` does not exist anywhere in `mdio/`. Every downstream copy program
rebuilds the creation JSON by hand from TensorStore specs — including
heuristics the library would not need (units guessing, invented dimension
names), because it lacks access to library internals.

```cpp
// mdio/dataset.h
/// Serializes the LOGICAL schema of an open Dataset — variables (including
/// header variables), dimension labels, dtypes, chunk grids, and user
/// attributes — to the creation JSON consumed by from_json(). Store
/// binding (path, zarr_version, tensorstore Context) is NOT part of the
/// output: from_json keeps taking those as parameters.
/// Round-trip guarantee: from_json(to_json(ds), path, version) opens an
/// equivalent dataset at `path`.
Result<nlohmann::json> Dataset::to_json(
    IncludeDefaults defaults = {}) const;
```

Design notes:

- Build on `Variable::get_spec()` (variable.h:1406) and translate to the
  MDIO creation schema: dtype names, compressor normalization, `chunkGrid`
  from chunks, `dimension_names` from domain labels, `UserAttributes`
  round-tripped from stored array metadata.
- Header variables are part of the round-trip: `Dataset` carries a
  `HeaderVariableCollection` (dataset.h:172, opened at dataset.h:969-983)
  and `CommitMetadata` already serializes them via `ToCommitJson()`
  (dataset.h:1331-1333) — `to_json()` must use the same path, or the
  round-trip guarantee fails silently for datasets with header variables.
- Struct-array round-trip has explicit prerequisites outside this plan:
  the spec derivation for structured variables currently yields `"byte"`
  (rejected by the creation schema — companion evaluation, Issue 02), and
  the write-dialect mismatch `"struct"` vs `"structured"` (zarr-python
  #2134, Issue 01) must land first. Until then the struct-array round-trip
  test is prerequisite-gated, not expected to pass.
- Output must validate against `dataset_schema.h` and open in mdio-python.
- No heuristics: the library has the real metadata; the downstream prototype
  guessed because it did not.

Tests: round-trip open → to_json → from_json → sampling validation; v2 and
v3 stores; datasets WITH header variables; struct arrays (prerequisite-gated,
see above).

Acceptance: downstream dataset copy ≤60 lines (from 224) with byte-identical
output to the current workaround; the downstream `variable_spec_builder`
(~460 lines) is deleted.

Size: ~300–400 lines + tests. Strongest single PR candidate.

## M2 — Chunk iteration: `Variable::chunks()`

Every downstream reader hand-rolls the same triple loop with edge clamping.
A 985-line draft iterator exists downstream and can be ported.

```cpp
// mdio/chunk_iterator.h (new header — variable.h is ~2000 lines already)
/// STL-compatible forward iteration over the variable's chunk grid.
/// Yields Box<> with absolute domain indices; edge chunks yield partial
/// boxes. Range-for support.
class ChunkRange { ChunkIterator begin() const; ChunkIterator end() const; };
ChunkRange Variable::chunks() const;
```

Design notes:

- Derive the grid from `dimensions()` (variable.h:1141) +
  `get_chunk_shape()` (variable.h:1433).
- Boxes are absolute w.r.t. the index domain (see "Domain origin semantics"
  below — domains are 0-based today).
- Edge chunks: partial boxes, never out-of-bounds indices.
- Policy — what the API exposes: the PHYSICAL chunk grid of the stored
  format. v2/v3 differences are normalized by `get_chunk_shape()`
  (variable.h:1453-1478); stores without `chunkGrid` fall back to
  single-chunk-per-array (dataset_factory.h:529-547). Traversal order is
  row-major over the grid and is NOT a contract. Rechunk/reformat changes
  the iteration; that is intended.
- This milestone is API ergonomics, not an I/O optimization: the read path
  stays `tensorstore::Read(store)` (variable.h:1078-1083). The measured
  per-task cost is M7's scope; M2's iterator is the substrate M7's bulk
  API builds on. Do NOT adopt one-box-per-chunk as the default read
  pattern — the evaluation measured 383 small tasks at 56 min vs 0.75 s
  single-task on NFS.

Tests: shapes not divisible by the chunk shape; rank 1–4; 0-sized dims
rejected; domains with offset (post-`isel` — see fix 4).

Acceptance: downstream trace reader ≤120 lines (from 358) with no manual
chunk loops, byte-identical output to the current workaround.

## M3 — Value-based selection: complete `Dataset::sel()`

`Dataset::sel(Descriptors...)` already exists (dataset.h:618-867) and
already implements selection by coordinate VALUE for all three descriptor
kinds — `ValueDescriptor` (dataset.h:704-734), `ListDescriptor`
(dataset.h:735-765), and `RangeDescriptor` with value→index conversion
(dataset.h:766-864) — with tests (dataset_test.cc:678-919; the full
`Dataset.sel*` suite passes at `fcbfb85`, `selList` included).

One gap remains, plus one cleanup:

1. Range endpoints require an exact value match ("Start value not found",
   dataset.h:819-821); mdio-python `sel(inline=slice(a, b))` selects the
   nearest contained values instead.
2. Cleanup: the `ListDescriptor` guard in `sel`'s validation lambda
   (dataset.h:655-683; `UnimplementedError` at :661-662) is dead code —
   the validation loop iterates an `initializer_list`, whose elements are
   const-qualified, so `outer_type<decltype(descriptor)>::type` is
   `const ListDescriptor<T>` and the guard's `if constexpr` never fires
   (compile-time verified: the error string is absent from the test
   binary). The `SliceDescriptor` deprecation check in the same lambda
   dies the same way. Delete the dead checks — as written, the guard
   misleads readers into believing List selection is unsupported.

(The `CoordinateSelector` layer — `UnimplementedError` in `_applyOp`,
coordinate_selector.h:255-257, reachable via `ReadDataVariables` — is a
separate, parallel selection path with overlapping responsibilities. This
milestone works in `Dataset::sel`; consolidating the two layers is a
follow-up decision, not part of M3.)

```cpp
// No new overload. The existing template stays the only sel():
//   template <typename... Descriptors>
//   Result<Dataset> Dataset::sel(Descriptors... descs);  // dataset.h:618
// Work items, in the existing method:
//   - nearest-value endpoint matching for RangeDescriptor;
//   - delete the dead ListDescriptor/SliceDescriptor checks
//     (dataset.h:661-671);
//   - descriptors stay VALUE-typed (RangeDescriptor<T> with T = the
//     coordinate dtype — the trait extract_descriptor_Ttype,
//     variable.h:175-193, already exists); Index-typed descriptors
//     remain isel's domain.
```

Design notes:

- Value→index lookup consolidates in the existing `descriptor_to_index`
  (dataset.h:532-608): add binary search there (coordinates are validated
  monotonic) instead of a third copy of the logic.
- The current implementation reads the full 1-D coordinate variable
  (`var.Read()`, dataset.h:549/790). Keep that for now — chunked or binary
  search over the coordinate is an optimization to revisit with M7's bulk
  reads.
- Endpoint semantics follow mdio-python: nearest contained value; error
  (not clamp) for values entirely outside the range; descending
  coordinates: match mdio-python behavior (verify against xarray `.sel`
  semantics when implementing — do not decide ad hoc in the test).

Tests: nearest-value endpoints (both ends); values entirely outside the
range (error, not clamp); descending coordinates (per mdio-python); the
existing `Dataset.sel*` suite keeps passing unchanged — it is the
regression net for the dead-guard removal.

Acceptance: `sel(inline=slice(a, b))` matches mdio-python's selection on
the same store; the existing `Dataset.sel*` suite passes with the dead
checks removed.

Size: ~2–4 days (the selection machinery already exists and is tested).

## M4 — Statistics: `statsV1` computation

The serialization target already exists (`dataset_schema.h` defines
`CenteredBinHistogram` with `counts`/`binCenters`; `UserAttributes` already
scrubs `statsV1`), and so does the result type:
`mdio::internal::SummaryStats` (stats.h:229-335) has exactly the fields the
on-disk contract needs — `int32_t count`, `float sum/sumSquares/min/max`,
`std::unique_ptr<const Histogram>` (stats.h:329-334; `Histogram` is an
abstract base, stats.h:82-107 — it cannot be a by-value member). What is
missing is only the computation and a merge for partials.

```cpp
// mdio/stats.h — reuse mdio::internal::SummaryStats; do NOT add a second
// struct with the same name and different field types.
/// Incremental (chunk-composable) computation over a variable.
Result<SummaryStats> ComputeStats(const Variable<>& var);
/// Order-independent combination of partials from distributed callers.
Result<SummaryStats> MergeStats(absl::Span<const SummaryStats> partials);
```

Publishing path — no new `UpdateAttributes` overload. The existing
`VariableBase::UpdateAttributes(const nlohmann::json&)` (variable.h:881-889,
returns `Result<void>`) stays the only attribute writer, and per its own
doc comment it does NOT persist; durability goes through
`Dataset::CommitMetadata`. `UserAttributes` already has constructors from
stats collections (stats.h:547-549, 570-573), so the flow is:
`ComputeStats` → serialize to the statsV1 JSON shape → existing
`UpdateAttributes(json)` → `CommitMetadata`.

Policy change, declared: stats.h:69-72 documents that there is no easy path
to add a histogram to an existing `UserAttributes`. This milestone adds one
(via the constructors above) — a deliberate revert of that design note,
called out here so PR review sees it.

Design notes:

- Field types follow statsV1 exactly (int32 count, float accumulators) —
  `double`/`Index` variants would truncate on persistence.
- Schema-compatible with mdio-python `builder/schemas/v1/stats.py`.

Tests: known distributions; empty variables; float32 vs float64; histogram
edges; merge of partials equals single-pass computation.

Acceptance: downstream il/xl reader ≤150 lines (from 532); statsV1 written
by C++ reads back in mdio-python.

Size: ~1 week.

## M5 — Dtype-erased transfer

Largely subsumed by M1 (copy round-trip) + existing Read/Write. The residual
need is element-wise transforms without per-dtype switch statements
(downstream keeps a 628-line dispatch table for this).

```cpp
// mdio/variable.h
/// Element-wise transform during src → dst transfer, dtype-erased.
/// fn receives ONE element of src's dtype as raw bytes (length =
/// src dtype itemsize) and writes the transformed element to dst_bytes.
/// For structured dtypes the unit is the whole record (the versioned
/// struct paths already exist, zarr.h:243-266). Async like the rest of
/// the I/O surface: composes with WriteFutures (variable.h:1127-1128).
using ElementTransform = absl::AnyInvocable<absl::Status(
    std::string_view src_bytes, std::string_view dst_bytes) const>;
Future<absl::Status> TransformVariable(const Variable<>& src,
                                       Variable<>& dst,
                                       ElementTransform fn);
```

Contract notes: the API is a pure per-element map — reversible iff `fn` is;
no cross-element state. Decide after M1 lands how much of this is still
needed.

## M6 — Execution layer (proposal stage)

`MapChunks(variable, fn, ParallelOptions)` over TensorStore futures. Not
scheduled: downstream keeps external orchestration (Parsl/Slurm) until the
core gaps are closed. Draft proposal exists downstream. If it is ever
scheduled, it must specify region fusion/batching, open-handle reuse, and
metadata caching — the evaluation measured the cost in per-task work
(metadata round-trips), not in missing parallelism (K=8 did not degrade),
so a chunk-level scheduler alone addresses the wrong bottleneck (see M7).

## M7 — Metadata-path performance (measured bottleneck)

The evaluation measured the dominant I/O cost in the per-task metadata
path, not in byte transfer: reading 16 chunks issues ~12k metadata syscalls
(`openat`/`getdents64`); per-task cost is linear in the total chunk-file
count (41× between granularity-equivalent grids; additive model
`0.07 s + 0.0025 s × source chunk files + 0.0026 s × accumulated
destination files`); 383 small tasks cost 56 min vs 0.75 s single-task on
NFS. None of M1–M6 touches this path — without this milestone the roadmap
can complete without moving the measured cost.

Deliverables (the evaluation's recorded fix directions):

1. **Direct chunk-key addressing** — construct chunk keys from the stored
   grid instead of directory listing (removes the `getdents64` walks).
2. **In-process metadata lookup cache** — positive and negative entries
   (a failed existence check must not re-hit the store).
3. **Bulk region reads** — a multi-region API on M2's iterator substrate:
   one open-handle set, many boxes (M2's one-box-per-chunk must not become
   the default read pattern).

Performance gates (apply to every milestone that touches the I/O path —
M2, M5, M6, M7): per-region latency, metadata-syscall count per read, and
loss of the linear dependence on source chunk-file count — measured on the
NFS backend. The effect is NFS-metadata-bound; on local SSD it collapses,
so the gates are backend-specific and say so.

Acceptance: the 383-task distributed case drops from 56 min to the
single-digit minutes predicted by the cost model with the metadata term
removed; byte-identical outputs to the current workaround.

Size: large; needs a benchmark harness first (the evaluation's probe
programs are the starting point).

## Small fixes (verified against `fcbfb85`)

Good first PRs, independent of the milestones:

1. **Slice error reports the wrong descriptor.** `Variable::slice` prints
   the ORIGINAL descriptor (`start=1004 > stop=1304` — a false comparison)
   while the check failed on the CLAMPED one. Report the clamped values
   (variable.h, slice loop). *(Implemented, wave 0: `1918f3a`.)*
2. **Missing store misreported as `.zmetadata` parse error.** Opening a
   non-existent store yields "Failed to parse .zmetadata. Try adding a
   trailing slash" because version detection fails silently and the flow
   falls back to v2. Detect the missing `zarr.json` first and say "not an
   MDIO store or path does not exist". *(Implemented, wave 0: `d9a8a41` —
   `DetectVersion` errors when no store markers exist; `from_zmetadata`
   propagates it.)*
3. **Interop: mdio-python 1.x stores lack required dataset metadata.**
   Verified against mdio-python 1.0.8 (probe, 2026-09-14): a fresh
   `to_mdio` write emits no `name`/`apiVersion`/`createdOn` anywhere in
   the root metadata (root `zarr.json` `attributes` is `{}`), while
   per-variable `dimension_names` IS written directly (the legacy
   `_ARRAY_DIMENSIONS` form is converted by the C++ v3 reader anyway,
   zarr_v3.h:769-785 — it is not a gap). The required-field validation
   (dataset_schema.h:368-372) is reachable only on the CREATE path
   (`Construct`, dataset_factory.h:711, via `from_json`); plain
   `Dataset::Open(path)` opens such stores (verified empirically,
   2026-09-16). The evaluation's interop failure fired on the consumer
   flow open → derive creation spec → `from_json`. Round-trip stores
   fail differently: `open_mdio` stamps
   `createdOn` into Dataset attrs (`xarray_builder.py:267`, space
   separator) and `to_mdio` passes it through — the C++ rejects it as
   non-RFC-3339 (companion mdio-python Issue 03, already prepared
   upstream). Policy — lenient reader: `Dataset::Open(path)` warns on
   missing `name`/`apiVersion`/`createdOn` (implemented, wave 0); the
   create path stays strict; stores mdio-cpp itself writes keep the full
   metadata. Contributing
   default metadata writing to mdio-python is a follow-up, not a blocker.
   *(Implemented, wave 0: `9ffa795` — warning at `Dataset::Open(path)`;
   the mechanism text above was corrected from the plan's original claim:
   the read path never rejected the fields, the create path does.)*
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
| 0 | Small fixes 1–4 | trust in error messages; mdio-python interop; domain-origin semantics settled before M2/M3 depend on it |
| 1 | M7 metadata-path + M1 `to_json` | measured I/O cost addressed; ~700 lines deleted; copy programs collapse |
| 2 | M2 chunks (on M7's bulk substrate) | all readers collapse |
| 3 | M3 `sel` completion | ROI selection collapses |
| 4 | M4 stats | readers lose accumulators |
| 5 | M5/M6 | transfer/execution (re-evaluate after M1) |

## Principles

1. **mdio-python parity**: same operation names, same semantics, same
   on-disk results (a store written by either implementation must round-trip
   in the other).
2. **Every PR ships tests** plus a stated downstream acceptance criterion
   (program line-count target or deleted workaround); I/O-touching PRs
   additionally carry a performance gate (see M7).
3. **No breaking changes without a deprecation note** — the API is pre-1.0
   but downstream pins exist.
4. **Interop tests are two-directional** (mdio-python-written stores open in
   C++; C++-written stores open in mdio-python).
