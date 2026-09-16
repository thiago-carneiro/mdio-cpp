#!/usr/bin/env python3
"""M7 gate instrument: count NFS metadata syscalls of nfs_metadata_bench.

Runs the mdio_nfs_metadata_bench harness against one dataset:

1. Clean runs (default 5): one fresh process per read, no strace --
   the per-read wall time the per-task cost model is built from.
2. Traced runs (default 3): the same fresh-process read under
   `strace -f -tt -T -y -e trace=openat,getdents64`; counts openat
   (total and failed) and getdents64, and appends one CSV line per
   run: dataset,run,wall_time_s,openat_total,openat_failed,getdents64.

Trace files are kept (default <csv dir>/traces/<label>/run-<N>.strace)
for attribution work -- the -y flag resolves dirfds so the probed paths
are visible.

Fails loudly on harness failure or checksum drift across runs (the two
datasets must return identical data; compare with --expect-checksum).

Usage:
  count_metadata_syscalls.py --bench <mdio_nfs_metadata_bench> \
      --dataset <path> --label <few|many> --csv <results.csv> \
      [--inline 191] [--clean-runs 5] [--traced-runs 3] \
      [--trace-dir <dir>] [--expect-checksum <sum>]
"""

from __future__ import annotations

import argparse
import csv
import re
import shutil
import statistics
import subprocess
import sys
from pathlib import Path

STRACE_FLAGS = ["-f", "-tt", "-T", "-y", "-e", "trace=openat,getdents64"]
READ_TIMEOUT_S = 600

# strace line forms counted (with -f, threads may resume a syscall on a
# later line: "<... openat resumed>) = 3"; the result is on that line, so
# the resumed form must count and the unfinished line must not).
SYSCALL_RE = re.compile(r"(?:^|\s)(?:<\.\.\. )?(openat|getdents64)(?:\(| resumed>\))")
RESULT_RE = re.compile(r"=\s*(-1\s+\w+|-?\d+)")
ERRNO_RE = re.compile(r"=\s*-1\s+(\w+)")
ELAPSED_RE = re.compile(r"elapsed_ms=([0-9.]+)\s+checksum=([0-9.e+-]+)")


def run_read(bench: Path, dataset: Path, inline: int) -> tuple[float, str]:
    """One fresh-process read; returns (elapsed_ms, checksum)."""
    proc = subprocess.run(
        [str(bench), "read", str(dataset), str(inline)],
        capture_output=True,
        text=True,
        timeout=READ_TIMEOUT_S,
    )
    if proc.returncode != 0:
        raise SystemExit(
            f"harness read failed (rc={proc.returncode}): {proc.stderr.strip()}"
        )
    match = ELAPSED_RE.search(proc.stdout)
    if not match:
        raise SystemExit(
            f"harness output not parseable: {proc.stdout.strip()!r}"
        )
    return float(match.group(1)), match.group(2)


def parse_trace(trace_path: Path) -> dict[str, int]:
    """Count openat (total/failed, by errno) and getdents64 in a trace."""
    counts = {
        "openat_total": 0,
        "openat_failed": 0,
        "getdents64": 0,
    }
    errnos: dict[str, int] = {}
    for line in trace_path.read_text(errors="replace").splitlines():
        syscall = SYSCALL_RE.search(line)
        if not syscall or not RESULT_RE.search(line):
            continue
        name = syscall.group(1)
        if name == "openat":
            counts["openat_total"] += 1
            errno = ERRNO_RE.search(line)
            if errno:
                counts["openat_failed"] += 1
                errnos[errno.group(1)] = errnos.get(errno.group(1), 0) + 1
        else:
            counts["getdents64"] += 1
    counts["errnos"] = errnos  # type: ignore[assignment]
    return counts


