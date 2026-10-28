# API Gap Plan Corrections Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use orquestra:specialist-pipeline (recommended) or orquestra:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Rework `docs/api-gap-plan.md` so every milestone is grounded in the actual base (`fcbfb85`), collides with no existing API, makes its decisions explicitly, and gives the measured metadata-path bottleneck its own milestone — implementing all 21 red-team findings (C1–C7, I1–I10, S1–S4) plus the selList×guard cross-finding.

**Architecture:** Section-scoped edits to a single committed document (`docs/api-gap-plan.md` on `feat/api-gap-plan`). Each task owns whole sections — no overlapping edits between tasks. Verification is grep-based citation checks against the base, plus one build-and-test baseline (Task 1) and one mdio-python metadata probe (already executed; results embedded in Task 8).

**Tech Stack:** markdown (the plan document), cmake + GoogleTest (mdio-cpp test suite), mdio-python 1.0.8 (metadata probe, already run).

## Global Constraints

- Base commit `fcbfb85` (v0.2.0-pre-release), branch `feat/api-gap-plan`. Every `file:line` citation in the corrected plan must be grep-verifiable against this base.
- This plan corrects a DOCUMENT. No library code changes — the milestones it describes are future work.
- The plan document stays in English (repo convention). Corrected section texts below are final — paste them verbatim.
- Temp artifacts (build dir, probe stores) go in `/u/u4w7/tmp`, never inside the repo.
- Commits: imperative subjects, one commit per task, on `feat/api-gap-plan`.
- Red-team findings coverage is the acceptance. Coverage map:

| Finding | Task | Finding | Task | Finding | Task |
|---|---|---|---|---|---|
| C1 (sel exists) | T2, T5 | I1 (header vars) | T3 | S1 (M2 header) | T4 |
| C2 (query() cite) | T5 | I2 (binding) | T3 | S2 (byte-identical) | T3–T6 |
| C3 (Index variant) | T5 | I3 (from_json cite) | T3 | S3 (coord reads) | T5 |
| C4 (SummaryStats) | T6 | I4 (fix 4 wave) | T9 | S4 (unaudited) | T2 |
| C5 (name hiding) | T6 | I5 (fix 3) | T8 | selList×guard | T1, T5 |
| C6 (snippet) | T6 | I6 (domain()) | T4 | | |
| C7 (perf gap) | T2, T4, T9 | I7 (M2 ergonomics) | T4 | | |
| | | I8 (layout policy) | T4 | | |
| | | I9 (M5 contract) | T7 | | |
| | | I10 (12 programs) | T2 | | |

---

### Task 1: Baseline — build the library and run the test suite

Resolves the selList×guard cross-finding (which of the mutually exclusive pair is broken at `fcbfb85`) and gives a mechanical baseline for small fixes 1–2. Long pole: FetchContent builds tensorstore/abseil (30–60+ min). Tasks 2–4 and 6–9 can run while it builds; **Task 5's step 1 gate depends on this task's result.**

**Files:**
- Create: `/u/u4w7/tmp/mdio-cpp-build/` (build dir, outside the repo)

**Interfaces:**
- Produces: the selList verdict (`PASS`/`FAIL`) recorded in `/u/u4w7/tmp/mdio-cpp-build/selList-verdict.txt`, consumed by Task 5 step 1.

- [ ] **Step 1: Configure**

```bash
cmake -S /tgdesenv/src/proj0/u4w7/src/external/mdio-cpp \
      -B /u/u4w7/tmp/mdio-cpp-build -DCMAKE_BUILD_TYPE=Release
```

Expected: configures cleanly; FetchContent downloads tensorstore, abseil, nlohmann/json (long).

- [ ] **Step 2: Build the two test targets**

```bash
cmake --build /u/u4w7/tmp/mdio-cpp-build \
      --target mdio_dataset_test mdio_variable_test -j 32
```

Expected: both targets build (long — tensorstore compiles here).

- [ ] **Step 3: Run the dataset test suite and capture the selList verdict**

