#!/usr/bin/env python3
"""Unit tests for count_metadata_syscalls.parse_trace (stdlib unittest).

Run: python3 -m unittest discover -s mdio/benchmark -p 'test_*.py'

The parser's tricky parts are exercised with synthetic strace lines:
normal results, failed results (errno breakdown), "<... openat resumed>"
fragments (multi-threaded traces split a syscall across lines), and
non-matching lines that must be ignored.
"""

from __future__ import annotations

import importlib.util
import tempfile
import unittest
from pathlib import Path

_SCRIPT = Path(__file__).parent / "count_metadata_syscalls.py"
_spec = importlib.util.spec_from_file_location("count_metadata_syscalls",
                                               _SCRIPT)
cms = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(cms)


class ParseTraceTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.trace_dir = Path(self._tmp.name)

    def write_trace(self, content: str) -> Path:
        path = self.trace_dir / "trace.strace"
        path.write_text(content)
        return path

    def test_counts_openat_getdents_and_errno_breakdown(self):
        trace = "\n".join([
            '100 19:00:00.000001 openat(AT_FDCWD, "/a", O_RDONLY) = 3</a> <0.000001>',
            '100 19:00:00.000002 openat(4</d>, "x", O_DIRECTORY) = -1 ENOTDIR (Not a directory) <0.001>',
            '100 19:00:00.000003 openat(4</d>, "y", O_RDONLY) = -1 ENOENT (No such file or directory) <0.001>',
            '100 19:00:00.000004 getdents64(3</a>, 0x1, 32768) = 100 <0.000001>',
            '100 19:00:00.000005 getdents64(3</a>, 0x1, 32768) = 0 <0.000001>',
        ])
        counts = dict(cms.parse_trace(self.write_trace(trace)))
        errnos = counts.pop("errnos")
        self.assertEqual(counts, {"openat_total": 3, "openat_failed": 2,
                                  "getdents64": 2})
        self.assertEqual(errnos, {"ENOTDIR": 1, "ENOENT": 1})

    def test_counts_resumed_openat_fragments_once(self):
        trace = "\n".join([
            '100 19:00:00.000001 openat(AT_FDCWD, "/a", O_RDONLY <unfinished ...>',
            '101 19:00:00.000002 openat(AT_FDCWD, "/b", O_RDONLY <unfinished ...>',
            '100 19:00:00.000003 <... openat resumed>) = 3</a> <0.001>',
            '101 19:00:00.000004 <... openat resumed>) = -1 ENOENT (No such file or directory) <0.001>',
        ])
        counts = dict(cms.parse_trace(self.write_trace(trace)))
        errnos = counts.pop("errnos")
        self.assertEqual(counts["openat_total"], 2)
        self.assertEqual(counts["openat_failed"], 1)
        self.assertEqual(errnos, {"ENOENT": 1})

    def test_ignores_unfinished_without_result_and_other_syscalls(self):
        trace = "\n".join([
            '100 19:00:00.000001 openat(AT_FDCWD, "/a", O_RDONLY <unfinished ...>',
            '100 19:00:00.000002 readlinkat(AT_FDCWD, "/x", 0x1, 99) = 10 <0.000001>',
            '100 19:00:00.000003 write(1, "openat(", 7) = 7 <0.000001>',
        ])
        counts = dict(cms.parse_trace(self.write_trace(trace)))
        counts.pop("errnos")
        self.assertEqual(counts, {"openat_total": 0, "openat_failed": 0,
                                  "getdents64": 0})


class ElapsedRegexTest(unittest.TestCase):
    def test_extracts_elapsed_ms_and_checksum(self):
        line = "elapsed_ms=275.4 checksum=3.87835e+07 first=0 last=999\n"
        match = cms.ELAPSED_RE.search(line)
        self.assertIsNotNone(match)
        self.assertEqual(match.group(1), "275.4")
        self.assertEqual(match.group(2), "3.87835e+07")


if __name__ == "__main__":
    unittest.main()
