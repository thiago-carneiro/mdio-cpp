# Robustness Review — mdio-cpp `fcbfb85..d454e3c`

Reviewer: painel reviewer (esforço max), 2026-09-17. Scope: 37 commits
implementing the api-gap plan (docs/api-gap-plan.md) — wave-0 fixes, M1
`to_json()`, M2 `chunks()`, M3 nearest-value `sel` endpoints, M4
`ComputeStats`/`MergeStats`, M5 `TransformVariable`, M7 variable index,
benchmark harness. Focus: error handling, partial failures, malformed
JSON, edge cases, async lifetime, data integrity. Deliberate decisions
(blosc-only creation schema, header-variable `to_json` failure, runtime
monotonicity detection, whole-record units, List fallback) were
respected, not re-litigated.

## Strengths

- **Loud, specific errors at decision points**: `to_json` rejects header
  variables with a pointed message (behavioral test asserts
  `InvalidArgument` + "header variables"); `MergeStats` errors name the
  exact incompatibility (stats.h:1006-1016); blosc-only codec and
  big-endian dtype rejections in the creation schema.
- **Sound async lifetime management in `TransformVariable`**
  (transform.h): destination handle copied into shared state
  (refcounted), source kept alive by the pending read future,
  destination buffer captured by the commit callback — no
  use-after-free window on any exit path; transforms apply sequentially
  on the read-completing thread.
- **Careful numeric hygiene in stats**: double accumulation with a
  single float32 rounding point (StatsAccumulator/CheckedFloat), NaN
  skipped as "missing" (variable.h:2184-2185), ±inf and int32-count
  overflow rejected loudly (stats.h:1031-1039), zero-count partials
  neutral so they can't poison min/max folds (stats.h:926-928).
- **Defensive parsing of foreign input**: `ExtractVariableIndex`
  treats every malformed form as "no index" and falls back to listing
  (zarr_v3.h:262-283, tested at zarr_test.cc:1039-1096);
  `blosc_shuffle_to_string` accepts both legacy int and V3 string forms
  (dataset_factory.h:218-230, tested).
- **Honest test hygiene**: empty variables → canonical empty statsV1,
  partial edge chunks, offset domains, descending/unordered axes,
  repeated values, aborted-transfer partial writes, and a
  decoy-variable behavioral test for the index; the one known
  round-trip gap is an explicit `GTEST_SKIP` pointing at the plan, not
  a silently missing test.

## Issues

### CRITICAL (Must Fix)

**NaN-poisoned coordinate axis silently mis-anchors range selections.**
`classify_coordinate_order` (dataset.h:978, 981) uses `std::is_sorted`,
which is NaN-blind — coords `[1, 2, NaN, 4]` classify as ascending, and
`nearest_contained_index`'s `lower_bound`/`upper_bound`
(dataset.h:1010-1023) then resolve bounds onto the NaN position
(start=3, stop=3.5 both land at index 2). Trigger: a float coordinate
variable containing NaN (nav-gap CDP coords) + `sel(RangeDescriptor)`.
The previous exact-match code errored loudly ("Start value not found");
M3's binary search turns that into **silently wrong data with no
error** — in the core selection API, on inputs the library itself
treats as in-scope (`ComputeStats` documents NaN as "missing"). No test
covers sel on a NaN-containing axis. Fix: in `resolve_range_endpoints`
(dataset.h:1083), guard `if constexpr (std::is_floating_point_v<ValueType>)`
with `std::any_of(..., std::isnan)` → `InvalidArgumentError("coordinate
axis contains NaN")` — or fall back to `resolve_exact_endpoints`, which
is NaN-safe (NaN `==` bound is false → loud "not found"). Add a
regression test: `[1,2,NaN,4]`, range `[3, 3.5]` must error, not return
index 2.

### IMPORTANT (Should Fix)