```bash
BIN=$(find /u/u4w7/tmp/mdio-cpp-build -type f -name mdio_dataset_test | head -1)
"$BIN" --gtest_filter='Dataset.sel*' > /u/u4w7/tmp/mdio-cpp-build/selList-verdict.txt 2>&1
tail -5 /u/u4w7/tmp/mdio-cpp-build/selList-verdict.txt
```

Expected: `Dataset.selList` **FAILS** — the validation guard (`dataset.h:661-662`) rejects `ListDescriptor` before the List branch runs, while the test asserts success. If `selList` **PASSES**, STOP: the guard analysis is wrong somewhere; re-derive before Task 5 (do not proceed on a wrong premise).

- [ ] **Step 4: Baseline the variable tests (small fixes 1–2 area)**

```bash
BIN=$(find /u/u4w7/tmp/mdio-cpp-build -type f -name mdio_variable_test | head -1)
"$BIN" > /u/u4w7/tmp/mdio-cpp-build/variable-test-baseline.txt 2>&1
tail -3 /u/u4w7/tmp/mdio-cpp-build/variable-test-baseline.txt
```

Expected: full run captured for reference. No verdict depends on it.

---

### Task 2: Header and Gap summary corrections

**Files:**
- Modify: `docs/api-gap-plan.md:5-24`

**Interfaces:**
- Produces: corrected program count (22/10), the unaudited-counts note, gap row 3 rewording, and new gap row 7 — later tasks' section texts reference these.

- [ ] **Step 1: Replace the intro paragraph (lines 5-10)**

Replace:

```markdown
This is the library-side plan for closing the API gaps between mdio-cpp and
mdio-python. The gaps were cataloged by a downstream consumer suite: 12
example programs (~5.5k lines of C++ against ~2k lines of equivalent Python),
where an estimated 60–70% of the C++ code exists only to work around these
gaps. Every item below lists the API to add, where it lands, and a measurable
acceptance criterion expressed as downstream code shrinkage.
```

with:

```markdown
This is the library-side plan for closing the API gaps between mdio-cpp and
mdio-python. The gaps were cataloged by a downstream consumer suite: 22
distinct example programs by the end of the evaluation (10 at the close of
its Phase 0), where an estimated 60–70% of the C++ code exists only to work
around these gaps. Every item below lists the API to add or complete, where
it lands, and a measurable acceptance criterion expressed as downstream code
shrinkage. Downstream line counts quoted per item come from the evaluation
corpus and are not independently audited in this repo.
```

- [ ] **Step 2: Reword gap row 3 and add gap row 7 (lines 17-24 table)**

Replace the row:

```markdown
| 3 | `sel` by value unimplemented | `dataset.sel(inline=slice(a, b))` | full coordinate reads + manual index math | M3 |
```

with:

```markdown
| 3 | `sel`: `ListDescriptor` blocked; range endpoints require exact value match | `dataset.sel(inline=slice(a, b))` | full coordinate reads + manual index math | M3 |
```

and after the row `| 6 | No execution layer | ... | M6 |` add:

```markdown
| 7 | Per-task I/O cost scales with chunk-file count (metadata path) | — (measured, not a parity gap) | distributed jobs: 56 min vs 0.75 s single-task | M7 |
```

- [ ] **Step 3: Verify**

```bash
rg -Un "22\ndistinct example programs|not independently audited|ListDescriptor. blocked|chunk-file count" docs/api-gap-plan.md
rg -c "12 example" docs/api-gap-plan.md   # expected: no match (exit 1)
```

Expected: all four new fragments present; the old count gone.

- [ ] **Step 4: Commit**

```bash
git add docs/api-gap-plan.md
git commit -m "docs: fix counts and gap summary in api-gap plan"
```

---

### Task 3: M1 — ground `to_json` in the actual base

**Files:**
- Modify: `docs/api-gap-plan.md:26-59` (whole M1 section)

**Interfaces:**
- Consumes: nothing from earlier tasks.
- Produces: the M1 section that Task 9's Sequencing row ("M7 + M1") and Task 10's citation check rely on.

- [ ] **Step 1: Replace the whole M1 section (lines 26-59)**

