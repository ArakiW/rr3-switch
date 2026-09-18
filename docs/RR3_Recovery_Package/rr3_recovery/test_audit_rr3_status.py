"""Tests for the attached read-only auditor, NOT the RR3 port or guest runtime."""
import contextlib
import hashlib
import io
import json
import struct
import tempfile
import unittest
from pathlib import Path

from audit_rr3_status import audit_elf, audit_text, main, parse_int, read_elf64


def make_report(delta="-0x4E1634", anchor="0x12B3614", target="0x12AEEE0", sha="0" * 64):
    return f"""# Fixture
### 地址映射
| 符号 | ELF VA | 从 onViewRenderJNI 的 delta |
|---|---|---|
| onViewRenderJNI | {anchor} | 0 |
| hideSplash 检查 | {target} | {delta} |
### 主渲染路径
Example.
`libRealRacing3.so` SHA-256: `{sha}`
"""


def make_elf(machine=183, align=4096):
    ident = b"\x7fELF" + bytes([2, 1, 1]) + bytes(9)
    header = struct.pack("<16sHHIQQQIHHHHHH", ident, 3, machine, 1, 0, 64, 0,
                         0, 64, 56, 1, 0, 0, 0)
    segment = struct.pack("<IIQQQQQQ", 1, 5, 0, 0x1000, 0, 256, 512, align)
    return header + segment + bytes(256 - len(header) - len(segment))


class AuditorTests(unittest.TestCase):
    def test_detects_reported_delta_error(self):
        r = audit_text(make_report())
        errors = [f for f in r["findings"] if f["severity"] == "ERROR"]
        self.assertEqual(len(errors), 1)
        self.assertEqual(errors[0]["calculated_delta"], "-0x4734")
        self.assertFalse(errors[0]["runtime_cause_proven"])

    def test_correct_delta_passes_arithmetic(self):
        r = audit_text(make_report(delta="-0x4734"))
        self.assertFalse(any(f["severity"] == "ERROR" for f in r["findings"]))
        self.assertEqual(r["runtime_checks"]["status"], "NOT_RUN")

    def test_bad_delta_destination_is_exact(self):
        r = audit_text(make_report())
        self.assertEqual(r["address_checks"][1]["reported_delta_resolves_to_va"], "0xdd1fe0")

    def test_rebase_label_difference_is_review_not_error(self):
        text = make_report(delta="-0x4734").replace(
            "### 主渲染路径", "| FUN_014bcbac | 0x13BCBAC | +0x109598 |\n### 主渲染路径")
        r = audit_text(text)
        f = next(x for x in r["findings"] if x["code"] == "LABEL_VA_NAMESPACE_DIFFERENCE")
        self.assertEqual(f["severity"], "REVIEW")
        self.assertEqual(f["difference"], "+0x100000")

    def test_missing_section_is_not_a_false_pass(self):
        with self.assertRaises(ValueError):
            audit_text("# No address table")

    def test_duplicate_anchor_rejected(self):
        text = make_report().replace("### 主渲染路径",
                                     "| onViewRenderJNI | 0x12B3614 | 0 |\n### 主渲染路径")
        with self.assertRaises(ValueError):
            audit_text(text)

    def test_invalid_hash_is_reported(self):
        r = audit_text(make_report(delta="-0x4734", sha="not-a-hash"))
        self.assertTrue(any(x["code"] == "MISSING_OR_INVALID_DSO_SHA256" for x in r["findings"]))

    def test_signed_number_parsing(self):
        self.assertEqual(parse_int("-0x4734"), -0x4734)
        self.assertEqual(parse_int("+0xCB9FBC"), 0xCB9FBC)
        self.assertEqual(parse_int("0"), 0)
        self.assertEqual(parse_int("-42"), -42)

    def test_invalid_delta_is_not_ignored(self):
        with self.assertRaises(ValueError):
            audit_text(make_report(delta="unknown"))

    def test_elf_header_and_segment(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp) / "synthetic.so"
            p.write_bytes(make_elf())
            e = read_elf64(p)
            self.assertEqual(e["segments"][0]["va"], 0x1000)
            self.assertEqual(e["segments"][0]["filesz"], 256)

    def test_truncated_elf_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp) / "truncated.so"
            p.write_bytes(make_elf()[:80])
            with self.assertRaises(ValueError):
                read_elf64(p)

    def test_wrong_architecture_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp) / "wrong.so"
            p.write_bytes(make_elf(machine=62))
            with self.assertRaises(ValueError):
                read_elf64(p)

    def test_invalid_segment_alignment_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp) / "badalign.so"
            p.write_bytes(make_elf(align=3))
            with self.assertRaises(ValueError):
                read_elf64(p)

    def test_optional_elf_identity_and_zero_fill(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp) / "synthetic.so"
            data = make_elf()
            p.write_bytes(data)
            r = audit_text(make_report(delta="+0x180", anchor="0x1000", target="0x1180",
                                       sha=hashlib.sha256(data).hexdigest()))
            audit_elf(r, p)
            self.assertTrue(r["elf_check"]["identity_matches_report"])
            self.assertTrue(r["elf_check"]["addresses"][1]["zero_fill_region"])
            self.assertIsNone(r["elf_check"]["addresses"][1]["file_offset"])
            self.assertEqual(r["elf_check"]["addresses"][0]["file_offset"], "0x0")

    def test_optional_elf_hash_mismatch_is_error(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp) / "synthetic.so"
            p.write_bytes(make_elf())
            r = audit_text(make_report(delta="+0x40", anchor="0x1000", target="0x1040"))
            audit_elf(r, p)
            self.assertTrue(any(f["code"] == "DSO_IDENTITY_NOT_CONFIRMED" for f in r["findings"]))

    def test_output_cannot_overwrite_input(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp) / "status.md"
            original = make_report()
            p.write_text(original, encoding="utf-8")
            with contextlib.redirect_stderr(io.StringIO()):
                rc = main([str(p), "--out", str(p)])
            self.assertEqual(rc, 2)
            self.assertEqual(p.read_text(encoding="utf-8"), original)

    def test_cli_writes_audit_failure_without_runtime_claim(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp) / "status.md"
            out = Path(tmp) / "audit.json"
            p.write_text(make_report(), encoding="utf-8")
            with contextlib.redirect_stdout(io.StringIO()):
                rc = main([str(p), "--out", str(out)])
            r = json.loads(out.read_text(encoding="utf-8"))
            self.assertEqual(rc, 1)
            self.assertEqual(r["summary"]["audit_errors"], 1)
            self.assertEqual(r["elf_check"]["status"], "NOT_RUN_NO_ELF_PROVIDED")
            self.assertEqual(r["runtime_checks"]["status"], "NOT_RUN")


if __name__ == "__main__":
    unittest.main()