def run_clean_runs(args: argparse.Namespace) -> list[float]:
    """Fresh-process untraced reads; returns per-run elapsed_ms."""
    clean_ms: list[float] = []
    checksum: str | None = None
    for run in range(1, args.clean_runs + 1):
        elapsed_ms, run_checksum = run_read(args.bench, args.dataset,
                                            args.inline)
        if checksum is None:
            checksum = run_checksum
        elif run_checksum != checksum:
            raise SystemExit(
                f"checksum drift across clean runs: {checksum} != {run_checksum}"
            )
        clean_ms.append(elapsed_ms)
        print(f"[{args.label}] clean run {run}: "
              f"elapsed_ms={elapsed_ms:.1f} checksum={run_checksum}")
    if args.expect_checksum is not None and checksum != args.expect_checksum:
        raise SystemExit(
            f"checksum mismatch: got {checksum}, expected {args.expect_checksum}"
        )
    if clean_ms:
        print(f"[{args.label}] clean median_s="
              f"{statistics.median(clean_ms) / 1000.0:.3f} "
              f"min_s={min(clean_ms) / 1000.0:.3f} "
              f"max_s={max(clean_ms) / 1000.0:.3f}")
    return clean_ms


def run_one_traced_run(strace: str, args: argparse.Namespace,
                        trace_path: Path) -> tuple[float, dict]:
    """One fresh-process read under strace; returns (wall_time_s, counts)."""
    proc = subprocess.run(
        [strace, *STRACE_FLAGS, "-o", str(trace_path),
         str(args.bench), "read", str(args.dataset), str(args.inline)],
        capture_output=True,
        text=True,
        timeout=READ_TIMEOUT_S,
    )
    if proc.returncode != 0:
        raise SystemExit(
            f"traced read failed (rc={proc.returncode}): "
            f"{proc.stderr.strip()}"
        )
    match = ELAPSED_RE.search(proc.stdout)
    if not match:
        raise SystemExit(
            f"harness output not parseable: {proc.stdout.strip()!r}"
        )
    return float(match.group(1)) / 1000.0, parse_trace(trace_path)


def run_traced_runs(args: argparse.Namespace, trace_dir: Path) -> None:
    """Fresh-process reads under strace; appends one CSV line per run."""
    strace = shutil.which("strace")
    if not strace:
        raise SystemExit("strace not found on PATH")
    csv_new = not args.csv.exists()
    with args.csv.open("a", newline="") as csv_file:
        writer = csv.writer(csv_file)
        if csv_new:
            writer.writerow(["dataset", "run", "wall_time_s",
                             "openat_total", "openat_failed", "getdents64"])
        for run in range(1, args.traced_runs + 1):
            trace_path = trace_dir / f"run-{run}.strace"
            wall_time_s, counts = run_one_traced_run(strace, args, trace_path)
            errnos = counts.pop("errnos")
            writer.writerow([
                args.label, run, f"{wall_time_s:.3f}",
                counts["openat_total"], counts["openat_failed"],
                counts["getdents64"],
            ])
            print(f"[{args.label}] traced run {run}: wall_time_s="
                  f"{wall_time_s:.3f} openat={counts['openat_total']} "
                  f"failed={counts['openat_failed']} "
                  f"getdents64={counts['getdents64']} "
                  f"errno_breakdown={errnos}")
    print(f"[{args.label}] traces kept in {trace_dir}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bench", required=True, type=Path)
    parser.add_argument("--dataset", required=True, type=Path)
    parser.add_argument("--label", required=True,
                        help="dataset label for the CSV (e.g. few|many)")
    parser.add_argument("--csv", required=True, type=Path)
    parser.add_argument("--inline", type=int, default=191)
    parser.add_argument("--clean-runs", type=int, default=5)
    parser.add_argument("--traced-runs", type=int, default=3)
    parser.add_argument("--trace-dir", type=Path, default=None)
    parser.add_argument("--expect-checksum", default=None,
                        help="fail if the read checksum differs")
    args = parser.parse_args()

    if not args.bench.is_file():
        raise SystemExit(f"bench executable not found: {args.bench}")
    if not args.dataset.is_dir():
        raise SystemExit(f"dataset directory not found: {args.dataset}")

    trace_dir = args.trace_dir or args.csv.parent / "traces" / args.label
    trace_dir.mkdir(parents=True, exist_ok=True)

    run_clean_runs(args)
    run_traced_runs(args, trace_dir)


if __name__ == "__main__":
    main()