Replace everything from `## M1 — Serialization round-trip: \`Dataset::to_json()\`` through `Size: ~300–400 lines + tests. Strongest single PR candidate.` with:

````markdown
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
````

- [ ] **Step 2: Verify citations against the base**

```bash
rg -n "Result<nlohmann::json> Dataset::to_json" mdio/dataset.h   # expected: no match (the API is proposed, not existing)
rg -n "from_json" mdio/dataset.h | head -3                        # expected: declarations at ~292/340/373
rg -n "ToCommitJson" mdio/dataset.h | head -2                     # expected: ~1331
rg -n "get_spec" mdio/variable.h | head -2                        # expected: ~1406
```

- [ ] **Step 3: Commit**

```bash
git add docs/api-gap-plan.md
git commit -m "docs: ground M1 to_json in the actual base"
```

---

### Task 4: M2 — chunk iteration policy, header, and perf framing

**Files:**
- Modify: `docs/api-gap-plan.md:61-88` (whole M2 section)

**Interfaces:**
- Consumes: gap row 7 from Task 2 (M7 exists as a milestone).
- Produces: the M2 section Task 9's Sequencing ("M2 on M7's bulk substrate") references.

- [ ] **Step 1: Replace the whole M2 section (lines 61-88)**

Replace everything from `## M2 — Chunk iteration: \`Variable::chunks()\`` through `Size: medium-large (draft exists).` with:

````markdown
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
````

- [ ] **Step 2: Verify citations against the base**

```bash
rg -n "dimensions\(\)" mdio/variable.h | head -2      # expected: ~1141
rg -n "get_chunk_shape" mdio/variable.h | head -2     # expected: ~1433
rg -n "domain\(\) const" mdio/variable.h              # expected: no match (exit 1) — no Variable::domain() declaration; store.domain() calls are tensorstore's
rg -n "single chunk per array" mdio/dataset_factory.h # expected: ~529-547 area
```

- [ ] **Step 3: Commit**

```bash
git add docs/api-gap-plan.md
git commit -m "docs: ground M2 chunk iteration in the actual base"
```

---

### Task 5: M3 — rework around the existing, WORKING `sel()` (re-derived)

