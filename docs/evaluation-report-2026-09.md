# mdio-cpp evaluation report — C++ vs Python rebase evaluation (formato-dados)

**Date:** 2026-09-13
**Subject:** mdio-cpp at pinned commit `fcbfb85`, evaluated by the formato-dados
seismic-processing consumer suite against the Python reference stack
(mdio-python 1.0.8 / xarray / dask).
**Authorship:** produced by a four-specialist team via the Orquestra runtime —
deep-thinker (context and work narrative), code-pipeline (bugs), perf-pipeline
(measurements), doc-pipeline (assembly and editing of this file).

## Abstract

Between September 2 and 13, 2026, the formato-dados evaluation suite rebased its
mdio-cpp consumption onto pinned commit `fcbfb85` (v0.2.0-pre-release base), ported
every example program to Python as a bit-identical parity reference (a battery that
ended at 39 exit-0 checks across 22 programs), and benchmarked both stacks on the
cluster. The verdict: the C++ path wins 1.5×–35.8× on every measured monolithic
workload (the 35.8× SEG-Y-ingest point is partially behavioral — the C++ port skips the
~28 MB structured `headers` variable the Python reference writes); the Python cost is a
flat ~6.7 s per task; and the recommendation inverts
at maximum granularity — at 1 inline per task Python wins (0.75× pre-fix; 0.86× on the
post-fix re-run node) — because the
mdio-cpp per-task cost scales with the total chunk-file count of the touched
datasets, an NFS-metadata effect reduced to an additive cost model. Three confirmed
mdio-cpp defects are documented here with submission-ready upstream texts (issues 01
and 02: structured-dtype metadata dialect, broken in both directions; issue 06: the
per-task cost law), embedded in full in the Bugs section. Submission of the
issues — blocked on an enterprise-managed GitHub account — and the push of the
evaluation branch await user decisions.

## Table of contents

1. Context and Work Narrative
   - What the evaluation was, and its verdict
   - Fase 0 — the rebase onto pinned `fcbfb85`, header patches retired
   - The parity battery and the Python reference ports
   - The API gap plan — the local branch in this repository
   - Benchmark campaigns on the cluster (September 9–10, 2026)
   - Critical-review panel and red team
   - The issues corpus and provenance checks against upstream
   - Issue 04 — the distributed-copy manifest divergence, found and fixed locally
   - Post-fix re-measurement (September 12–13, 2026)
   - Current status (as of 2026-09-13)
2. Bugs
   - Bugs found in mdio-cpp (candidates for upstream issues)
     - Issue 01 — structured-dtype write dialect (`"struct"` vs `"structured"`)
     - Issue 02 — invalid `"byte"` creation spec derived from a py-written structured variable
     - Issue 06 — per-task cost scales with chunk-file count (NFS metadata)
   - Complete upstream issue texts (English, submission-ready)
   - Bugs fixed in the evaluation codebase during the work (evaluation-code bugs, not library bugs)
   - Upstream mdio-cpp bugs verified as already fixed (provenance)
3. Measurements
   - A/B benchmark against the Python reference
   - Family-granularity curve
   - The additive per-task cost model
   - NFS probes: isolating source vs destination file counts
   - Node effects and the py/cpp ratio detector
   - Provenance (raw data)
4. Status and next steps
5. Editorial notes

Reading conventions: paths cited as `formato-dados/…` refer to the evaluation
repository at `/tgdesenv/src/proj0/u4w7/src/formato-dados` (branch
`feature/fase0-rebase-python-port`) — provenance citations only; this report is
self-contained without that repository. Bare `§N` references are sections of its
gap-closing plan (`formato-dados/docs/gap_closing_plan.md`). Raw benchmark data
lives under `/u/u4w7/tmp/fd_*`.

## Context and Work Narrative

This is the complete account of what the evaluation did concerning mdio-cpp: how the
library was pinned, consumed, verified, measured, and reported on between September 2
and September 13, 2026. Bug-by-bug detail and measurement detail are deliberately not
repeated here — every item is named and pointed to the section of this report that
carries it (*Bugs* for the defects and the interop findings, *Measurements* for the
benchmark results).

The actors:

- **mdio-cpp — this repository.** Cloned from `TGSAI/mdio-cpp` on Sep 2, 2026 at
  commit `fcbfb85` (the v0.2.0-pre-release base, Aug 2026), which was still `main`
  HEAD when provenance was re-checked on 2026-09-12 — nothing fixed upstream
  (`formato-dados/issues/README.md`, pre-submission checklist). The only local
  divergence from `main` is the docs branch `feat/api-gap-plan` (3 commits, below).
  No library code was modified locally at any point of the evaluation.
- **The evaluation repository** — `formato-dados` at
  `/tgdesenv/src/proj0/u4w7/src/formato-dados`, cited below as `formato-dados/…`:
  a downstream consumer suite of seismic-processing example programs, built
  incrementally since May 2025, originally against an earlier fork pin
  (brian-michell fork, branch `v0.1.63_latest` @ `457285c`, July 2024 —
  `docs/api-gap-plan.md`, "Domain origin semantics"). All evaluation work ran on its
  branch `feature/fase0-rebase-python-port` (Sep 7–13, 2026).
- **The Python reference** — mdio-python 1.0.8 (zarr 3.1.3, xarray 2025.10.1, dask
  2025.10.0; `formato-dados/issues/README.md`), with the mdio-python clone at
  `/tgdesenv/src/proj0/u4w7/src/external/mdio-python` (`main` @ `a2895b5`) used for
  provenance checks.

### What the evaluation was, and its verdict

The evaluation asked, for this project's seismic-processing dataset workloads (copy,
full scan, gain, distributed copy, header edit/add, schema-driven creation, SEG-Y
ingest, plane read, decimation, dimension reorder): when does the C++ path on
mdio-cpp/tensorstore pay over the Python path on mdio-python/xarray/dask — and when
does it not? (`formato-dados/docs/relatorio_decisao_cpp_vs_python.md`.)

The verdict (decision report dated 2026-09-10):

- C++ wins **1.5× to 35.8×** on every measured monolithic workload. The Python cost
  is a **flat ~6.7 s per task** (interpreter boot, imports, dask graph build and
  execute) — it does not scale with I/O; chaining N Python stages costs ~6.7 s × N
  before moving any bytes.
- The recommendation **inverts at maximum granularity**: at 1 inline per task (383
  tasks) Python is faster (0.75×). The C++ per-task cost explodes there (0.72 s at
  coarse granularity to an 8.83 s run average, growing over the run), for a reason
  that became issue 06 (below).
- The sweet spot of the distributed family mode is **family ≥ source chunk** (here,
  48 inlines): C++ at its best (8.9×–10.7×), whole-dataset copies in seconds.
- Costs invisible in seconds: the pinned mdio-cpp has three structured-dtype
  **metadata-dialect** incompatibilities with the Python path (issues 01/02/03
  below) — never data problems (chunks are byte-identical), always zarr-v3 metadata
  dialect.
- Recommendations: monolithic or coarse tasks → C++ (no measured exception); many
  thin tasks → coarsen the family or stay in Python; consolidate Python stages
  before switching languages; keep one structured-dtype dialect per dataset (the
  Python side is the interoperable one today); SEG-Y ingest is the biggest C++ win
  (35.8×).
- Scope: ~110–119 MB datasets (383×304×256 post-stack; 96×152×8×256 pre-stack seed),
  shared NFS, one node, K ≤ 8 — larger volumes may shift the inversion points.

Full tables, methodology, and raw-data paths (`/u/u4w7/tmp/fd_*`) live in the
*Measurements* section of this report.

### Fase 0 — the rebase onto pinned `fcbfb85`, header patches retired

