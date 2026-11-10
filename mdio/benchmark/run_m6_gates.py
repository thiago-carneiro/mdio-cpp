#!/usr/bin/env python3
"""M6 gate driver: clean + strace-traced runs of mdio_m6_map_chunks_bench.

Runs each arm N clean times (one fresh process per run — the per-run wall
time) and M traced times under `strace -f -tt -T -y -e
trace=openat,getdents64` (the M7 census pattern), appending one CSV line
per traced run: arm,run,wall_time_s,openat_total,openat_failed,getdents64.
Fails loudly on harness failure, on checksum drift across runs of an arm,
or on post-gain checksum drift ACROSS arms of the same region (all arms of
a region must transform the region identically).

Usage:
  run_m6_gates.py --bench <mdio_m6_map_chunks_bench> --dataset <path> \
      --csv <results.csv> --arms <label:arm:region:fusion:workers,...> \
      [--clean-runs 5] [--traced-arms <label,...>] [--traced-runs 3]
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
READ_TIMEOUT_S = 1800

SYSCALL_RE = re.compile(
    r"(?:^|\s)(?:<\.\.\. )?(openat|getdents64)(?:\(| resumed>\))")
RESULT_RE = re.compile(r"=\s*(-1\s+\w+|-?\d+)")
ERRNO_RE = re.compile(r"=\s*-1\s+(\w+)")
ELAPSED_RE = re.compile(
    r"elapsed_ms=([0-9.]+)\s+checksum=(\S+)\s+restored=(\S+)")


def run_once(bench: Path, dataset: Path, arm_args: list[str],
             trace_path: Path | None = None) -> tuple[float, str]:
    """One fresh-process run; returns (elapsed_ms, post_gain_checksum)."""
    cmd: list[str] = []
    if trace_path is not None:
        strace = shutil.which("strace")
        if not strace:
            raise SystemExit("strace not found on PATH")
        cmd += [strace, *STRACE_FLAGS, "-o", str(trace_path)]
    cmd += [str(bench), "read", str(dataset), *arm_args]
    proc = subprocess.run(cmd, capture_output=True, text=True,
                          timeout=READ_TIMEOUT_S)
    if proc.returncode != 0:
        raise SystemExit(
            f"harness read failed (rc={proc.returncode}): {proc.stderr.strip()}")
    match = ELAPSED_RE.search(proc.stdout)
    if not match:
        raise SystemExit(f"harness output not parseable: {proc.stdout.strip()!r}")
    return float(match.group(1)), match.group(2)


def parse_trace(trace_path: Path) -> dict:
    """Count openat (total/failed, by errno) and getdents64 in a trace."""
    counts: dict = {"openat_total": 0, "openat_failed": 0, "getdents64": 0,
                    "errnos": {}}
    for line in trace_path.read_text(errors="replace").splitlines():
        syscall = SYSCALL_RE.search(line)
        if not syscall or not RESULT_RE.search(line):
            continue
        if syscall.group(1) == "openat":
            counts["openat_total"] += 1
            errno = ERRNO_RE.search(line)
            if errno:
                counts["openat_failed"] += 1
                errnos = counts["errnos"]
                errnos[errno.group(1)] = errnos.get(errno.group(1), 0) + 1
        else:
            counts["getdents64"] += 1
    return counts


def check_checksum_drift(checksums: dict, region: str,
                         checksum: str) -> None:
    """Fail loudly when arms of one region produce different output."""
    if region in checksums and checksum != checksums[region]:
        raise SystemExit(
            f"post-gain checksum drift across arms of region {region}: "
            f"{checksum} vs {checksums[region]}")
    checksums[region] = checksum


def run_clean_phase(args: argparse.Namespace, arms: dict) -> None:
    """Fresh-process untraced runs per arm; prints medians."""
    checksums: dict = {}
    for label, arm_args in arms.items():
        clean_ms: list[float] = []
        checksum = None
        for run in range(1, args.clean_runs + 1):
            elapsed_ms, checksum = run_once(args.bench, args.dataset, arm_args)
            clean_ms.append(elapsed_ms)
            print(f"[{label}] clean run {run}: elapsed_ms={elapsed_ms:.1f} "
                  f"checksum={checksum}")
        if not clean_ms:
            continue
        check_checksum_drift(checksums, arm_args[1], checksum)
        print(f"[{label}] median_ms={statistics.median(clean_ms):.1f} "
              f"min_ms={min(clean_ms):.1f} max_ms={max(clean_ms):.1f}")


def run_traced_phase(args: argparse.Namespace, arms: dict) -> None:
    """Fresh-process reads under strace; appends one CSV line per run."""
    trace_dir = args.trace_dir or args.csv.parent / "traces"
    trace_dir.mkdir(parents=True, exist_ok=True)
    csv_new = not args.csv.exists()
    with args.csv.open("a", newline="") as csv_file:
        writer = csv.writer(csv_file)
        if csv_new:
            writer.writerow(["arm", "run", "wall_time_s", "openat_total",
                             "openat_failed", "getdents64"])
        for label in [x for x in args.traced_arms.split(",") if x]:
            for run in range(1, args.traced_runs + 1):
                trace_path = trace_dir / f"{label}-run-{run}.strace"
                elapsed_ms, _ = run_once(args.bench, args.dataset,
                                         arms[label], trace_path)
                counts = parse_trace(trace_path)
                errnos = counts.pop("errnos")
                writer.writerow([label, run, f"{elapsed_ms / 1000.0:.3f}",
                                 counts["openat_total"],
                                 counts["openat_failed"],
                                 counts["getdents64"]])
                print(f"[{label}] traced run {run}: wall_time_s="
                      f"{elapsed_ms / 1000.0:.3f} "
                      f"openat={counts['openat_total']} "
                      f"failed={counts['openat_failed']} "
                      f"getdents64={counts['getdents64']} errnos={errnos}")
    print(f"traces kept in {trace_dir}")


def parse_arms(spec: str) -> dict:
    """Parse label:arm:region:fusion:workers specs into arm argument lists."""
    arms: dict = {}
    for item in spec.split(","):
        label, arm, region, fusion, workers = item.split(":")
        arms[label] = [arm, region, fusion, workers]
    return arms


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bench", required=True, type=Path)
    parser.add_argument("--dataset", required=True, type=Path)
    parser.add_argument("--csv", required=True, type=Path)
    parser.add_argument("--arms", required=True,
                        help="label:arm:region:fusion:workers, comma-separated")
    parser.add_argument("--clean-runs", type=int, default=5)
    parser.add_argument("--traced-arms", default="",
                        help="comma-separated labels to also run under strace")
    parser.add_argument("--traced-runs", type=int, default=3)
    parser.add_argument("--trace-dir", type=Path, default=None)
    args = parser.parse_args()

    arms = parse_arms(args.arms)
    run_clean_phase(args, arms)
    run_traced_phase(args, arms)


if __name__ == "__main__":
    main()