**Index-path opens silently drop variables whose metadata read fails.**
`BuildVariableSpecs` skips on `!result.ok() || !result->has_value()`
(zarr_v3.h:591) and on unparseable JSON (594); only the
all-candidates-empty case is loud (614-616). Trigger: an mdio-written
v3 store where one indexed variable's `zarr.json` is missing, corrupt,
or unreadable (partial copy, foreign deletion, permission error,
transient NFS failure) → `Dataset::Open` succeeds with the variable
silently absent; pipelines iterating variables process an incomplete
dataset with no signal. The code's own comment says "The index is
authoritative for mdio-written stores" (zarr_v3.h:683-685) — divergence
should surface. The List path legitimately needs the lenient skip
(groups/non-array children); the index path does not. Fix: thread an
`is_index_path` flag through
`ReadChildMetadata`/`OnV3ChildReadsComplete`/`BuildVariableSpecs`; on
the index path, fail the open naming the variable. Add a test: index
lists a variable whose directory is absent → open must fail.

### SUGGESTION (Nice to Have)

- stats.h:132-133, 320-322 (pre-existing, untouched by this diff, but
  M4 makes it hot): `FromJson`'s implicit vector conversions and
  `get<int32_t>()`/`get<float>()` throw uncaught nlohmann exceptions on
  wrong-typed persisted statsV1 instead of returning an error — and
  the exception escapes the open path. Type-check before converting.
- dataset.h:1041-1069: `resolve_exact_endpoints` lacks the start>stop
  guard its monotonic sibling has (1097-1101); flipped endpoints on
  unordered axes still error, but downstream with an index-level
  message. Add the guard for a value-level message.
- zarr_v3.h:239: `ExtractVariableNames` uses const
  `json["kvstore"]["path"]` (UB if absent) + `.get<std::string>()`
  (throws on non-string); use `contains()`/type checks and skip
  malformed specs.
- transform.h: the element callback's byte-count contract is documented
  but unenforced — wrong byte counts overflow the destination buffer; a
  bounds-checked span-based callback would turn that into an error.
- benchmark harness: `std::stoll` on argv without validation →
  uncaught exception on garbage input (tool-only).
- variable.h `chunks(dims...)`: template parameter `DimensionIdentifier`
  shadows the type name — rename.
- dataset.h:188/1465: `WarnOnMissingDatasetMetadata` is wired only into
  the `Open(path)` overload; spec/JSON opens don't warn.

## Recommendations

Land the NaN guard (≈10 lines + test) before merge. Land the index-path
loud failure with it, or explicitly accept it in the plan with a
documenting test. File the `FromJson` hardening as a follow-up — M4's
compute→publish→reopen cycle makes that path routine. Everything else
is non-blocking.

## Assessment

**Ready to merge? With fixes.** The NaN mis-anchoring is a
silent-wrong-data regression in the core `sel` API and must land first;
the index-path silent drop should land with it or be explicitly
accepted. The rest of the change set held up well under adversarial
reading: keep-alive, overflow, empty/edge-chunk, and monotonicity paths
are correct and loudly defended.

## Verification

Full 10,457-line diff read plus targeted source reads of every touched
header and test file; spec conformance checked against the plan's
status notes (all seven milestones present; refuted premises
documented, not silently diverged); tests not re-executed by reviewer
(no execution tooling in this pass — plan status notes record passing
acceptance runs); enforcer N/A (Python-only tool, C++ changes). Cleared
after verification: kvstore path/separator handling, endpoint+offset
arithmetic, `to_json` validate-copy (from_json re-validates),
TransformVariable keep-alive and sequencing, ComputeStats
binning/NaN/inf/overflow, MergeStats (operates on parsed objects — its
`get<int64_t>()` cannot see malformed JSON), ChunkIterator bounds,
DetectVersion, `blosc_shuffle_to_string`, `ExtractVariableIndex`
fallbacks.

## Residual risk (documented deliberate, no action)

Stale variable index on foreign additions — externally added variables
are invisible to index-based opens, and `CommitMetadata` on such a
store regenerates the index without them, perpetuating staleness.
Worth a line in user-facing docs.