The consumer suite had been consuming mdio-cpp through post-fetch header-patch
scripts. Fase 0 of the gap-closing plan
(`formato-dados/docs/gap_closing_plan.md` §2), executed Sep 7–8, 2026 (commit
`f5b3c45`, "build(mdio): Pin mdio-cpp to fcbfb85 and drop header patch
scaffolding"), ended that:

- `GIT_TAG` pinned to `fcbfb85` in the root `CMakeLists.txt` (it had been `main`;
  upstream API churn — `get_variable` → `variables.get` — had already broken the
  build once, hence the standing rule: pin, never `main`; §1).
- The patch scripts (`fix_mdio_inlines.sh`, `fix_mdio_pointers.sh`) and the
  commented `PATCH_MDIO` block were removed because the underlying bugs had been
  **fixed upstream**: `18b7540` (inline/pointers), `b5e42fc` (non-zero-origin
  clamp), `52678bd` (refcount), `535f7d1` (slice dropping elements) (§2, §12).
- The changed API surface was adapted — `dataset.get_variable<T>(name)` →
  `dataset.variables.get<T>(name)` / `.at(name)` — across 5 programs, and
  `copy_validation` was re-run against a copy to confirm equivalence.

Result: 100 % patch-free compilation and equivalence held; 9 of 10 programs passed
the re-executed battery (Sep 8). The one segfault (`trace_reader_il_xl`) was the
program's own undefined behavior — an `ArrayView` bound to a temporary
`StridedLayout` — not a tensorstore regression; fixed in the rebase with a named
layout (commit `47844ff`), exit=0 after. The one remaining failure
(`indexed_slicing_and_trace_selection`) was a pre-existing program bug from Sep
2025 exposed by the battery, later fixed in Fase 2 (commit `cff82bb`) (§2 status).
Two hard rules came out of Fase 0 and held for the rest of the evaluation: **no
header patches, ever**, and **version pin, never `main`** (§1).

One suspected library bug was withdrawn during this phase: an apparent tensorstore
`IterateOverArrays` regression (2-D segfault) was refuted on Sep 8, 2026 while an
upstream issue was being prepared — the "minimal reproducer" had replicated the
consumer program's own UB. No upstream issue was filed; the withdrawal is recorded
as Small fix #5 in `docs/api-gap-plan.md`.

### The parity battery and the Python reference ports

Every example program was ported to Python as the reference implementation (commit
`7ba2072`, "feat(python): Port MDIO example programs to Python"), with parity
defined as **bit-identical outputs** (`np.array_equal` on all variables). The
in-repo battery (`scripts/battery.sh`) runs 10 targets — the nine programs plus the
distributed flow stage-by-stage (init, one task per manifest entry, verify) — and
reached 10/10 exit=0. (A panel correction, below, made this claim precise: the
earlier "10/10" had counted the distributed flow verified outside the script via
the Parsl driver; after the correction the script itself runs all 10 targets —
`formato-dados/docs/gap_closing_plan.md` §14.)

The battery grew with the backlog pairs until the end of the evaluation:
`create_from_schema` (§15), `segy_ingest` — a native C++ rev1/IBM byte-level reader
(§16), later completed with the structured `headers` variable (§20) — `plane_reader`
and `decimate` (§23-adjacent commits), family mode (§17), and the reorder pair
(§23). Final count: **39 exit-0 checks across 22 distinct programs** (39/39;
`formato-dados/issues/05_mdio_python_to_mdio_write_grid_from_stale_encoding.md`,
"Experimentos que confirmam" item 6). Pure helpers
extracted along the way got Catch2 unit tests in `formato-dados/tests/` — 42 cases
/ 16,647 assertions by the end, including property sweeps of the partition split
and the permute fast paths (§14 adendo, §17, §23). Note: the vendored mdio forces
its own suite into CTest, and `mdio_acceptance_test` has one **known upstream
failure at the pinned commit** (`fillValueParityWithPython/V2`) — not ours (§14).

Fases 1–5 of the plan were executed in full on the branch (commits `768db1a`
ChunkIterator, `bd29797` CopyDataset, `31ed3e1` ComputeSummaryStats, `7e98df4` +
`7f77c85` validation fixes, `cff82bb` ValueToIndex, `d63e6fe` local parallel layer,
`9fb4df0` distributed reduction, `4bef091` SEG-Y removal; §13). Five real bugs in
the consumer suite were found and fixed during execution — the fail-open sampler
(v3 dtype location), the `trace_gain` dtype-spelling guard, value-as-index ROI
selection, the always-zero validation count, and the main-thread write
amplification — each detailed in the *Bugs* section of this report (§13).
Three strategic decisions were recorded (§9, §13): upstream contribution =
**document only** (no PRs or issues opened by the initiative); SEG-Y = **Option A,
hybrid** (`segy_to_mdio.cpp` removed; ingestion stays in Python with templates; C++
processes MDIO — the later native `segy_ingest` pair is a benchmark/example, not a
reversal); multi-node = **external orchestration** (the existing Parsl/Slurm
driver), no own execution layer.

### The API gap plan — the local branch in this repository

The library-side counterpart of the consumer work is `docs/api-gap-plan.md`, in
this clone, on branch `feat/api-gap-plan`: **3 commits on top of `fcbfb85`** —
`5d3f1dd` (add the plan), `4e6a61b` (correct the domain-origin-semantics item),
`9046367` (withdraw the tensorstore regression item). This docs branch is the
entire local divergence from `main`.

The plan catalogs the six API gaps the consumer suite measured (no `to_json`
round-trip, ~700 downstream lines of hand-built creation JSON; no chunk iteration;
`sel` by value unimplemented; no `statsV1` computation; no dtype-erased transfer;
no execution layer), each with a proposed API (M1–M6), tests, and a downstream
acceptance criterion expressed as code shrinkage, plus sequencing waves and
principles (mdio-python parity in names, semantics, and on-disk results; every PR
ships tests; no breaking changes without deprecation; two-directional interop
tests). It also lists small fixes verified against `fcbfb85`: the slice error
reporting the wrong (unclamped) descriptor; a missing store misreported as a
`.zmetadata` parse error; mdio-python 1.x stores lacking metadata required by the
C++ v3 reader (root `name`/`apiVersion`/`createdOn`, per-variable
`dimension_names`); and the domain-origin semantics (0-based open domains, sliced
variables carrying offsets — the value-vs-index conflation behind the `b5e42fc`
clamp fix). Under the conservative upstream decision, the plan is a catalog for
future contribution — nothing in it was submitted.

### Benchmark campaigns on the cluster (September 9–10, 2026)

All cluster campaigns ran on dedicated compute nodes of the quati cluster via
Slurm (AMD EPYC 9334, shared NFS), with warmup + rotated rounds, medians, a drift
control, and — for the parallel and reorder rounds — predictions written before
measuring:

| Campaign | Job | Node | Scope |
|---|---|---|---|
| Definitive A/B round | 421150 | bw33b03n01 | 10 workload pairs (21 candidates incl. control 0.99×); 1 warmup + 5 rotated rounds |
| Family-granularity curve | 421156 | bw33b10n34 | families 1/8/48/383 (383/48/8/1 tasks), full flow, one process per task |
| Parallel execution | 421182 | bw33b10n34 | partition stage at K = 2/4/8 concurrent tasks, both languages |
| Mechanism probe | 421183 | bw33b10n34 | two-arm probe of the C++ per-task cost explosion |
| Reorder round | 425001 | bw33b03n01 | 3 reorder pairs added (13 pairs = 26 candidates + control in the final script) |

The A/B benchmark (`scripts/benchmark_cpp_vs_python.sh`) was first registered on
Sep 9 (gap-closing plan §14 adendo), extended with the distributed and header
pairs (commit `b89b236`), then run definitively as job 421150. Headline gaps
(medians): copy 3.3×, reader 10.0×, gain 3.3×, distributed 7.8×, header_edit 1.5×,
header_add 3.3×, create 6.2×, segy_ingest 35.8×, plane_reader 14.4×, decimate
4.9×; CVs ≤ 7 % except cpp_copy (9 %), cpp_create (21 %) and cpp_segy_ingest
(13 %) (§18). The granularity curve found the family=1 inversion (Python 0.75×)
and the sweet spot at family ≥ 48 (§19). The parallel round showed both stacks
scaling near-linearly to K=8 (speedups 7.5–8.6×) with the inversion persisting,
narrowed (Python 1.25× faster at family=1, K=8) (§21). The mechanism probe
established that the C++ per-task cost scales with the **total chunk-file count**
of source and destination — an additive model (≈ 0.07 s + 0.0025 s × source files
+ 0.0026 s × accumulated destination files) fits the entire granularity curve,
task-level regression n=222, R² ≈ 0.99/0.96 (§22) — the finding that became
issue 06. The reorder round measured the transposition pairs: post-stack 2.5×,
pre-stack 7.1–7.6×, control 1.01× (§24). Two documented incidents (detail in
*Measurements*): the mechanism probe's first attempt, inside job 421182, failed
on a bash indirect-expansion bug in the harness and was re-run as job 421183 with
`--dependency=afterok`; and a local rebuild during job 421150 corrupted one
`cpp_distributed` repetition (exit 126), excluded from the medians.

### Critical-review panel and red team

**Panel (Sep 9, 2026).** A compact heterogeneous review panel — four reviewer
seats on different models via the local bridge, plus a factual auditor; full
registry in
`formato-dados/.orquestra/pipeline/revisao-gap-closing-20260909/findings.md` —
reviewed the gap-closing execution. Verdict: **sustainable with conditions**. Four
of the five recommended conditions were executed the same day (commits `eee5a59`,
`e52074d`): (1) the battery and a synthetic-dataset generator moved in-repo
(`scripts/battery.sh`, `scripts/generate_synthetic_dataset.py`); (2) strict
manifest parsing — fail-open became fail-loud (missing/invalid `dim`/`start`/`end`
now error; a range selecting zero chunks errors); (3) unit tests for the extracted
pure helpers (Catch2, initially 22 cases); (4) the "10/10" wording corrected. The
fifth condition — pushing the branch — was left to the user. Out-of-scope findings
were registered but not executed (relative manifest intervals vs absolute
`ChunkIterator`; `BuildDataType` throwing on v3 structured dtypes; worker threads
without try/catch; the `trace_gain` validator accepting only float; a fail-loud
false-negative on structured-only datasets) (§14).

**Red team of the issues corpus (Sep 12, 2026; corrections in commit `4b4e7ec`).**
Issue 04 was reclassified as internal (below); issue 06's caveat that "f8/f48 run
2–4× above the prediction" was shown to be a label-swap artifact — with correct
labels the additive model fits the whole curve; the battery count "22 targets" was
reformulated (39 checks / 29 log sections / 22 distinct programs); and the
granularity-curve per-task derivations were corrected. Separately, the
mechanism-probe conclusions had been corrected after an evidence review (commit
`b23e349`): an earlier version refuted the destination-growth hypothesis on a
miscounted file inventory ("~100 files" vs 850–1,700 actual) — with the correct
count the hypothesis stands (§22, correction note).

### The issues corpus and provenance checks against upstream

Seven confirmed issues were collected, one file each
(`formato-dados/issues/`), each with the proposed upstream text (English, with
reproducer and verbatim error messages), candidate solutions, and the confirming
experiments. Three concern mdio-cpp:

- **01** — mdio-cpp writes structured dtypes in its own dialect (`"name":
  "struct"`, fields as objects, fill_value as object, no blosc); zarr-python
  cannot even open the dataset (cpp → py interop).
- **02** — mdio-cpp opens a py-written structured-dtype dataset, but the creation
  spec it derives emits `"byte"`, rejected by its own schema — cross-dialect copy
  is broken in `distributed_copy_init` (py → cpp interop).
- **06** — per-task cost scales with the total chunk-file count of the touched
  datasets (NFS metadata), not with data read: 41× at 6,128 files in the
  autonomous reproducer, ~0.0024 s per accumulated destination file.

(03 and 05 target mdio-python — the `createdOn` space separator that mdio-cpp
correctly rejects, and the stale `encoding["chunks"]` grid after transpose; 07, the
collateral xarray `region=` finding, is a future candidate — see below.) Issues
01/02 are metadata dialect only — data chunks are byte-identical across dialects.

**Provenance checks (Sep 12, 2026; commit `7f53705`)**, per the pre-submission
checklist (`formato-dados/issues/README.md`): (1) target `main`/HEAD checked for
every issue — mdio-cpp `main` == `fcbfb85`, nothing fixed; mdio-python `main` still
carries the 03 bug (`xarray_builder.py:267` uses `str()`) and the 05 mechanism;
(2) duplicate searches across the mdio-cpp, mdio-python, zarr-python and
tensorstore trackers — none found; the mandatory cross-link was identified:
zarr-python **#2134** "[v3] Structured dtype support", the shared root cause of
01/02; (3) exact versions registered in every issue text (mdio-cpp commit; pip
freeze of the Python env); (4) autonomous reproducers — minimal in-text
reproducers for 01/02/03/05, and for 06 a standalone C++ reproducer
(`formato-dados/issues/repro/repro_nfs_file_count.cpp`, mdio-cpp + abseil +
nlohmann/json only), validated live: 41× isolated (11.2 s vs 0.27 s for the same
single-inline read), identical checksums, mechanism confirmed by strace (~12,000
metadata syscalls per read: 8,072 `openat` — 6,137 of them failed existence
probes — plus 3,849 `getdents64`); (5) submission strategy — 01 and 02 share a
root cause in two directions, so they must be cross-linked, not duplicated; 02
depends on the 03 workaround (declared in its text); filing is to be escalated,
not dumped (a suggested order exists), from a pre-release pin.

### Issue 04 — the distributed-copy manifest divergence, found and fixed locally

The two distributed-copy implementations serialized their partition manifests with
different top-level JSON shapes: the C++ stage binaries
(`distributed_copy_init`/`distributed_copy_partition`) wrote a top-level **array**
of partition descriptors; the Python driver wrote a top-level **object** with
`source`/`dest`/`dimension`/`inline_chunk`/`partitions`. Cross-consumption failed
in both directions with unhelpful errors ("Manifest must be a non-empty array" /
"list indices must be integers or slices, not str"), confirmed bidirectionally on
Sep 12 with real manifests from probe job 421183
(`formato-dados/issues/04_distributed_manifest_shape_divergence.md`).

The red team reclassified it as **internal**: the manifests are written by this
repo's example programs, not by the mdio libraries — an upstream filing "would die
on the first comment". It was fixed in-repo the same day (commit `be23c69`): a
unified `distributed-manifest/1` format (object with `format`/`source`/`dest`/
`dimension`/`family_size`/`partitions` with per-variable entries). The C++ side
converged — init/partition now write the new format and still accept the legacy
array; the Python side writes and consumes the unified form (and rejects
`transfer: "chunk"` loudly — chunk mode is the C++ stage's domain).
Cross-consumption was verified in both directions and the full battery passed
39/39. A collateral discovery made during the fix — `to_zarr` with `region=`
silently drops index coordinates (xarray behavior) — was minimally reproduced and
recorded as issue 07, a future upstream candidate for pydata/xarray.

### Post-fix re-measurement (September 12–13, 2026)

Because the fix also changed the benchmark scripts' manifest parsing, the affected
measurements were re-run (commits `b178f3d`, `3e093b2`; evidence in
`/u/u4w7/tmp/fd_bench_postfix/` and `/u/u4w7/tmp/fd_family_smoke_postfix/`):

- **Full A/B re-run** (Sep 13, login node gr15sdes01): 5 rounds, 26 candidates,
  control 1.02× (gate 1.15×), exit 0. Both stacks ran systematically faster than
  the definitive round (node factors 0.69–0.95× for 19 of the 20 shared
  candidates; the one exception, `cpp_segy_ingest`, ran 1.48× — slower on the
  shared login node, a node artifact) — a node effect, not the fix;
  `py_distributed` — the only candidate whose data path changed — shifted 0.77×,
  in line with the other Python candidates (0.71–0.95×), i.e. no anomalous
  regression; the ratios reproduce the report (copy 3.5×, reader 10.4×,
  segy_ingest 17.3×, plane_reader 19.6×, distributed 8.7×).
- **Granularity smoke** (FAMILIES=8: init → 48 tasks → verify, both languages):
  exit 0.
- **Full granularity curve re-run** (Sep 13, job 28807, node gr15b01n02; original:
  job 421156 on bw33b10n34): 8/8 combos, exit 0. Both stacks shifted together
  (C++ 0.73–0.89×, Python 0.77–0.84×) — again a node effect; the Python/C++ ratios
  show no systematic direction (f1 0.75→0.86, f8 4.28→4.78, f48 9.35→9.83, f383
  9.54→8.25). The Python family flow is structurally immune to the fix (one
  variable per task before and after); the per-variable manifest openings only
  affect cross-language consumption.

**Verdict: no structural change** — the fix changed the manifest format, not the
measured behavior. One collateral finding was registered in issue 06: the additive
model's per-file coefficients are node-specific (they measure NFS metadata
latency) — on gr15b01n02 the model over-predicts the metadata-dominated points by
19–27 % while the structure holds, which is itself evidence that the cost is
NFS-metadata-bound.

### Current status (as of 2026-09-13)

- **Upstream issue texts are prepared and paste-ready** for 01, 02 and 06
  (mdio-cpp), plus 03 and 05 (mdio-python) and 07 (xarray, not red-teamed): final
  bodies, exact `gh issue create` commands, and a suggested escalation order (03
  first; then 01 + 02 cross-linked; then 05; then 06; then 07) are in
  `formato-dados/issues/upstream/`, one file per issue.
- **Submission is blocked on credentials, not content**: this environment's GitHub
  account is an EMU (Enterprise Managed User) and cannot create issues outside the
  petrobrasbr-exp enterprise (GraphQL error: "As an Enterprise Managed User, you
  cannot access this content"). Filing requires a personal account — a user
  decision.
- Also open with the user: pushing the evaluation branch
  `feature/fase0-rebase-python-port` (the panel's fifth condition). The closing
  stretch of the work trail, the issues-corpus phase (Sep 12–13), is eight
  commits: `3197887` (collect confirmed issues into `issues/`), `4b4e7ec` (red
  team corrections), `7f53705` (provenance checks + issue 06 reproducer),
  `be23c69` (manifest convergence, issue 04), `757c6f2` (issue 04 marked
  resolved), `301e239` (issue 07 + paste-ready upstream bodies), `b178f3d`
  (post-fix benchmark re-validation), `3e093b2` (full post-fix granularity
  verdict).
- In this repository, the evaluation leaves behind one committed artifact — the
  `feat/api-gap-plan` docs branch (3 commits on top of `fcbfb85`) — plus this untracked
  report; the three prepared upstream issue texts (01, 02, 06) live in the formato-dados
  corpus, awaiting a personal account to be filed.

## Bugs

This section is the evaluation's consolidated bug record, in four parts:
defects found in mdio-cpp itself at the pinned commit (with submission-ready
upstream issue texts), bugs found and fixed in the evaluation's own code while
the parity battery was being built, and mdio-cpp bugs from the header-patch
era verified as already fixed upstream at the pin.

Unless absolute, source paths are relative to the evaluation corpus root
`/tgdesenv/src/proj0/u4w7/src/formato-dados/`. mdio-cpp was pinned at `fcbfb85`
(v0.2.0-pre-release); the 2026-09-12 pre-submission checks confirmed
`main`/HEAD == `fcbfb85` — nothing reported in the first subsection was fixed
upstream at evaluation time (`issues/README.md`).

### Bugs found in mdio-cpp (candidates for upstream issues)

Three confirmed mdio-cpp defects, all reproduced against the pin and all
absent from the upstream trackers (duplicate search 2026-09-12 across
mdio-cpp, mdio-python and zarr-python: no duplicates — `issues/README.md`).
None has been submitted yet: the GitHub account in this environment is an
Enterprise Managed User that cannot file issues outside the enterprise, so the
final paste-ready bodies wait in `issues/upstream/` (`issues/README.md`,
"Submissão upstream — estado"). The corpus also holds companion candidates
against mdio-python (03: `createdOn` written with a space separator; 05: stale
chunk grid after `transpose`) and xarray (07: `to_zarr(region=)` silently
drops indexed coordinates); those are out of scope for this report's mdio-cpp
perspective.

#### Issue 01 — structured-dtype write dialect (`"struct"` vs `"structured"`)

Source: `issues/01_mdio_cpp_struct_dtype_write_dialect.md`; submission text:
`issues/upstream/01_mdio-cpp_struct_dtype_write_dialect.md`.

**What happens.** When mdio-cpp writes a variable with a structured dtype —
e.g. a SEG-Y trace header variable: 89 fields, 232-byte packed records — the
zarr v3 metadata it emits uses a private structured-dtype dialect that
zarr-python does not recognize. The dataset cannot even be **opened** by the
Python side:

```
ValueError: No Zarr data type found that matches {'configuration': {'fields': [{'data_type': 'int32', 'name': 'trace_seq_num_line'}, ...]}, 'name': 'struct'}
```

**The two dialects side by side** (same variable, same chunk bytes, written by
each stack):

| Aspect | mdio-cpp writes | mdio-python / zarr-python expects |
|---|---|---|
| type name | `"struct"` | `"structured"` |
| field encoding | objects `{"data_type": "int32", "name": "..."}` | arrays `["field_name", "int32"]` |
| codecs | `["bytes"]` (no blosc) | `["bytes", "blosc"]` |
| `fill_value` | object `{"field": 0, ...}` | base64 string `"AAAA..."` |

**Root-cause context.** zarr v3 currently has no portable structured dtype:
zarr-python 3.1.3 implements `"structured"` as a zarr-specific extension
(`zarr/core/dtype/npy/structured.py:106`, `_zarr_v3_name: Literal["structured"]`;
line 68: "This representation is not currently defined in an external
specification" — tracked in zarr-developers/zarr-python#2134). Both stacks
write private dialects; mdio-cpp's happens to be a *different* private
dialect. Issue 02 below is the same root cause surfacing on the read/derive
side.

**Not a data problem.** During the headers port (commit `cdd4231`), the
`headers` variable's chunks were compared byte-for-byte after blosc
decompression — identical in both dialects (chunks (0,0) and (2,2)); field
probes (89 fields × 4 traces) checked the values. The difference is metadata
only, and numeric variables are unaffected (chunk bytes verified bit-identical
between the stacks).

**Impact.** Cross-stack consumption is impossible for structured variables: a
dataset written by the C++ ingest cannot be opened by any zarr-python-based
consumer.

**Fix directions** (full text in the next subsection): align the write dialect
with zarr-python's `"structured"` (name, array-form fields, base64 fill_value,
blosc codec entry — the highest-value fix); or, if a portable zarr v3
structured dtype is genuinely unavailable, fail loudly at write time; the
documented consumer-side workaround is separate numeric variables or one stack
per dataset.

**Provenance.** Reproduced 2026-09-12 (login node, venv `/u/u4w7/.venv`,
binary `build/bin/Release/segy_ingest`); mdio-cpp `main`/HEAD == `fcbfb85`
(nothing fixed upstream); duplicate search found none; when submitting, the
mandatory cross-link is zarr-python #2134 "[v3] Structured dtype support"
(open) as the shared root cause — related but distinct domain issues:
mdio-python #865 (ingest rev2, ScalarType S8) and mdio-python #582 (closed;
structured + `_FillValue` in zarr v2). Exact versions: mdio-python 1.0.8,
zarr 3.1.3, xarray 2025.10.1, dask 2025.10.0.

#### Issue 02 — invalid `"byte"` creation spec derived from a py-written structured variable

Source: `issues/02_mdio_cpp_struct_dtype_byte_spec.md`; submission text:
`issues/upstream/02_mdio-cpp_struct_dtype_byte_spec.md`.

**What happens.** The mirror of issue 01 on the read/derive side. mdio-cpp
**can open** a dataset written by mdio-python that contains a
structured-dtype variable (the `"structured"` dialect parses fine), but the
creation spec derived from that variable is invalid — the data type comes out
as `"byte"`, which mdio-cpp's own creation-spec schema rejects:

```
INVALID_ARGUMENT: Validation failed, here is why: At /variables/4/dataType of "byte" - no subschema has succeeded, but one of them is required to validate. Type: anyOf, number of failed subschemas: 2
```

Variable 4 is `headers` (variables 0–3: inline, crossline, time, amplitude).
The failure surfaces when building a new dataset from the opened one
(open → derive creation spec → `Dataset::from_json`), so cross-dialect copies
of structured variables are broken in this direction too.

**Reproducer subtleties.** Two notes for a self-contained reproducer:

1. Normalize `createdOn` first — mdio-python 1.0.8 writes it with a space
   separator (companion corpus issue 03, an mdio-python candidate), which
   mdio-cpp's dataset-level validation rejects BEFORE the variable-level error
   surfaces; without that step the reproducer shows the wrong failure.
2. The core defect is independent of any application code: the spec mdio-cpp
   itself derives for a structured variable it just opened comes out `"byte"`
   — the loss happens inside the library's spec API, not in the caller's
   derivation loop.

**Error ordering.** Without the `createdOn` normalization, issue 03's error
appears first (metadata validation precedes variable validation); the two
issues are independent and were confirmed separately in the same reproducer.

**Impact / workaround.** Cross-dialect copy/reorder of datasets containing
structured variables fails in mdio-cpp. Workaround in use: keep structured
variables on one stack per dataset (the Python side is the interoperable one
today).

**Provenance.** First observed during the headers port (commit `cdd4231`) in
the cross-dialect copy test; re-produced 2026-09-12 with a minimal reproducer
(scratch `/u/u4w7/tmp/repro_createdon`: opens the dataset, calls
`tg::utils::copy::BuildDatasetCreationSpec` and `mdio::Dataset::from_json`,
no workaround). mdio-cpp `main`/HEAD == `fcbfb85`; no duplicates; when
submitting, cross-link zarr-python #2134 and issue 01 (shared root cause —
cross-link, do not duplicate).

#### Issue 06 — per-task cost scales with chunk-file count (NFS metadata)

Source: `issues/06_mdio_cpp_per_task_cost_scales_with_chunk_file_count.md`;
submission text: `issues/upstream/06_mdio-cpp_per_task_cost_file_count.md`.

**What the probe established.** A dedicated two-arm probe (job 421183, node
bw33b10n34, quati cluster; script `/u/u4w7/tmp/fd_family_probe/probe.sh`, raw
data `/u/u4w7/tmp/fd_family_probe/tasks.tsv`) established that the per-task
cost of mdio-cpp region writes scales with the **number of chunk files** in
the touched datasets — source AND destination — not with the bytes actually
read or written. Context: the granularity curve (job 421156) had shown the
C++ per-task cost exploding from 0.72 s (family=48) to 8.83 s (family=1, run
average). Each family=1 task writes 17 files (16 seismic chunks + 1
coordinate); the source grids compared are `[48,76,64]` (128 chunks/variable)
vs `[1,76,64]` (6,128 chunks/variable) over shape (383, 304, 256).

| Measurement | s/task | Condition (files at destination) |
|---|---|---|
| Arm 1 early (tasks 0–49) | 1.604 | source 128 chunks, dest 0→850 (avg ~425) |
| Arm 1 late (tasks 333–382) | 3.640 | source 128 chunks, dest 850→1,700 (avg ~1,275) |
| Arm 2 first 50 (tasks 0–49) | 16.236 | source 6,128 chunks, dest 0→850 (avg ~425) |
| Arm 2 last measured (tasks 70–121, n=52) | 19.628 | source 6,128 chunks, dest 1,190→2,074 (avg ~1,630) |
| Reference 421156 (f1 average) | 8.83 | source 128 chunks, dest grows to 6,511 (avg ~3,247) |

- **Source side:** with the source pre-rechunked to one chunk per inline
  (6,128 chunk files per variable vs the original 128), each task — reading
  exactly its own 16 small chunks, i.e. ZERO read amplification — is **10x
  slower** (16.2 vs 1.6 s/task). If the cost were bytes read, this arm would
  have been the cheapest; the read-amplification hypothesis was refuted.
- **Destination side:** within a run, each task costs more than the previous
  one, linearly in accumulated destination files: **~0.0024 s per file** (arm
  1 slope; arm 2 degradation gives an independent ~0.0028 — same law). The
  linear model predicts the full-run per-task average within 5% (8.4 s
  predicted vs 8.83 s observed).

**Mechanism (strace).** `strace -f -tt -T -y` on a single-inline read
(16 chunks, 311 KB) from the 6,128-chunk dataset shows **~12,000 metadata
syscalls**: 8,072 `openat` (NFS LOOKUP, ~1.3 ms each, 11.25 s total; 6,137 of
them FAILED existence probes returning ENOENT/ENOTDIR) plus 3,849
`getdents64` (NFS READDIR) — against 296 total for the same read from the
128-chunk dataset (198 `openat`, 0.29 s; 98 `getdents64`). The chunk key
`c/<i>/<j>/<k>` is walked one component at a time with
`openat(O_DIRECTORY)` — `seismic` → `c` → `<i>` → `<j>` → `<k>` — each
`openat` an NFS LOOKUP round-trip of ~1.3 ms; key existence is probed with
`openat` instead of being computed from the chunk grid; directory enumeration
adds the `getdents64` calls. The process is 99% idle (CPU 1%): the cost is
entirely metadata round-trip latency. Tool note: `strace -c` under-reports
per-syscall time under `-f` (multi-threaded); the counts are correct, the
durations come from `strace -f -tt -T` (p50 = 1.32 ms/call, 11.3 s summed).

**Additive cost model.** Per-task cost ≈ 0.07 s + 0.0025 s × (source chunk
files) + 0.0026 s × (accumulated destination files). It fits the measured
granularity curve within 1.0–1.3x at the metadata-dominated points — family=1
8.85 s predicted vs 8.83 observed; family=8 1.45 vs 1.57; family=48 0.57 vs
0.72 — with family=383 the relative-worst fit (0.41 vs 0.75, 1.82x; smallest
absolute — the base cost dominates), and the probe arms at 0.98–1.00x. A
task-level regression over both probe arms (n=222 tasks) gives R² ≈ 0.99
(arm 1) and 0.96 (arm 2). The per-file coefficients are **node-specific** —
they measure NFS metadata latency: re-measured on a second node (gr15b01n02,
2026-09-13), the same model over-predicts the metadata-dominated points by
19-27% (f1: 6.43 observed vs 8.85 predicted; f8: 1.17 vs 1.45) while the
structure holds — itself evidence that the cost is NFS-metadata-bound.

**Ruled out.** Not server serialization: under K=8 concurrent tasks (job
421182) the per-task cost does not worsen (speedups 7.5–8.6x) — the cost is
per-task work, not contention. Not metadata dialect: both probe sources were
py-written (the rechunked one built by the Python port) — the experimental
variable is file count, not dialect.

**Concrete impact.** A distributed copy of a ~110 MB dataset as 383
one-inline tasks (destination growing to 6,511 files) takes **56 minutes** in
C++ at 8.83 s/task (43 min in Python), while the same copy in one task takes
0.75 s. Practical rule until fixed: keep chunks large — one chunk per task at
least as large as the source chunk keeps tasks at 0.7–1.6 s.

**Autonomous reproducer.** `issues/repro/repro_nfs_file_count.cpp` — a single
file depending only on mdio-cpp (`mdio/mdio.h`), abseil and nlohmann/json; no
application code (full source in the submission text, next subsection). It
creates two datasets with IDENTICAL data (same shape 383×304×256, same
deterministic values, same coordinates) differing only in the seismic chunk
grid (`[48,76,64]` → 128 files vs `[1,76,64]` → 6,128), then reads inline 191
— the same 16 chunks touched in both grids, the same 311 KB returned, and the
few-grid read decompresses MORE bytes per chunk (a byte-cost model predicts
"few" as the slower arm). One fresh process per read (mirroring one task per
process), 5 runs per arm:

| Measurement | few (128 chunks) | many (6,128 chunks) | ratio |
|---|---|---|---|
| open+read, 5 fresh processes | 0.275–0.282 s (first cold run: 0.429 s) | 11.156–11.359 s | **41x** |
| checksum of the read inline | 3.87835e+07 | 3.87835e+07 | identical |
| `openat` (strace -f -T) | 198 (0.29 s) | 8,072 (11.25 s) | 41x |
| — of which failed probes | 137 | 6,137 (ENOENT/ENOTDIR) | — |
| `getdents64` (strace -f -T) | 98 | 3,849 | 39x |
| process CPU | 6% | 1% (0.19 s sys, 0.00 s user) | — |

**Honest limitations** (red team 2026-09-12): the dedicated probe has n=1 run
per arm (the job started with residual load 13.6, registered in
`environment.txt`; within-run comparisons are unaffected) — the autonomous
reproducer adds n=5 fresh processes per arm, stable. The effect was measured
on two distinct NFS deployments (the quati cluster and the login node — same
law, ~1.3–2.4 ms per round-trip); other backends (S3, GCS, local) may not
exhibit it. Two earlier analysis errors were caught and corrected in review:
an earlier version of the model-fit claim ("f8/f48 run 2–4x above
prediction") was an artifact of swapped labels, and the first analysis of
this experiment series erroneously refuted the destination growth from a
wrong file count ("~100" vs the real 850–1,700) — the raw data cited above
is the source of truth.

**Expected behavior / fix directions.** Opening/reading a region should pay
metadata cost proportional to the chunks in the region's index space, not to
the whole dataset's file count. The chunk grid is fully described in
`zarr.json` — chunk keys are computable without listing the directory and
without probing key existence with `openat`. Fixes: construct chunk keys
directly from the grid; cache negative/positive metadata lookups across opens
within a process; document chunk-grid sizing guidance for NFS deployments
until the driver is fixed.

**Provenance.** mdio-cpp `main`/HEAD == `fcbfb85`; duplicate search: mdio-cpp
"NFS" (0 results), mdio-cpp "metadata performance" (only #103, closed,
coordinate index — unrelated), tensorstore "readdir" (0 results). Submission
target: mdio-cpp (the cost is in the open path of the zarr/tensorstore driver
it uses); if triage there points to tensorstore, re-forward with the same
reproducer.

### Complete upstream issue texts (English, submission-ready)

The three bodies below are the complete, paste-ready issue texts (four-backtick
fences because the bodies themselves contain triple-backtick code fences).
They were reproduced from the evaluation corpus and then corrected in place by
the red-team pass of 2026-09-14 — the corpus copies are legacy; the texts below
are the authoritative submission versions, and the deltas are listed in the
Editorial notes.

Issue 01 — struct dtype write dialect:

````
## Summary

When mdio-cpp writes a variable with a structured dtype (e.g. a SEG-Y trace header: 89 fields, 232-byte packed records), the zarr v3 metadata it emits uses a private structured-dtype dialect that zarr-python does not recognize. The dataset cannot even be **opened** by the Python side:

```
ValueError: No Zarr data type found that matches {'configuration': {'fields': [{'data_type': 'int32', 'name': 'trace_seq_num_line'}, ...]}, 'name': 'struct'}
```

## Context

zarr v3 currently has **no portable structured dtype** — zarr-python 3.1.3 implements `"structured"` as a zarr-specific extension (`zarr/core/dtype/npy/structured.py:106`, `_zarr_v3_name: Literal["structured"]`; line 68: "This representation is not currently defined in an external specification" — tracked in zarr-developers/zarr-python#2134). So both sides are writing private dialects; mdio-cpp's just happens to be a *different* private dialect. The companion issue (read direction: mdio-cpp opens py-written structured variables but derives an invalid `"byte"` creation spec) is the same root cause surfacing on the other side.

## The two dialects side by side

Same variable, same chunk bytes (verified bit-identical after blosc decompression), written by each stack:

| Aspect | mdio-cpp writes | mdio-python / zarr-python expects |
|---|---|---|
| type name | `"struct"` | `"structured"` |
| field encoding | objects `{"data_type": "int32", "name": "..."}` | arrays `["field_name", "int32"]` |
| codecs | `["bytes"]` (no blosc) | `["bytes", "blosc"]` |
| `fill_value` | object `{"field": 0, ...}` | base64 string `"AAAA..."` |

## Reproducer

Write a dataset containing one structured-dtype variable with mdio-cpp, then open it with mdio-python:

```python
import mdio
ds = mdio.open_mdio("headers_written_by_cpp.mdio")
# ValueError: No Zarr data type found that matches ... 'name': 'struct'
```

## Expected

mdio-cpp emits the same structured-dtype encoding zarr-python reads (round-trip both directions), or — if zarr v3 has no portable structured dtype yet (zarr-developers/zarr-python#2134) — both stacks document that structured variables are stack-local today.

## Suggested fixes

1. **Align the write dialect with zarr-python's `"structured"`** (name, array-form fields, base64 fill_value, blosc codec entry). This makes cpp-written datasets readable by the whole zarr-python ecosystem — the highest-value fix.
2. If a portable zarr v3 structured dtype is genuinely unavailable, **fail loudly at write time** ("structured dtypes are not portable; write as separate numeric variables") instead of emitting a dialect nothing else reads.
3. Consumer-side workaround (documented): store header fields as separate numeric variables, or keep structured variables on one stack per dataset.

## Impact

Cross-stack consumption is impossible for structured variables: a dataset written by the C++ ingest cannot be opened by any zarr-python-based consumer. Numeric variables are unaffected (chunk bytes verified bit-identical between the stacks).

## Environment

mdio-cpp pinned commit `fcbfb85` (== current `main` HEAD at the time of writing); mdio-python 1.0.8, zarr 3.1.3, xarray 2025.10.1, dask 2025.10.0.
````

Issue 02 — struct dtype byte spec:

````
## Summary

The mirror of the write-side dialect gap (companion issue: mdio-cpp writes structured dtypes as `"struct"`, which zarr-python cannot open). mdio-cpp **can open** a dataset written by mdio-python that contains a structured-dtype variable (the `"structured"` dialect parses fine), but the creation spec derived from that variable is invalid — the data type comes out as `"byte"`, which mdio-cpp's own creation-spec schema rejects:

```
INVALID_ARGUMENT: Validation failed, here is why: At /variables/4/dataType of "byte" - no subschema has succeeded, but one of them is required to validate. Type: anyOf, number of failed subschemas: 2
```

The failure surfaces when building a new dataset from the opened one (open → derive creation spec → `Dataset::from_json`), so cross-dialect copies of structured variables are broken in this direction too.

## Reproducer

Write a dataset with one structured-dtype variable using mdio-python (`to_mdio`), then on the C++ side open it, derive the creation spec from the opened variable's spec (`Variable::get_spec()`, `mdio/variable.h:1406`), and pass it to `mdio::Dataset::from_json`. Two notes for a self-contained reproducer:

1. Normalize `createdOn` first (see the companion mdio-python issue: mdio-python 1.0.8 writes it with a space separator, which mdio-cpp's dataset-level validation rejects BEFORE the variable-level error surfaces) — otherwise the reproducer shows the wrong failure.
2. The core upstream defect is independent of any application code: deriving the variable's spec with the library's own API (`Variable::get_spec()`, `mdio/variable.h:1406`, wrapping `spec()` → `ToJson()`) and assembling the creation spec from it yields `"byte"` as the structured variable's data type, which the creation-spec schema rejects. `Dataset::CommitMetadata` itself erases the tensorstore `dtype` from this same derived spec (`mdio/dataset.h:1293-1295`) — mdio-cpp's own code treats that dtype as non-round-trippable.

## Expected

mdio-cpp either round-trips the structured dtype it just read, or fails at open time with a clear "structured dtype not supported for this operation" message — not a schema rejection of a spec it produced itself.

## Context

zarr v3 currently has no portable structured dtype (zarr-developers/zarr-python#2134; zarr-python 3.1.3's `"structured"` is a zarr-specific extension — `zarr/core/dtype/npy/structured.py:68`: "This representation is not currently defined in an external specification"). Both directions of this gap (write dialect + derivation loss) share that root cause.

## Impact

Cross-dialect copies of structured variables fail in both directions: py-written structured datasets cannot be copied or re-created from the C++ side.

## Environment

mdio-cpp pinned commit `fcbfb85` (== current `main` HEAD at the time of writing); mdio-python 1.0.8, zarr 3.1.3, xarray 2025.10.1, dask 2025.10.0.
````

Issue 06 — per-task cost scales with chunk file count:

````
## Summary

A dedicated two-arm probe established that the per-task cost of mdio-cpp region writes scales with the **number of chunk files** in the touched datasets — source AND destination — not with the bytes actually read or written:

1. **Source side:** with the source pre-rechunked to one chunk per inline (6128 chunk files per variable vs the original 128), each task — reading exactly its own 16 small chunks, i.e. ZERO read amplification — is **10x slower** (16.2 vs 1.6 s/task). If the cost were bytes read, this arm would have been the cheapest. An autonomous reproducer (below) isolates the source-side effect to the open+read alone: **41x** (11.2 s vs 0.27 s) for the same single-inline read, with identical checksums.
2. **Destination side:** within a run, each task costs more than the previous one, linearly in accumulated destination files: **~0.0024 s per file** (same slope in both probe arms, independently derived). The linear model predicts the full-run per-task average within 5%.

An additive model — per-task cost ≈ 0.07 s + 0.0025 s × (source chunk files) + 0.0026 s × (accumulated destination files) — fits the metadata-dominated points of the measured granularity curve within 1.0-1.3x (family=1: 8.85 predicted vs 8.83 s; family=8: 1.45 vs 1.57; family=48: 0.57 vs 0.72; probe arms: 0.98-1.00x); the boot-dominated family=383 point (smallest absolute cost) runs 1.8x above the prediction (0.41 vs 0.75 s). A task-level regression over both probe arms (n=222 tasks) gives R² ≈ 0.99/0.96 per arm (an independent recomputation gives 0.98/0.92; the joint fit over all 222 tasks gives R² = 0.9975). The per-file coefficients are node-specific (they measure NFS metadata latency): re-measured on a second node, the same model over-predicts the metadata-dominated points by 19-27% while the structure holds — itself evidence that the cost is NFS-metadata-bound.

## Measured mechanism (strace)

A single-inline read (16 chunks, 311 KB) from the 6128-chunk dataset issues **~12,000 metadata syscalls** — 8,072 `openat` (NFS LOOKUP, ~1.3 ms each; 6,137 of them FAILED existence probes returning ENOENT/ENOTDIR) plus 3,849 `getdents64` (NFS READDIR) — against 296 total for the same read from the 128-chunk dataset. The chunk key `c/<i>/<j>/<k>` is walked one component at a time with `openat(O_DIRECTORY)`, and key existence is probed with `openat` instead of being computed from the chunk grid. The process is 99% idle (CPU 1%): the cost is entirely metadata round-trip latency.

A distributed copy of a ~110 MB dataset as 383 one-inline tasks (destination growing to 6511 files) takes **56 minutes** at 8.83 s/task, while the same copy in one task takes 0.75 s.

## Expected

Opening/reading a region should pay metadata cost proportional to the chunks in the region's index space, not to the whole dataset's file count. The chunk grid is fully described in `zarr.json` — chunk keys are computable without listing the directory and without probing key existence with `openat` (the strace evidence shows 6,137 failed probes in one single-inline read).

## Autonomous reproducer

No application code — mdio-cpp + abseil + nlohmann/json only. Two datasets with IDENTICAL data (same shape 383x304x256, same values, same coordinates), differing only in the seismic chunk grid. **Run the reproducer on the storage class under test** — the paths below are examples. The 41x was measured on NFS-backed storage, where each metadata round-trip costs ~1.3-2.4 ms; on local SSD the metadata round-trips are nearly free and the factor collapses (the cost is metadata-bound, not byte-bound), so point the datasets at an NFS-like backend to reproduce it:

```bash
$ repro create /tmp/few.mdio few    # chunks [48,76,64] -> 128 files
$ repro create /tmp/many.mdio many  # chunks [1,76,64]  -> 6128 files

# One fresh process per read; inline 191 touches the same 16 chunks in
# both grids, returns the same 311 KB, and the few-grid read
# decompresses MORE bytes per chunk (a byte-cost model predicts "few"
# as the slower arm):
$ repro read /tmp/few.mdio 191    # ~0.27-0.28 s  (5 runs)
$ repro read /tmp/many.mdio 191   # ~11.2-11.4 s  (5 runs)  -> 41x
#   checksum identical in both — same data read

# Mechanism (follow threads; -c under-reports time with -f):
$ strace -f -tt -T -y -e trace=openat,getdents64 repro read /tmp/many.mdio 191
#   openat:      8,072 calls, 11.25 s total, ~1.3 ms each (NFS LOOKUP)
#                6,137 failures (ENOENT/ENOTDIR existence probes)
#   getdents64:  3,849 calls (NFS READDIR enumeration)
#   vs few.mdio: openat 198 (0.29 s) + getdents64 98
```

<details>
<summary>Full reproducer source (repro_nfs_file_count.cpp)</summary>

```cpp
// Single file; depends only on mdio-cpp (mdio/mdio.h), abseil, nlohmann/json.
// Usage:
//   repro create <dataset_path> <few|many>
//   repro read   <dataset_path> <inline_index>
// "read" prints: elapsed_ms=<ms> checksum=<sum> first=<v> last=<v>
// (checksum/first/last must MATCH between the two datasets).
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "mdio/mdio.h"
#include "nlohmann/json.hpp"

namespace {

constexpr int64_t kInlineSize = 383;
constexpr int64_t kCrosslineSize = 304;
constexpr int64_t kTimeSize = 256;

uint32_t SampleAt(int64_t i, int64_t j, int64_t k) {
  return static_cast<uint32_t>((i * 31 + j * 17 + k * 7) % 1000);
}

nlohmann::json BuildVariableSpec(const std::string& name,
                                 const std::string& dtype,
                                 const std::vector<std::pair<std::string, int64_t>>& dims,
                                 const std::vector<int64_t>& chunk_shape) {
  nlohmann::json spec;
  spec["name"] = name;
  spec["dataType"] = dtype;
  nlohmann::json dimensions = nlohmann::json::array();
  nlohmann::json coords = nlohmann::json::array();
  for (const auto& [label, size] : dims) {
    dimensions.push_back({{"name", label}, {"size", size}});
    coords.push_back(label);
  }
  spec["dimensions"] = std::move(dimensions);
  spec["coordinates"] = std::move(coords);
  spec["metadata"]["chunkGrid"] = {{"name", "regular"},
                                   {"configuration", {{"chunkShape", chunk_shape}}}};
  return spec;
}

nlohmann::json BuildCreationSpec(const std::vector<int64_t>& seismic_chunk) {
  nlohmann::json spec;
  spec["metadata"] = {{"name", "repro_nfs_file_count"},
                      {"apiVersion", "1.0.0"},
                      {"createdOn", "2026-09-12T00:00:00+00:00"},
                      {"attributes", {{"origin", "repro_nfs_file_count"}}}};
  spec["variables"] = nlohmann::json::array(
      {BuildVariableSpec("seismic", "float32",
                         {{"inline", kInlineSize},
                          {"crossline", kCrosslineSize},
                          {"time", kTimeSize}},
                         seismic_chunk),
       BuildVariableSpec("inline", "uint16", {{"inline", kInlineSize}}, {kInlineSize}),
       BuildVariableSpec("crossline", "uint16", {{"crossline", kCrosslineSize}},
                         {kCrosslineSize}),
       BuildVariableSpec("time", "uint16", {{"time", kTimeSize}}, {kTimeSize})});
  return spec;
}

template <typename T, mdio::DimensionIndex R, mdio::ArrayOriginKind K>
T* BufferPointer(mdio::VariableData<T, R, K>& variable_data) {
  auto accessor = variable_data.get_data_accessor();
  return reinterpret_cast<T*>(reinterpret_cast<char*>(accessor.data()) +
                              variable_data.get_flattened_offset() * sizeof(T));
}

template <typename T, typename Fill>
absl::Status FillAndWrite(mdio::Dataset& dataset, const std::string& name, Fill fill) {
  MDIO_ASSIGN_OR_RETURN(auto variable, dataset.get_variable<T>(name));
  MDIO_ASSIGN_OR_RETURN(auto data, mdio::from_variable<T>(variable));
  T* buffer = BufferPointer(data);
  fill(buffer);
  mdio::WriteFutures futures = variable.Write(data);
  return futures.status();
}

absl::Status RunCreate(const std::string& path, const std::string& variant) {
  std::vector<int64_t> seismic_chunk;
  if (variant == "few") {
    seismic_chunk = {48, 76, 64};
  } else if (variant == "many") {
    seismic_chunk = {1, 76, 64};
  } else {
    return absl::InvalidArgumentError("variant must be 'few' or 'many'");
  }
  nlohmann::json creation_spec = BuildCreationSpec(seismic_chunk);
  MDIO_ASSIGN_OR_RETURN(auto dataset, mdio::Dataset::from_json(
                                          creation_spec, path,
                                          mdio::constants::kCreateClean)
                                          .result());
  MDIO_RETURN_IF_ERROR(FillAndWrite<mdio::dtypes::float32_t>(
      dataset, "seismic", [&](mdio::dtypes::float32_t* buffer) {
        size_t index = 0;
        for (int64_t i = 0; i < kInlineSize; ++i)
          for (int64_t j = 0; j < kCrosslineSize; ++j)
            for (int64_t k = 0; k < kTimeSize; ++k)
              buffer[index++] = static_cast<float>(SampleAt(i, j, k));
      }));
  MDIO_RETURN_IF_ERROR(FillAndWrite<mdio::dtypes::uint16_t>(
      dataset, "inline", [&](mdio::dtypes::uint16_t* buffer) {
        for (int64_t i = 0; i < kInlineSize; ++i) buffer[i] = static_cast<uint16_t>(1000 + i);
      }));
  MDIO_RETURN_IF_ERROR(FillAndWrite<mdio::dtypes::uint16_t>(
      dataset, "crossline", [&](mdio::dtypes::uint16_t* buffer) {
        for (int64_t j = 0; j < kCrosslineSize; ++j) buffer[j] = static_cast<uint16_t>(1000 + j);
      }));
  MDIO_RETURN_IF_ERROR(FillAndWrite<mdio::dtypes::uint16_t>(
      dataset, "time", [&](mdio::dtypes::uint16_t* buffer) {
        for (int64_t k = 0; k < kTimeSize; ++k) buffer[k] = static_cast<uint16_t>(k);
      }));
  std::cout << "create " << variant << " ok" << std::endl;
  return absl::OkStatus();
}

absl::Status RunRead(const std::string& path, int64_t inline_index) {
  const auto start = std::chrono::steady_clock::now();
  MDIO_ASSIGN_OR_RETURN(auto dataset,
                        mdio::Dataset::Open(path, mdio::constants::kOpen).result());
  MDIO_ASSIGN_OR_RETURN(auto variable,
                        dataset.get_variable<mdio::dtypes::float32_t>("seismic"));
  MDIO_ASSIGN_OR_RETURN(auto sliced, variable.slice({{"inline", inline_index, inline_index + 1},
                                                     {"crossline", 0, kCrosslineSize},
                                                     {"time", 0, kTimeSize}}));
  MDIO_ASSIGN_OR_RETURN(auto data, sliced.Read().result());
  const auto elapsed = std::chrono::steady_clock::now() - start;
  const float* buffer = BufferPointer(data);
  const size_t count = static_cast<size_t>(kCrosslineSize) * kTimeSize;
  double checksum = 0;
  for (size_t i = 0; i < count; ++i) checksum += buffer[i];
  std::cout << "elapsed_ms="
            << std::chrono::duration<double, std::milli>(elapsed).count()
            << " checksum=" << checksum << " first=" << buffer[0]
            << " last=" << buffer[count - 1] << std::endl;
  return absl::OkStatus();
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 4 && std::string(argv[1]) == "create") {
    const absl::Status status = RunCreate(argv[2], argv[3]);
    if (!status.ok()) { std::cerr << "create failed: " << status << std::endl; return 1; }
    return 0;
  }
  if (argc == 4 && std::string(argv[1]) == "read") {
    const absl::Status status = RunRead(argv[2], std::stoll(argv[3]));
    if (!status.ok()) { std::cerr << "read failed: " << status << std::endl; return 1; }
    return 0;
  }
  std::cerr << "usage: " << argv[0]
            << " create <path> <few|many> | read <path> <inline>" << std::endl;
  return 2;
}
```

</details>

## Suggested fixes

1. **Avoid directory listing and existence probing on the read path**: construct chunk keys directly from the chunk grid in `zarr.json` (the grid is known; no `readdir`/`openat` probe needed to address a chunk).
2. **Cache negative/positive metadata lookups** across opens within a process (even per-process, one task touches 16+17 files yet pays for thousands).
3. **Document chunk-grid sizing guidance for NFS deployments** (few large chunks; the cost model above) until the driver is fixed.

## Impact

Fine-grained chunking (or many small variables) makes per-task costs explode on NFS even when each task touches few bytes. Measured on two distinct NFS deployments (same law, ~1.3-2.4 ms per round-trip); other backends (S3, GCS, local) may not exhibit it.

## Environment

mdio-cpp pinned commit `fcbfb85` (== current `main` HEAD at the time of writing), tensorstore via CMake FetchContent; NFS-backed storage; AMD EPYC 9334 nodes.
````

### Bugs fixed in the evaluation codebase during the work (evaluation-code bugs, not library bugs)

Everything in this subsection is a bug in the evaluation's **own** C++/Python
code — the parity-battery programs and helpers — found and fixed while the
battery was being built. None is an mdio-cpp defect. The first five come from
the gap-closing execution record (`docs/gap_closing_plan.md` §13, "Bugs reais
encontrados e corrigidos durante a execução"); the sixth is the
distributed-copy manifest divergence (corpus issue 04).

1. **Fail-open validation (Phase 1.4).** The validation sampler read the
   dtype from the v2 layout (`spec["metadata"]["dtype"]`); in v3 stores that
   is null and **all** variables were silently skipped — every "Validation
   OK" since the v3 migration was empty. This also hid the trace_gain bug
   below. Fixed: top-level dtype, geometry via the index domain, and
   fail-loud (zero validated variables = error). Lesson captured:
   `~/.learned_lessons/20260908124800`.
2. **trace_gain transform guard (Phase 1.4).** The guard compared
   `view.dtype != "<f4"`, but v3 stores report `"float32"` — the gain
   transform never ran, and 99.99% of the output was a copy without gain.
   Fixed: the guard accepts `float32`/`<f4`/`>f4`; verified bit-exact
   (29,806,592 samples = input × 1.1).
3. **Value-as-index selection (Phase 2).** The ROI passed coordinate values
   (inline 1083–1382) as 0-based domain indices — "Slice descriptor for
   crossline is invalid". Fixed with `ValueToIndex` /
   `ValueRangeToIndexRange`; the output now matches the Python reference to
   float precision.
4. **Validation count always zero (Phase 3b-A).**
   `check_random_samples_custom_diff` returned `0` on success instead of the
   validated-sample count — the distributed verify failed every
   *successful* validation (latent: only that caller checked the value).
   Fixed to return the count.
5. **Main-thread write amplification (Phase 3a).** Blocking the main thread
   in `WriteFutures::status()` triggers pathological pipeline re-submission
   (~127x: 15 GB written to copy 115 MB; 57 s). Transfers are now pipelined
   on a non-main thread: 5.3 s → 2.2 s, and deterministic. Lesson captured:
   `~/.learned_lessons/20260908130200`.
6. **Distributed-copy manifest format divergence (issue 04).** The two
   distributed-copy example programs serialized their partition manifests
   with different top-level JSON shapes — the C++ side a top-level **array**
   of partition descriptors, the Python side a top-level **object**
   (`source`/`dest`/`dimension`/`inline_chunk`/`partitions`) — so a manifest
   produced by one program could not be consumed by the other (C++ consuming
   a py manifest: "Partition copy failed: Manifest must be a non-empty
   array"; py consuming a C++ manifest: "list indices must be integers or
   slices, not str"). Red-team classification (2026-09-12): **internal** —
   the manifests are written by this repo's example programs
   (`trace_copy_parallel_distributed.cpp:135-193`,
   `trace_copy_parallel_distributed.py:209-218`), not by the library;
   submitted upstream it would die at the first comment. Fixed in the repo
   (commit `be23c69`, 2026-09-12) by converging the C++ manifest format on
   the Python one: a unified `distributed-manifest/1` object
   (`format`/`source`/`dest`/`dimension`/`family_size`/`partitions` with
   per-variable entries); the C++ side writes the new format and still
   accepts the legacy array; the Python side writes and consumes the
   unified form (and rejects `transfer: "chunk"` loudly — chunk mode is the
   C++ stage's domain). Cross-consumption was verified in both directions
   plus the full battery (39/39); end-to-end flows re-ran on both stacks,
   and the benchmark suite was re-executed post-fix (A/B: 5 rounds, 26
   candidates, control 1.02x < gate 1.15x; granularity-curve smoke,
   FAMILIES=8) with no anomalous regression. Source:
   `issues/04_distributed_manifest_shape_divergence.md`.

### Upstream mdio-cpp bugs verified as already fixed (provenance)

In the header-patch era (May–September 2025) the evaluation carried post-fetch
scripts that patched mdio-cpp headers after FetchContent
(`fix_mdio_inlines.sh`, `fix_mdio_pointers.sh`). The rebase onto the pinned
commit `fcbfb85` removed the patch scripts: every bug below is fixed upstream
at the pin, the build is patch-free, and the battery passes 10/10 targets
(`docs/gap_closing_plan.md` §2; `README.md`, "Historical bug verification").
One line per fix commit (details: `docs/gap_consequences.md` §3.6;
`README.md` bug table):

- `18b7540` (Jul/2025) — "fix missing inlines and incorrect pointers in
  'from_variable'": non-inline definitions in headers (multiple-definition
  link errors) and incorrect pointers in `from_variable` with non-zero slice
  origin; verified by the since-removed `mdio_inline_bug` /
  `mdio_pointer_bug` programs.
- `b5e42fc` (Sep/2025) — "correct sliceInRange stop clamp for non-zero origin
  domains": `sliceInRange` used shape instead of origin+shape; verified by
  `indexed_slicing_and_trace_selection`.
- `52678bd` (Sep/2024) — attribute refcount bug; verified by `header_edit`.
- `535f7d1` (May/2025) — dropped elements in slice; verified by
  `indexed_slicing_and_trace_selection`.

Provenance note: per `docs/gap_consequences.md` §3.6, the non-zero-origin fix
(`b5e42fc`) landed on the eve of the last commit of the 2025 verification
programs — that entire effort lived with the bug open.

## Measurements

All measurements below concern **mdio-cpp pinned at `fcbfb85`** (which pins
tensorstore at `917edaf34`), zarr v3 on NFS, benchmarked against the Python
reference implementations of the same workloads (mdio-python 1.0.8 /
xarray / dask). The dataset is the synthetic post-stack volume
`full_stack_agc.mdio` — 383×304×256 float32 seismic plus coordinates,
110,406,161 bytes (~110 MB), source chunk grid `[48,76,64]` (128 chunks per
variable), NFS-backed store. Every number in this section was re-verified
against the raw results files listed in the provenance table; per-task costs
are computed as partitions-stage seconds ÷ task count.

### A/B benchmark against the Python reference

**Method.** Each workload exists as a C++ program (mdio-cpp/tensorstore) and
a Python port (mdio-python/xarray/dask). Both are executed interleaved in the
same run — same node, same dataset, same round — with 1 untimed warmup pass
and 5 timed passes per candidate, candidate order rotated per pass, medians
quoted. Drift control: `cpp_copy` is repeated at the end of the candidate
list; a round is discarded if control ÷ cpp_copy exceeds 1.15×.

**Pre-fix definitive round** — job 421150, dedicated Slurm compute node
`bw33b03n01` (cluster quati, AMD EPYC 9334, 128 cores, idle), Sep 9, 2026.
Ten workload pairs (20 candidates); control 0.99×; CVs ≤ 7% except cpp_copy
(9%), cpp_create (21%), cpp_segy_ingest (13%). One cpp_distributed
repetition failed (exit 126) and was excluded — its median is over the 4
valid repetitions (9.36 s; including the failed one would give 9.07 s and
would not change the ratio).

| Workload | C++ (s) | Python (s) | Python/C++ |
|---|---|---|---|
| copy (full dataset + validation) | 2.303 | 7.493 | 3.3× |
| reader (full scan, per-trace stats) | 0.697 | 6.951 | 10.0× |
| gain (per-sample transform + copy) | 2.239 | 7.288 | 3.3× |
| distributed copy (init + 8 partitions + verify) | 9.358† | 73.087 | 7.8× |
| header_edit (copy + edit existing attribute)‡ | 5.396 | 8.084 | 1.5× |
| header_add (copy + new attribute) | 2.276 | 7.530 | 3.3× |
| create_from_schema (generate + write, no read) | 1.233 | 7.664 | 6.2× |
| segy_ingest (rev1/IBM parse + MDIO write)§ | 1.324 | 47.394 | 35.8× |
| plane_reader (one time-slice, stats + probes) | 0.430 | 6.178 | 14.4× |
| decimate (every 4th inline, strided copy) | 1.374 | 6.787 | 4.9× |

† Median over 4 valid repetitions (rep 3 failed, exit 126). With all 5 the
median would be 9.07 s — the ratio does not change.
‡ Benchmark artifact, not an attribute-update penalty: header_edit's input is
the one dataset the benchmark rewrites every round, so it is always cold in
the NFS page cache. A hot-input probe on the benchmark node gives 2.18 s
(vs 2.23 s for header_add).
§ The Python reference writes an extra `headers` variable (~28 MB,
structured dtype) that the C++ port deliberately skips; the shared variables
(amplitude, coordinates, cdp_x/cdp_y, trace_mask) are bit-identical across
the two ingests.

mdio-cpp wins every monolithic workload in this round, from 1.5× (header_edit,
cold-input artifact) to 35.8× (segy_ingest).

**Post-fix re-validation.** After the internal distributed-manifest fix
(commit `be23c69`, Sep 12, 2026 — unified `distributed-manifest/1` format),
the full A/B was re-run on Sep 13, 2026, on login node `gr15sdes01` (same
CPU model EPYC 9334, same dataset, same 5-run protocol), now covering 26
candidates (13 pairs — the ten above plus the three reorder pairs). Control
1.02× (< 1.15× gate), all exits 0.

| Workload | C++ (s) | Python (s) | Python/C++ |
|---|---|---|---|
| copy | 1.755 | 6.190 | 3.5× |
| reader | 0.572 | 5.970 | 10.4× |
| gain | 1.796 | 6.927 | 3.9× |
| distributed copy | 6.414 | 55.997 | 8.7× |
| header_edit | 4.871 | 7.019 | 1.4× |
| header_add | 1.789 | 7.101 | 4.0× |
| create_from_schema | 1.011 | 6.178 | 6.1× |
| segy_ingest | 1.959 | 33.847 | 17.3× |
| plane_reader | 0.299 | 5.848 | 19.6× |
| decimate | 1.071 | 5.589 | 5.2× |
| reorder (xline, iline) transpose | 2.394 | 7.200 | 3.0× |
| reorder (off_bin, iline, xline) | 1.308 | 6.409 | 4.9× |
| reorder (off_bin, xline, iline) axis swap | 1.298 | 6.383 | 4.9× |

The two rounds' ratios differ (e.g. segy_ingest 35.8× → 17.3×,
plane_reader 14.4× → 19.6×) because of the node, not the code: both stacks
shifted jointly between the rounds (see "Node effects" below), and the
segy_ingest pair moved in opposite directions on the login node. The
decision-relevant ordering — mdio-cpp faster on every monolithic workload —
holds in both rounds.

**No anomalous regression from the manifest fix.** py_distributed, the only
candidate whose data path the fix touched (per-variable opens in the
unified manifest), shifted 0.77× between rounds — in line with the other
Python candidates (0.71-0.95×); cpp_distributed shifted 0.69×, in line with
the C++ field (0.69-0.90×). The Python family flow is structurally immune to
the fix (one variable per task before and after — "Partição N copiada
(1 variáveis)" in the logs). The single candidate outside the joint band was
cpp_segy_ingest (1.48×, slower on the login node: 1.32 → 1.96 s median);
that workload touches no manifest and its log output is identical across
rounds, so the shift is environmental rather than attributable to the fix
[attribution by elimination; the login node is shared].

**Reorder (pre-fix, separate round).** Job 425001, same node `bw33b03n01`,
control 1.01×, CVs ≤ 6.2%: even at the worst measured read amplification
(76×, full transpose) the C++ side pays only ~0.6 s over a plain copy
(3.30 vs 2.70 s) and wins 2.5×; the 8×-amplification and true-axis-swap
variants win 7.6× and 7.1×. I/O amplification alone does not invert the
comparison at this dataset scale — the inversion appears only in the
many-task regime below.

### Family-granularity curve

**What it measures.** The distributed copy partitioned along inline into
tasks grouped by family size (inlines per task) 1 / 8 / 48 / 383 — task
counts 383 / 48 / 8 / 1 on the 383-inline dataset — run as the full
three-stage flow: init, every partition task as its own process invocation
(sequential; the HPC task model, so per-task process startup is part of the
measured cost), verify. The destination chunk grid is aligned to the family
size, so every task writes exactly 16 seismic chunks + 1 coordinate file
(17 files); the destination grows to tasks × 17 files (6,511 at family=1).

**Pre-fix** — job 421156, node `bw33b10n34` (EPYC 9334, idle, load
0.11-0.22), Sep 9, 2026:

| Family (inlines/task) | Tasks | C++ s/task | Python s/task | Python/C++ | C++ total (s) | Python total (s) |
|---|---|---|---|---|---|---|
| 1 | 383 | 8.83 | 6.63 | **0.75× (Python wins)** | 3401.8 | 2572.9 |
| 8 | 48 | 1.57 | 6.71 | 4.28× | 79.5 | 340.4 |
| 48 (= source chunk) | 8 | 0.72 | 6.77 | 9.35× | 7.6 | 67.6 |
| 383 (whole) | 1 | 0.75 | 7.14 | 9.54× | 2.1 | 22.7 |

The curve inverts the monolithic result at maximum granularity: mdio-cpp's
per-task cost is small at coarse granularity (0.72-0.75 s/task) but explodes
at family=1 (8.83 s/task on average, growing over the run — early tasks
~1.6 s), while the Python per-task cost stays flat at ~6.7 s whatever the
family size (interpreter startup, imports and the dask/xarray open cost
dominate; data volume per task is noise). Family=1 is pathological for both
stacks (43-57 min for a ~110 MB copy) and is the only measured regime where
the Python reference wins.

**Post-fix** — job 28807, node `gr15b01n02` (EPYC 9354 — a different CPU
model from the 9334 nodes; load 0.14-0.29), Sep 13, 2026:

| Family (inlines/task) | Tasks | C++ s/task | Python s/task | Python/C++ | C++ total (s) | Python total (s) |
|---|---|---|---|---|---|---|
| 1 | 383 | 6.43 | 5.52 | **0.86× (Python wins)** | 2478.7 | 2151.0 |
| 8 | 48 | 1.17 | 5.58 | 4.78× | 58.7 | 282.8 |
| 48 | 8 | 0.58 | 5.68 | 9.83× | 6.3 | 56.7 |
| 383 | 1 | 0.67 | 5.51 | 8.25× | 3.2 | 17.2 |

**Verdict: the manifest fix did not change the curve.** Both stacks shifted
together between the nodes (C++ 0.73-0.89×, Python 0.77-0.84×) — the
signature of a node effect (NFS metadata latency plus the CPU model
difference), not of a code change. The py/cpp ratios moved without
systematic direction (f1 0.75→0.86, f8 4.28→4.78, f48 9.35→9.83,
f383 9.54→8.25). Supporting evidence: a login-node run of 7 of the 8
combinations (the C++ family=1 leg was lost to a collision with a
concurrent backfill job, 28775) is consistent where it is reliable
(py f1 5.56 vs 5.52; cpp f8 1.14 vs 1.17 s/task), and a post-fix smoke run
at family=8 matches (cpp 1.13, py 5.48 s/task).

### The additive per-task cost model

The family=1 explosion was reduced to an additive cost model (issue 06 of
the evaluation corpus):

> per-task cost ≈ 0.07 s + 0.0025 s × (source chunk files)
>                + 0.0026 s × (accumulated destination files)

An independent joint least-squares fit over all 222 probe tasks (below)
reproduces the coefficients: 0.0025 s per source file, 0.0026 s per
accumulated destination file, intercept 0.11 s, R² = 0.9975 — the documented
model is the least-squares fit of the probe data.

Fit on the original node (job 421156 curve):

| Point | Predicted (s/task) | Observed (s/task) | obs/pred |
|---|---|---|---|
| family=1 | 8.85 | 8.83 | 1.00× |
| family=8 | 1.45 | 1.57 | 1.08× |
| family=48 | 0.57 | 0.72 | 1.28× |
| family=383 | 0.41 | 0.75 | 1.82× |
| probe arm 1 (aggregate, n=100) | 2.58 | 2.62 | 1.02× |
| probe arm 2 (aggregate, n=122) | 18.06 | 17.92 | 0.99× |

The model is worst in relative terms exactly where the absolute cost is
smallest (family=383: the 0.07 s base term dominates) and at the arm-1 early
batch (1.07-1.09×); three of the four probe batch means fit at 0.99-1.00×.
A task-level regression over both probe arms (n = 222) is documented as
R² ≈ 0.99 (arm 1) / 0.96 (arm 2); an independent recomputation of the
per-arm linear fit gives 0.98 / 0.92 — the same structure, with the exact
values depending on the fit definition.

**The honest limitation: the per-file coefficients are node-specific** —
they measure NFS metadata round-trip latency, not compute. Re-measured on
the second node (`gr15b01n02`, Sep 13), the same model over-predicts the
metadata-dominated points — family=1: 6.43 s observed vs 8.85 predicted
(27% over); family=8: 1.17 vs 1.45 (19-20% over) — while the structure
holds (coarse points still fit; both stacks shifted together). That the
coefficients move with the node is itself evidence that the cost is
NFS-metadata-bound.

### NFS probes: isolating source vs destination file counts

Job 421183, node `bw33b10n34`, Sep 10, 2026, 01:34-02:16 local (the job started with
residual load 13.6 from the preceding parallel job — within-run comparisons
are unaffected; absolute values carry that condition). Two arms, each
isolating one variable:

- **Arm 1 — destination growth.** Original source (128 chunks/variable),
  sparse task set: tasks 0-49 ("early") and 333-382 ("late") run, the
  intermediates not run, so the destination holds 850-1,700 files at the
  late batch instead of the 6,511 of a full run.
- **Arm 2 — source file count.** Source pre-rechunked to `[1,76,64]`
  (6,128 chunks/variable, written by the Python port — same metadata
  dialect), full run: every task reads exactly its own 16 chunks, i.e.
  zero read amplification. Interrupted at task 122 — the linear growth was
  established.

Results (batch means, verified from the raw per-task file):

| Batch | s/task | Condition (destination files) |
|---|---|---|
| Arm 1 early (tasks 0-49) | 1.604 | source 128 chunks, dest 0→850 |
| Arm 1 late (tasks 333-382) | 3.640 | source 128 chunks, dest 850→1,700 |
| Arm 2 first 50 (tasks 0-49) | 16.236 | source 6,128 chunks, dest 0→850 |
| Arm 2 last measured (tasks 70-121, n=52) | 19.628 | source 6,128 chunks, dest 1,190→2,074 |
| Reference: full family=1 run (job 421156) | 8.83 | source 128 chunks, dest grows to 6,511 |

What the probe establishes:

1. **Destination growth is real and linear.** Arm-1 slope =
   (3.64 − 1.60)/(1,275 − 425) ≈ 0.0024 s/file (recomputed: 0.00237); the
   arm-2 degradation gives an independent slope ≈ 0.0028 (recomputed:
   0.00282). The arm-1 slope predicts the full-run family=1 average within
   5% (8.4 vs 8.83 s).
2. **It is not read amplification — the opposite.** With the source
   rechunked to 6,128 chunks (each task reads exactly its own chunk, zero
   amplification), the task is 10× slower (16.24 vs 1.60 s at equivalent
   destination state): 48× the files → 10× the cost, with fewer bytes
   touched per chunk. A bytes-based cost model predicts the rechunked arm
   as the cheapest — refuted.
3. **Mechanism (strace, login node, Sep 12).** A single-inline read
   (16 chunks, 311 KB) from the 6,128-chunk dataset issues ~12,000 metadata
   syscalls — 8,072 `openat` (NFS LOOKUP, ~1.3 ms each; 6,137 of them
   failed existence probes returning ENOENT/ENOTDIR) plus 3,849
   `getdents64` (NFS READDIR) — against 296 total for the same read from
   the 128-chunk dataset. The chunk key `c/<i>/<j>/<k>` is walked one
   component at a time with `openat(O_DIRECTORY)`, and key existence is
   probed with `openat` instead of being computed from the chunk grid
   (which `zarr.json` fully describes). The process is 99% idle — the cost
   is metadata round-trip latency, not CPU. (Tool note: `strace -c`
   under-reports per-call durations under `-f`; counts are correct,
   durations come from `strace -f -tt -T`, p50 = 1.32 ms/call.)
4. **Autonomous reproducer** (`issues/repro/repro_nfs_file_count.cpp` —
   mdio-cpp + abseil + nlohmann/json only, no application code): two
   datasets with identical data (same shape 383×304×256, same values, same
   coordinates) differing only in the seismic chunk grid — `[48,76,64]`
   (128 files) vs `[1,76,64]` (6,128). Reading inline 191 (the same 16
   chunks touched in both grids, the same 311 KB returned; the few-grid read
   decompresses *more* bytes per chunk): 0.275-0.282 s vs 11.156-11.359 s
   over 5 fresh processes per arm — **41×** — with identical checksums
   (3.87835e+07).
5. **Not server serialization.** Under K=8 concurrent tasks (job 421182),
   per-task cost does not worsen — wall-clock speedups 7.5-8.6× at every
   family size (family=48 K8: 0.78 s vs 6.94 s; family=8 K8: 8.74 s vs
   42.25 s; family=1 K8: 400.1 s vs 318.9 s — the family=1 inversion
   persists under parallelism, Python 1.25× faster). The cost is per-task
   work, not contention.

### Node effects and the py/cpp ratio detector

Per-language absolute times do not transfer across nodes: the post-fix A/B
round (login `gr15sdes01` — same CPU model, same dataset, same protocol) is
systematically faster than the pre-fix definitive round (compute
`bw33b03n01`) in both stacks, with node factors of **0.69-0.95× for 19 of
the 20 shared candidates** (C++ 0.69-0.90, Python 0.71-0.95). The single
exception is cpp_segy_ingest at 1.48× (slower on the login node) — the
shortest, most CPU-bound candidate, on a shared node; its Python counterpart
moved 0.71× (inside the band), which is why the segy_ingest ratio drops
from 35.8× to 17.3× across the rounds: a node artifact of two
opposite-direction shifts, not a code change. The family curve shows the
same joint shift on a third node (`gr15b01n02`, an EPYC 9354): C++
0.73-0.89×, Python 0.77-0.84×.

The py/cpp ratio is the transferable metric to first order: both languages share the node
within a round, so the ratio cancels the node factor. That is
why the ratios reproduce across rounds at the metadata-light points (copy 3.3→3.5, reader
10.0→10.4, distributed 7.8→8.7) while absolute times do not, and why cross-node comparisons
in this report quote ratios, not seconds. Short, metadata-bound workloads drift more across
nodes (plane_reader 14.4→19.6, segy_ingest 35.8→17.3 between the pre-fix and post-fix
rounds' nodes) — for those, ratios transfer as orders of magnitude, not exact factors.
The additive model's per-file coefficients are the deliberate exception —
node-specific by construction, because they measure NFS latency.

### Provenance (raw data)

| Measurement | Raw data |
|---|---|
| Pre-fix A/B (job 421150, `bw33b03n01`) | `/u/u4w7/tmp/fd_bench_quati/results.tsv`, `slurm-421150.out` |
| Pre-fix reorder round (job 425001) | `/u/u4w7/tmp/fd_bench_reorder/results.tsv` |
| Pre-fix family curve (job 421156, `bw33b10n34`) | `/u/u4w7/tmp/fd_family_gran/results.tsv` |
| Parallel execution (job 421182) | `/u/u4w7/tmp/fd_family_par/results.tsv` |
| NFS probe (job 421183, `bw33b10n34`) | `/u/u4w7/tmp/fd_family_probe/tasks.tsv` |
| Post-fix A/B (login `gr15sdes01`, Sep 13) | `/u/u4w7/tmp/fd_bench_postfix/results.tsv` |
| Post-fix smoke (family=8) | `/u/u4w7/tmp/fd_family_smoke_postfix/results.tsv` |
| Post-fix family curve (job 28807, `gr15b01n02`) | `/u/u4w7/tmp/fd_family_gran_postfix/results.tsv` |
| Issue texts (mechanism, model, manifest fix) | `issues/06_*`, `issues/04_*`, `docs/relatorio_decisao_cpp_vs_python.md` in the evaluation repo (`src/formato-dados`) |

## Status and next steps

- **Upstream issue texts are prepared and paste-ready** for the three mdio-cpp
  issues of this report — 01 (structured-dtype write dialect), 02 (invalid
  `"byte"` creation spec), 06 (per-task cost vs chunk-file count) — with final
  bodies embedded in full (corrected) in the Bugs section above, exact
  `gh issue create` commands, and a suggested escalation order (03 first; then
  01 + 02 cross-linked; then 05; then 06; then 07). (03 and 05 target
  mdio-python; 07 targets xarray — out of scope for this report.)
- **Submission is blocked on credentials, not content**: this environment's
  GitHub account is an EMU (Enterprise Managed User) and cannot create issues
  outside the petrobrasbr-exp enterprise (GraphQL error: "As an Enterprise
  Managed User, you cannot access this content"). Filing requires a personal
  account — a user decision.
- **Push of the evaluation branch** `feature/fase0-rebase-python-port` (the
  review panel's fifth condition) is likewise open with the user.
- In this repository, the evaluation leaves behind one committed artifact — the
  `feat/api-gap-plan` docs branch (3 commits on top of `fcbfb85`) — plus this
  untracked report, which carries the corrected submission texts for issues
  01, 02 and 06 (awaiting a personal account to be filed).

## Editorial notes

Assembly reconciliations (rule: where two sections disagreed, the measurements
section's evidence-verified numbers won). No content was dropped; the three
upstream issue bodies in "Complete upstream issue texts" were reproduced from
the evaluation corpus and corrected in place by the red-team and follow-up
passes — the corpus copies are legacy; the embedded texts are the authoritative
submission versions.

1. **Family=1 inversion ratio (context, twice).** The context section quoted
   Python at 0.76×; the measurements section's family-granularity table
   (job 421156) gives 0.75× (6.63 vs 8.83 s/task). Reconciled to 0.75×.
2. **A/B CV exceptions (context).** The context section listed cpp_create
   (21%) and cpp_segy_ingest (13%) above the ≤ 7% band; the measurements
   section also lists cpp_copy (9%). Reconciled to include cpp_copy.
3. **Post-fix A/B node factors (context).** The context section said both
   stacks ran at 0.69–0.95× without exception and quoted the other Python
   candidates at 0.81–0.95×; the measurements section gives 0.69–0.95× for
   19 of the 20 shared candidates — the exception is cpp_segy_ingest at
   1.48× (slower on the shared login node) — and 0.71–0.95× for the Python
   candidates. Reconciled accordingly.
4. **Second-node over-prediction range (context, bugs).** Both said 20–27%;
   the measurements section's per-point evidence gives 27% (family=1) and
   19–20% (family=8). Reconciled to 19–27% in the narrative and in the
   embedded issue 06 text (follow-up pass, note 8).
5. **Post-fix family=1 observation (bugs).** The bugs section said 6.44 s
   observed; the measurements section says 6.43 s (post-fix curve table and
   model section). Reconciled to 6.43.
6. **Additive-model fit claim (bugs).** The bugs section claimed the entire
   granularity curve fits within 1.0–1.3× while itself listing family=383
   (0.41 s predicted vs 0.75 s observed = 1.82×); the measurements section's
   fit table reads 1.00×/1.08×/1.28×/1.82× and characterizes family=383 as
   worst in relative terms exactly where the absolute cost is smallest.
    Reconciled: the 1.0–1.3× claim now covers the metadata-dominated points,
    and family=383 is quoted at 1.82×. (The embedded issue 06 text was aligned
    to this in the follow-up pass, note 8.)
7. **Section cross-references (context).** The context section pointed to
   sibling sections as *Bugs and fixes*, *Benchmark results* and
   *Interop findings*; aligned to the final titles — *Bugs* (which carries
   the defects and the interop findings) and *Measurements* (which carries
   the benchmark results).
8. **Red-team pass (2026-09-14, post-assembly).** A four-seat red team (factual auditor,
   claims verifier, empirical re-executor, reasoning strategist) reviewed this report, the
   `feat/api-gap-plan` divergence and the embedded upstream bodies; all recomputed numbers
   matched (medians, curves, node factors, model predictions, commit hashes). Corrections
   applied to this report: the 35.8× SEG-Y-ingest headline is qualified as partially
   behavioral (the C++ port skips the ~28 MB structured `headers` variable the Python
   reference writes); the 0.75× inversion carries its post-fix value (0.86×); the
   battery-count citation points to the corpus file that carries the full count; the
   distributed-copy pre-fix median reads 9.358 s (was truncated to 9.357); the py/cpp-ratio
   transferability claim is qualified (short metadata-bound workloads drift across nodes:
   plane_reader 14.4→19.6, segy_ingest 35.8→17.3); and the closing inventory separates the
   committed docs branch from this untracked report. Follow-up pass (2026-09-14, formato-dados
   declared legacy): the remaining body-level findings were applied directly to the embedded
   texts, which are the authoritative submission versions. Issue 06's reproducer now carries a
   storage-class note (the 41× is NFS-metadata-bound; on local SSD it collapses) and its model
   paragraph states the metadata-dominated fit (family=383 at 1.82×) with the reconciled R²
   values (0.99/0.96 documented, 0.98/0.92 recomputed, joint fit 0.9975) and the 19-27%
   second-node range; issue 02's reproducer now derives the spec through the library's own
   API (`Variable::get_spec()`, `mdio/variable.h:1406`) and grounds the in-library claim in
   source (`Dataset::CommitMetadata` erases the tensorstore dtype from the same derived
   spec, `mdio/dataset.h:1293-1295`) instead of the unverified "born inside the library"
   attribution.