**Gate resolution (2026-09-14, empirical):** Task 1's build produced
`selList PASSED` — the full `Dataset.sel*` filter passed 18/18
(`selList-verdict.txt`). The planned premise ("the guard blocks
ListDescriptor; test and guard are mutually exclusive") was INVERTED.
Re-derivation (compile-time probe + `strings` on the test binary): the
guard at dataset.h:661-662 is DEAD CODE — the validation loop
`for (auto& descriptor : {descriptors...})` iterates an
`initializer_list`, whose elements are const-qualified, so
`outer_type<decltype(descriptor)>::type` is `const ListDescriptor<T>` and
the guard's `if constexpr` (`is_same_v` against `ListDescriptor<T>`) is
always false; its body is discarded at compile time (the error string is
absent from the test binary). The `SliceDescriptor` deprecation check in
the same lambda dies the same way. List selection WORKS and is tested.

**Files:**
- Modify: `docs/api-gap-plan.md` (gap-summary row 3 + whole M3 section)

**Interfaces:**
- Consumes: Task 1's selList verdict (PASSED) + the probe evidence.
- Produces: the M3 section Task 10's citation check references.

- [ ] **Step 1: Fix gap-summary row 3**

Row 3 currently claims `ListDescriptor` is blocked — false (it works).
In the gap summary table, replace the Consequence-column text of row 3:

- old: `` `sel`: `ListDescriptor` blocked; range endpoints require exact value match ``
- new: `` `sel`: range endpoints require exact value match (nearest-value selection absent) ``

(the rest of the row — mdio-python call, current workaround, M3 — unchanged)

- [ ] **Step 2: Replace the whole M3 section**

Replace everything from `## M3 — Value-based selection: \`Dataset::sel()\`` through `Size: ~3–5 days.` with:

````markdown
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
````

- [ ] **Step 3: Verify citations and the row fix**

```bash
rg -n "Support for ListDescriptor is not yet implemented" mdio/dataset.h    # expected: ~661 (dead guard's text — present but unreachable)
rg -n "Start value not found" mdio/dataset.h                                # expected: ~819
rg -n "not supported" mdio/coordinate_selector.h | head -2                  # expected: ~256
rg -n "extract_descriptor_Ttype" mdio/variable.h | head -2                  # expected: ~175
rg -n "TEST\(Dataset, selList\)" mdio/dataset_test.cc                       # expected: ~812
rg -n "PASSED" /u/u4w7/tmp/mdio-cpp-build/selList-verdict.txt | tail -1     # expected: "[  PASSED  ] 18 tests." (empirical record)
rg -n 'ListDescriptor` blocked' docs/api-gap-plan.md                        # expected: no match (exit 1) — row 3 fixed
rg -Un "const ListDescriptor|dead code|initializer_list" docs/api-gap-plan.md | head -6  # expected: the re-derived M3 claims present
```

- [ ] **Step 4: Commit**

```bash
git add docs/api-gap-plan.md
git commit -m "docs: rework M3 around the existing passing sel"
```

---


### Task 6: M4 — rework stats around the existing `SummaryStats`

**Files:**
- Modify: `docs/api-gap-plan.md:120-147` (whole M4 section)

**Interfaces:**
- Consumes: nothing from earlier tasks.
- Produces: the M4 section Task 10's citation check references.

- [ ] **Step 1: Replace the whole M4 section (lines 120-147)**

Replace everything from `## M4 — Statistics: \`statsV1\` computation` through `Size: ~1 week.` with:

````markdown
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
````

- [ ] **Step 2: Verify citations against the base**

```bash
rg -n "class SummaryStats" mdio/stats.h                    # expected: ~229
rg -n "UpdateAttributes" mdio/variable.h | head -2         # expected: ~881
rg -n "does not commit changes to durable media" mdio/variable.h  # expected: ~878
rg -n "class Histogram" mdio/stats.h                       # expected: ~82
```

- [ ] **Step 3: Commit**

```bash
git add docs/api-gap-plan.md
git commit -m "docs: rework M4 stats around existing SummaryStats"
```

---

### Task 7: M5 and M6 — transform contract and execution-layer framing

**Files:**
- Modify: `docs/api-gap-plan.md:149-167` (M5 and M6 sections)

**Interfaces:**
- Consumes: gap row 7 / M7 existence (Task 2, Task 9).
- Produces: M5/M6 sections consistent with M7's framing.

- [ ] **Step 1: Replace the M5 section (lines 149-161)**

Replace everything from `## M5 — Dtype-erased transfer` through `Decide after M1 lands how much of this is still needed.` with:

````markdown
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
````

- [ ] **Step 2: Replace the M6 section (lines 163-167)**

Replace everything from `## M6 — Execution layer (proposal stage)` through `Draft proposal exists downstream.` with:

```markdown
## M6 — Execution layer (proposal stage)

`MapChunks(variable, fn, ParallelOptions)` over TensorStore futures. Not
scheduled: downstream keeps external orchestration (Parsl/Slurm) until the
core gaps are closed. Draft proposal exists downstream. If it is ever
scheduled, it must specify region fusion/batching, open-handle reuse, and
metadata caching — the evaluation measured the cost in per-task work
(metadata round-trips), not in missing parallelism (K=8 did not degrade),
so a chunk-level scheduler alone addresses the wrong bottleneck (see M7).
```

- [ ] **Step 3: Verify citations against the base**

```bash
rg -n "WriteFutures" mdio/variable.h | head -2   # expected: ~1127
rg -n "IterateOverArrays|struct" mdio/zarr/zarr.h | head -3  # struct paths ~243-266
```

- [ ] **Step 4: Commit**

```bash
git add docs/api-gap-plan.md
git commit -m "docs: define M5 transform contract and M6-M7 framing"
```

---

### Task 8: Small fix 3 — rewrite with probe evidence

The probe was already executed (2026-09-14, mdio-python 1.0.8, venv
`/u/u4w7/.venv`): a fresh `to_mdio` write emits NO `name`/`apiVersion`/
`createdOn` anywhere in the root metadata (root `zarr.json` `attributes`
is `{}`), and per-variable `dimension_names` IS written directly (probe
output: `var dimension_names: ['inline', 'crossline']`,
`_ARRAY_DIMENSIONS: None`). The `createdOn` the evaluation's Issue 03
found comes from the READ path: `open_mdio` stamps it into Dataset attrs
(`xarray_builder.py:267`, `str(datetime)` — space separator) and a
round-trip `to_mdio` passes it through, which the C++ rejects as
non-RFC-3339.

**Files:**
- Modify: `docs/api-gap-plan.md:182-186` (small fix 3 only — fixes 1, 2, 4, and the withdrawn 5 stay as-is)

**Interfaces:**
- Consumes: the probe results above (already on record).
- Produces: the corrected fix 3 with the policy decision made explicit.

- [ ] **Step 1: Replace small fix 3 (lines 182-186)**

Replace:

```markdown
3. **Interop: mdio-python 1.x stores lack required metadata.** mdio-python
   `to_mdio` writes neither the root dataset metadata
   (`name`/`apiVersion`/`createdOn`) nor per-variable `dimension_names`,
   both required by the C++ v3 reader. Either tolerate missing metadata
   with defaults in C++, or contribute the metadata writing to mdio-python.
```

with:

```markdown
3. **Interop: mdio-python 1.x stores lack required dataset metadata.**
   Verified against mdio-python 1.0.8 (probe, 2026-09-14): a fresh
   `to_mdio` write emits no `name`/`apiVersion`/`createdOn` anywhere in
   the root metadata (root `zarr.json` `attributes` is `{}`), while
   per-variable `dimension_names` IS written directly (the legacy
   `_ARRAY_DIMENSIONS` form is converted by the C++ v3 reader anyway,
   zarr_v3.h:769-785 — it is not a gap). The C++ v3 reader requires all
   three dataset fields (dataset_schema.h:368-372), so fresh py-written
   stores fail its required-field validation (the evaluation's interop
   finding). Round-trip stores fail differently: `open_mdio` stamps
   `createdOn` into Dataset attrs (`xarray_builder.py:267`, space
   separator) and `to_mdio` passes it through — the C++ rejects it as
   non-RFC-3339 (companion mdio-python Issue 03, already prepared
   upstream). Policy — lenient reader: the C++ v3 reader tolerates
   missing `name`/`apiVersion`/`createdOn` with defaults and a warning;
   stores mdio-cpp itself writes keep the full metadata. Contributing
   default metadata writing to mdio-python is a follow-up, not a blocker.
```

- [ ] **Step 2: Verify citations**

```bash
rg -n "createdOn" /tgdesenv/src/proj0/u4w7/src/external/mdio-python/src/mdio/builder/xarray_builder.py  # expected: :267
rg -n '"required"' mdio/dataset_schema.h | head -3   # expected: the name/apiVersion/createdOn block ~368-372
rg -n "_ARRAY_DIMENSIONS" mdio/zarr/zarr_v3.h        # expected: ~769-785
```

- [ ] **Step 3: Commit**

```bash
git add docs/api-gap-plan.md
git commit -m "docs: rewrite interop small fix with probe evidence"
```

---

### Task 9: M7 milestone, Sequencing, and Principles

**Files:**
- Modify: `docs/api-gap-plan.md` — insert a new M7 section after M6 (after line 167 post-Task-7); replace Sequencing (lines 211-220 pre-edit); amend Principle 2 (line 227-228 pre-edit).

**Interfaces:**
- Consumes: gap row 7 (Task 2), M2's substrate note (Task 4), M6's framing (Task 7).
- Produces: the final plan structure Task 10 verifies.

- [ ] **Step 1: Insert the M7 section after M6**

Insert after the M6 section:

````markdown
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
````

- [ ] **Step 2: Replace the Sequencing table**

Replace the whole Sequencing table (wave rows 0–5) with:

```markdown
| Wave | Items | Unlocks downstream |
|---|---|---|
| 0 | Small fixes 1–4 | trust in error messages; mdio-python interop; domain-origin semantics settled before M2/M3 depend on it |
| 1 | M7 metadata-path + M1 `to_json` | measured I/O cost addressed; ~700 lines deleted; copy programs collapse |
| 2 | M2 chunks (on M7's bulk substrate) | all readers collapse |
| 3 | M3 `sel` completion | ROI selection collapses |
| 4 | M4 stats | readers lose accumulators |
| 5 | M5/M6 | transfer/execution (re-evaluate after M1) |
```

- [ ] **Step 3: Amend Principle 2**

Replace:

```markdown
2. **Every PR ships tests** plus a stated downstream acceptance criterion
   (program line-count target or deleted workaround).
```

with:

```markdown
2. **Every PR ships tests** plus a stated downstream acceptance criterion
   (program line-count target or deleted workaround); I/O-touching PRs
   additionally carry a performance gate (see M7).
```

- [ ] **Step 4: Verify**

```bash
rg -n "## M7|Small fixes 1–4|performance gate" docs/api-gap-plan.md
rg -c "Small fixes 1–3" docs/api-gap-plan.md   # expected: no match (exit 1)
```

- [ ] **Step 5: Commit**

```bash
git add docs/api-gap-plan.md
git commit -m "docs: add M7 metadata-path milestone and resequence"
```

---

### Task 10: Final consistency pass and verification

**Files:**
- Modify: `docs/api-gap-plan.md` (only if the pass finds issues)

**Interfaces:**
- Consumes: all previous tasks' commits.
- Produces: the verified, coverage-complete corrected plan.

- [ ] **Step 1: Citation sweep — every `file:line` in the plan must resolve**

```bash
# Extract file:line citations from the plan and spot-verify each against the base:
rg -o '\b(dataset|variable|stats|dataset_factory|dataset_schema|coordinate_selector|header_variable|zarr_v3|zarr)\.h:[0-9]+' docs/api-gap-plan.md | sort -u
# For each cited file:line, confirm the cited symbol exists at/near that line, e.g.:
sed -n '661,662p' mdio/dataset.h      # ListDescriptor UnimplementedError
sed -n '229p'   mdio/stats.h          # class SummaryStats
sed -n '881,889p' mdio/variable.h     # UpdateAttributes(json)
sed -n '1406p'  mdio/variable.h       # get_spec
```

Expected: every citation resolves to the cited symbol (±5 lines).

- [ ] **Step 2: Consistency checks**

```bash
rg -n "M7" docs/api-gap-plan.md            # appears in: gap summary row, M6 note, M2 note, M7 section, Sequencing wave 1, Principle 2
rg -n "selList" docs/api-gap-plan.md       # M3 body + acceptance
rg -n "byte-identical" docs/api-gap-plan.md  # M1, M2, M7 acceptances
rg -c "verbatim|CoordinateSelector::query" docs/api-gap-plan.md  # expected: no match (exit 1)
```

- [ ] **Step 3: Coverage check against the red-team findings**

Walk the coverage table in this plan's Global Constraints: each of C1–C7, I1–I10, S1–S4, selList×guard must point at an applied task. Any finding without an applied correction is a plan failure — fix before committing.

- [ ] **Step 4: Commit (only if the pass made fixes)**

```bash
git add docs/api-gap-plan.md
git commit -m "docs: final consistency pass over api-gap plan"
```

---

## Self-review record

- **Spec coverage:** all 21 findings + the cross-finding mapped (table in Global Constraints); no gaps.
- **Placeholder scan:** every corrected section is complete paste-ready text; the only conditional (Task 5's gate) resolves via Task 1's recorded verdict, with the stop condition explicit.
- **Consistency:** section names match the plan's headings (`M1 — Serialization round-trip: Dataset::to_json()` etc.); citations cross-checked against the base during drafting (dataset.h:661/819/292, stats.h:229/82, variable.h:881/1406/1141/1433/175, coordinate_selector.h:256, dataset_test.cc:812, dataset_factory.h:529-547, zarr_v3.h:769-785, dataset_schema.h:368-372, xarray_builder.py:267).
