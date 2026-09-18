# M7 — Metadata-path: benchmark harness + syscall attribution (etapas 1+2)

Branch: `feat/api-gap-m7` (from `main` @ `b360d5b`). Date: 2026-09-16.
Node: `gr15sdes01` (login/dev), NFS `s6006vfs0606.petrobras.biz:/DSV_TG_001`
(same filer as the issue-06 reproducer run). No fix implemented in this
step — measurement and attribution only, per the M7 plan ("needs a
benchmark harness first").

## 1. Harness (etapa 1)

- `mdio/benchmark/nfs_metadata_bench.cpp` — port of the issue-06
  autonomous reproducer (data formula kept identical, so checksums are
  comparable with the recorded evidence). One read per process by
  design: the measured cost is per-open metadata traffic; in-process
  repeats would hit the very caches M7's deliverable 2 gates.
- `mdio/benchmark/count_metadata_syscalls.py` — drives N clean runs
  (fresh process each, median reported) and M traced runs under
  `strace -f -tt -T -y -e trace=openat,getdents64`; emits one CSV line
  per traced run `(dataset,run,wall_time_s,openat_total,openat_failed,
getdents64)` and keeps the trace files for attribution.
- CMake: `MDIO_BUILD_BENCHMARKS` option (default OFF), target
  `mdio_nfs_metadata_bench`; not a ctest test (needs an explicit NFS
  path).
- Datasets: `/tgdesenv/src/proj0/u4w7/home2/tmp/m7-bench/{few,many}.mdio`
  (NFS). Verified chunk-file counts: few 128, many 6128 seismic chunks
  (136/6136 total files) — matches issue 06.

## 2. Baseline (this node)

| Measurement | few (128 chunks) | many (6128 chunks) | ratio |
|---|---|---|---|
| clean open+read, median of 5 fresh processes | 0.310 s (min 0.296, max 0.323) | 11.633 s (min 11.461, max 11.855) | 37.5x |
| `openat` (traced, 3 runs) | 225 (137 failed) | 8100 (6137 failed) | 36x |
| — failed breakdown | 136 ENOTDIR + 1 ENOENT | 6136 ENOTDIR + 1 ENOENT | — |
| `getdents64` (traced) | 98 | 3849 | 39x |
| checksum of the read inline | 3.87835e+07 | 3.87835e+07 | identical |

Issue 06 reference (login node, 2026-09-12): 0.275-0.282 s vs 11.2-11.4 s;
failed openat 137/6137, getdents64 98/3849 — reproduced within noise on
this node. Note: the issue's TOTAL openat counts (198/8072) undercounted
by ~28 per run — multi-threaded strace splits a syscall across
`<unfinished ...>`/`<... openat resumed>)` lines and the resumed form was
not being counted; the harness's parser counts both forms (unit-tested;
the failure counts are unaffected — the walk's probes are single-line).
CSV: `m7-bench/baseline.csv`; traces:
`m7-bench/traces/{few,many}/run-{1..3}.strace`.

## 3. Attribution (etapa 2)

### 3.1 The dominant cost is ONE full-store recursive List issued by mdio's own variable discovery

Trace timeline (many, run 1): root `zarr.json` reads at 43.451-43.456 s;
**the recursive walk runs 43.457 → 55.045 s (11.6 s, ~99.9 % of the
elapsed time)**; per-variable `zarr.json` reads + the 16 chunk reads take
the last ~25 ms (55.045 → 55.071 s).

Emitter chain:

- `mdio/zarr/zarr_v3.h:580` — `OnV3RootReadComplete` calls
  `tensorstore::kvstore::ListFuture(state->kvs)` with **no range
  restriction** to discover the dataset's variables.
- `mdio/zarr/zarr_v3.h:185-201` — `ExtractChildArrayCandidates` uses
  only entries matching top-level `NAME/zarr.json` — i.e. the walk
  enumerates all 6128 chunk files to find 4 variable names.
- `tensorstore/kvstore/file/file_key_value_store.cc:839-878` —
  `ListTask` maps the kvstore List onto
  `internal_os::RecursiveFileList` (line 851), pruning recursion by
  `IntersectsPrefix(options.range, path)` (lines 853-855). No
  `KeyRange` can express "only top-level `NAME/zarr.json`": any range
  containing `seismic/zarr.json` also intersects `seismic/c/...`
  ("c" < "z"), so the full-store range mdio needs cannot prune.
- The zarr3 array driver itself does NOT list at open (no List call in
  `tensorstore/driver/zarr3/driver.cc`; the trace shows only
  `zarr.json` reads + chunk reads after the walk).

### 3.2 (a) Who emits the `openat(O_DIRECTORY)` component walks

`tensorstore/internal/os/file_lister_posix.cc:146-151` — the lister
pushes every directory entry from `readdir` onto a stack and opens it
with `openat(parent_fd, component, O_RDONLY|O_DIRECTORY|O_NOFOLLOW)` to
classify it. Success → it is a directory → `fdopendir` + `readdir`
(lines 173-188); `ENOTDIR` → it is a file → emit as key (lines 156-160).
The "component-by-component walk" of the issue is this recursion
(`c` → `<i>` → `<j>`), one probe per entry, ~1924 successful directory
opens + 6136 file probes in the many arm.

The chunk READ path does NOT walk components: each of the 16 chunks is
opened with a single `openat(AT_FDCWD, "<full path>", O_RDONLY)`
(trace lines 11946-11977, ~25 ms total). Chunk keys are already
computed from the grid — no probing of chunk-key existence anywhere.

### 3.3 (b) Who emits the 6137 "failed existence probes"

They are not chunk-key existence probes. 6136 ENOTDIR = **every FILE
entry in the store**, each probed once by the lister's dir-vs-file test
(3.2): 6128 seismic chunks + 3 coordinate chunks (`inline/c/0`,
`crossline/c/0`, `time/c/0`) + 4 variable `zarr.json` + 1 root
`zarr.json`. The remaining 1 ENOENT is abseil's startup probe of
`/sys/devices/system/cpu/cpu0/tsc_freq_khz` (trace line 9) — unrelated
to mdio. Evidence: `openat(5<.../inline/c>, "0", O_DIRECTORY) = -1
ENOTDIR` (line 19), `openat(7<.../seismic/c/0/0>, "1", O_DIRECTORY) =
-1 ENOTDIR` (line 31), `openat(4<.../inline>, "zarr.json",
O_DIRECTORY) = -1 ENOTDIR` (line 21).

### 3.4 (c) Who emits the 3849 `getdents64`

The same lister's `readdir` (file_lister_posix.cc:188) — the directory
enumeration of the walk: 2 calls per directory (one batch + one
end-of-directory), 1924 directories walked (root 1 + 4 variable dirs +
4 `c/` dirs + 383 `<i>` dirs + 1532 `<j>` dirs) + 1 = 3849.

### 3.5 Quantitative split (many, run 1; durations from `strace -T`)

| Syscall | calls | sum of durations | share |
|---|---|---|---|
| `openat` (the lister's probes + metadata/chunk opens) | 8100 | 11.227 s | 99.75 % |
| `getdents64` (the lister's readdir) | 3849 | 0.028 s | 0.25 % |

Each `openat` is an NFS LOOKUP round-trip (~1.39 ms mean); `getdents64`
(NFS READDIR) is nearly free on this filer (~7 µs/call). **The cost is
the per-entry LOOKUP probes, not the directory enumeration.** (Caveat:
READDIR cost is server-dependent; on this filer it is cached/cheap.)

### 3.6 Refinements to the issue-06 mechanism text

1. "key existence is probed with `openat` instead of being computed
   from the chunk grid" — refuted in mechanism: chunk keys ARE computed
   from the grid (16 single-`openat` chunk reads); the failed probes
   are the file lister's dir-vs-file classification during variable
   discovery.
2. "3.849 getdents64 (NFS READDIR enumeration)" as a cost term — on
   this filer READDIR is 0.25 % of the syscall time; the LOOKUP probes
   are the whole cost.
3. Consequence: issue-06 "possible solution 1" (direct chunk-key
   addressing) targets a path that is already direct; the fix site is
   variable discovery, not chunk addressing.

## 4. Fix-site comparison

### (i) TensorStore open spec/parametrization (complete spec avoids listing?)

No. The List is issued by mdio (zarr_v3.h:580), not by the zarr3
driver; a complete tensorstore spec removes the driver's metadata read
but not mdio's need to discover variables. Resolves (a)/(b)/(c):
nothing.

### (ii) Patch TensorStore (pin is FetchContent GIT_TAG, cmake/FindEXT_TENSORSTORE.cmake:10)

Feasible without forking the build: `FETCHCONTENT_SOURCE_DIR_TENSORSTORE=<patched clone>` (CMake 3.30.3 available) or a fork tag. Patch
target: `file_lister_posix.cc` — classify entries from `readdir`'s
`d_type` (fallback to the `openat` probe on `DT_UNKNOWN`), eliminating
~8k LOOKUP round-trips per open. Expected effect on the many arm:
11.5 s → well under 0.5 s (getdents 0.028 s + ~30 metadata/chunk
opens). Resolves (b) fully and the dominant share of (a); (c) remains
but is measured-free here (server-dependent elsewhere). Risks: fork
maintenance until upstreamed; `DT_UNKNOWN` fallback keeps correctness;
does not remove the semantic waste (walking 6128 entries to find 4
variables) — on a filer with expensive READDIR the walk cost returns.

### (iii) Direct read path in mdio-cpp (compute chunk keys, read via kvstore)

Refuted by attribution: the chunk read path is already key-computed and
costs ~25 ms; the dominant cost is variable discovery at open, which
this option does not touch (unless it also bypasses `Dataset::Open`
entirely). Would reimplement the codec pipeline (sharding, codecs,
dtype conversion, fill values) for ~0 gain. Resolves: nothing, at high
risk.

### (iv) Metadata lookup cache at mdio-cpp level

In-process caching does not help the measured workload (one process per
task; each task pays the List once at open). A cross-process cache
needs a persisted artifact — which converges to the recommended option
below. Resolves: nothing for the 383-task case.

### (v) RECOMMENDED — stop listing: variable index in the root `zarr.json` (mdio-cpp, zarr_v3.h)

The create path already writes the root `zarr.json` and knows the
variable list (`zarr_v3.h:408-410` — the comment there documents the
current listing design: "variables are discovered via directory
listing, not stored in zarr.json"). Change:

- Create: write the variable-name list into the root `zarr.json`
  attributes (or a zarr-v3 consolidated-metadata-shaped field, for
  interop friendliness).
- Open: if the index is present, skip `ListFuture` (zarr_v3.h:580)
  entirely and go straight to the per-variable `NAME/zarr.json` reads
  that `OnV3ListComplete` already performs (zarr_v3.h:546-555); if
  absent (mdio-python stores, foreign stores), fall back to today's
  List.

Resolves (a), (b) and (c) completely for mdio-written stores — which is
what the 383-task workload reads and writes. Zero TensorStore changes;
read path stays backward compatible (fallback preserves foreign-store
support); mdio-python interop unchanged (its stores lack the index →
fallback = current behavior). Risks: index staleness if a store is
mutated by non-mdio tooling (mitigate: document that the index is
authoritative for mdio-created stores; optionally verify cheaply);
mdio-python should adopt writing the index later (follow-up, not a
blocker). Optionally pair with (ii) upstreamed as defense-in-depth for
foreign stores on filers where READDIR is expensive.

## 5. M7 gate instrument

The harness + script are the M7 performance-gate instrument
("metadata-syscall count per read" and "per-region latency", NFS
backend): baseline few 225 openat / 98 getdents64 / 0.310 s vs many
8100 / 3849 / 11.633 s. After the fix, the many arm must lose the
linear dependence on source chunk-file count (expect ≈ few-level
counts: ~200 openat, ~100 getdents64, sub-second reads), with the
checksum gate (3.87835e+07) proving identical data.

## 6. Evidence index

- Baseline CSV: `/tgdesenv/src/proj0/u4w7/home2/tmp/m7-bench/baseline.csv`
- Traces: `/tgdesenv/src/proj0/u4w7/home2/tmp/m7-bench/traces/{few,many}/run-{1,2,3}.strace`
  (cited lines from `traces/many/run-1.strace`: 9, 13-21, 27-34, 11921, 11922-11945, 11946-11977)
- Datasets: `/tgdesenv/src/proj0/u4w7/home2/tmp/m7-bench/{few,many}.mdio`
- TensorStore source (pinned 917edaf34):
  `tensorstore/internal/os/file_lister_posix.cc:146-160,174-188`;
  `tensorstore/kvstore/file/file_key_value_store.cc:839-878`
- mdio source: `mdio/zarr/zarr_v3.h:185-201,408-410,531-585`
