# Copyright 2026 TGS
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#    http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
"""Unit tests for the pure helpers of run_m6_gates.py (M6 gate driver)."""

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))

import run_m6_gates  # noqa: E402


def test_parse_trace_counts_openat_and_getdents64(tmp_path):
    trace = tmp_path / "run.strace"
    trace.write_text(
        "123 12:00:00.000001 openat(AT_FDCWD, \"/a/zarr.json\", "
        "O_RDONLY) = 4 <0.000010>\n"
        "124 12:00:00.000002 openat(AT_FDCWD, \"/a/0/0/0\", O_RDONLY) = -1 "
        "ENOENT (No such file or directory) <0.000005>\n"
        "125 12:00:00.000003 getdents64(5, 0x7f..., 32768) = 120 <0.000004>\n"
        # The -f resumed form must count on the resumed line only.
        "126 12:00:00.000004 openat(<... resuming interrupted read ...> "
        "AT_FDCWD, \"/b\", O_RDONLY <unfinished ...>\n"
        "127 12:00:00.000005 <... openat resumed>) = 6 <0.000001>\n"
    )
    counts = run_m6_gates.parse_trace(trace)
    assert counts["openat_total"] == 3
    assert counts["openat_failed"] == 1
    assert counts["errnos"] == {"ENOENT": 1}
    assert counts["getdents64"] == 1


def test_parse_trace_ignores_non_matching_lines(tmp_path):
    trace = tmp_path / "run.strace"
    trace.write_text("garbage\nwrite(1, \"hello\", 5) = 5\n")
    counts = run_m6_gates.parse_trace(trace)
    assert counts == {"openat_total": 0, "openat_failed": 0,
                      "getdents64": 0, "errnos": {}}


def test_check_checksum_drift_accepts_first_and_equal():
    checksums = {}
    run_m6_gates.check_checksum_drift(checksums, "all", "100")
    # Same region, equal checksum: no error.
    run_m6_gates.check_checksum_drift(checksums, "all", "100")
    # A different region is independent.
    run_m6_gates.check_checksum_drift(checksums, "191", "7.75671e+07")
    assert checksums == {"all": "100", "191": "7.75671e+07"}


def test_check_checksum_drift_fails_on_region_mismatch():
    checksums = {"all": "100"}
    with pytest.raises(SystemExit, match="checksum drift"):
        run_m6_gates.check_checksum_drift(checksums, "all", "200")


def test_parse_arms_builds_argument_lists():
    arms = run_m6_gates.parse_arms(
        "base:slice-serial:all:1:1,map-k8w4:map:all:8:4")
    assert arms == {"base": ["slice-serial", "all", "1", "1"],
                    "map-k8w4": ["map", "all", "8", "4"]}


def test_parse_arms_rejects_malformed_spec():
    with pytest.raises(ValueError):
        run_m6_gates.parse_arms("not-an-arm-spec")
