"""Synthetic on-disk PE structures only; no game process or DLL loading."""
import hashlib
import importlib.util
from pathlib import Path
import struct
import sys
import unittest

MODULE_PATH = Path(__file__).resolve().parents[1] / "tools" / "inspect_build.py"
SPEC = importlib.util.spec_from_file_location("dominus_inspect_build", MODULE_PATH)
inspect_build = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = inspect_build
SPEC.loader.exec_module(inspect_build)


def fixture():
    data = bytearray(0xA00)
    data[:2] = b"MZ"
    struct.pack_into("<I", data, 0x3C, 0x80)
    data[0x80:0x84] = b"PE\0\0"
    struct.pack_into("<HHIIIHH", data, 0x84, 0x8664, 3, 0, 0, 0, 240, 0)
    opt = 0x98
    struct.pack_into("<H", data, opt, 0x20B)
    struct.pack_into("<II", data, opt + 56, 0x4000, 0x200)
    struct.pack_into("<I", data, opt + 108, 16)
    struct.pack_into("<II", data, opt + 112 + 8, 0x3040, 40)
    struct.pack_into("<II", data, opt + 112 + 24, 0x2000, 12)
    for index, (name, rva, raw, size, flags) in enumerate([
        (b".text", 0x1000, 0x200, 0x400, 0x60000020),
        (b".pdata", 0x2000, 0x600, 0x200, 0x40000040),
        (b".rdata", 0x3000, 0x800, 0x200, 0x40000040),
    ]):
        entry = opt + 240 + index * 40
        data[entry:entry + 8] = name.ljust(8, b"\0")
        struct.pack_into("<IIII", data, entry + 8, size, rva, size, raw)
        struct.pack_into("<I", data, entry + 36, flags)
    pattern = b"\x48\x89\xAA\xBB\xCC\xDD\xEE\xFF"
    data[0x220:0x228] = pattern
    struct.pack_into("<III", data, 0x600, 0x1010, 0x1100, 0x3000)
    struct.pack_into("<IIIII", data, 0x840, 0x30A0, 0, 0, 0x3080, 0x30A0)
    data[0x880:0x88A] = b"d3d11.dll\0"
    struct.pack_into("<QQQ", data, 0x8A0, 0x30C0, (1 << 63) | 2, 0)
    data[0x8C0:0x8C2] = b"\0\0"
    data[0x8C2:0x8D4] = b"D3D11CreateDevice\0"
    profile = {
        "profile_id": "synthetic-only",
        "sha256": hashlib.sha256(data).hexdigest().upper(),
        "required_imports": ["d3d11.dll"],
        "signatures": [{"name": "fixture", "pattern": pattern.hex(" "),
                        "function_offset": "-0x10", "expected_function_rva": "0x1010"}],
    }
    return data, profile


class PEInspectionTests(unittest.TestCase):
    def test_valid_fixture_and_runtime_always_disabled(self):
        data, profile = fixture()
        result = inspect_build.inspect_bytes(bytes(data), profile)
        self.assertTrue(result["static_profile_matches"])
        self.assertFalse(result["runtime_hooks_authorized"])
        self.assertEqual(result["runtime_status"], "unvalidated_disabled")
        self.assertEqual(result["imports"][0]["symbols"], [
            {"name": "D3D11CreateDevice"}, {"ordinal": 2}])
        self.assertTrue(result["signatures"][0]["pdata_corroborated"])

    def test_hash_mismatch_fails_closed(self):
        data, profile = fixture()
        data[-1] ^= 1
        result = inspect_build.inspect_bytes(bytes(data), profile)
        self.assertFalse(result["static_profile_matches"])
        self.assertFalse(result["sha256_matches"])

    def test_profile_cannot_authorize_runtime(self):
        data, profile = fixture()
        profile["runtime_hooks_enabled"] = True
        result = inspect_build.inspect_bytes(bytes(data), profile)
        self.assertTrue(result["static_profile_matches"])
        self.assertFalse(result["runtime_hooks_authorized"])

    def test_multiple_signature_matches_fail_closed(self):
        data, profile = fixture()
        data[0x230:0x238] = data[0x220:0x228]
        result = inspect_build.inspect_bytes(bytes(data), profile)
        self.assertEqual(result["signatures"][0]["match_count"], 2)
        self.assertFalse(result["signatures"][0]["passed"])

    def test_signature_outside_text_is_ignored(self):
        data, profile = fixture()
        data[0x900:0x908] = data[0x220:0x228]
        result = inspect_build.inspect_bytes(bytes(data), profile)
        self.assertEqual(result["signatures"][0]["match_count"], 1)

    def test_missing_pattern_fails_closed(self):
        data, profile = fixture()
        data[0x220:0x228] = b"\x90" * 8
        result = inspect_build.inspect_bytes(bytes(data), profile)
        self.assertEqual(result["signatures"][0]["match_count"], 0)
        self.assertFalse(result["static_profile_matches"])

    def test_wrong_expected_rva_fails_closed(self):
        data, profile = fixture()
        profile["signatures"][0]["expected_function_rva"] = "0x1020"
        result = inspect_build.inspect_bytes(bytes(data), profile)
        self.assertFalse(result["signatures"][0]["matches_expected_rva"])

    def test_missing_pdata_corroboration_fails_closed(self):
        data, profile = fixture()
        struct.pack_into("<III", data, 0x600, 0x1000, 0x1100, 0x3000)
        result = inspect_build.inspect_bytes(bytes(data), profile)
        self.assertFalse(result["signatures"][0]["pdata_corroborated"])

    def test_missing_required_import_fails_closed(self):
        data, profile = fixture()
        profile["required_imports"].append("xinput1_4.dll")
        result = inspect_build.inspect_bytes(bytes(data), profile)
        self.assertEqual(result["missing_required_imports"], ["xinput1_4.dll"])
        self.assertFalse(result["static_profile_matches"])

    def test_truncated_images_rejected(self):
        data, _ = fixture()
        for length in [0, 1, 0x3F, 0x81, 0x100, 0x1FF, 0x9FF]:
            with self.subTest(length=length), self.assertRaises(inspect_build.PEError):
                inspect_build.PEImage(bytes(data[:length]))

    def test_wrong_machine_rejected(self):
        data, _ = fixture()
        struct.pack_into("<H", data, 0x84, 0x14C)
        with self.assertRaisesRegex(inspect_build.PEError, "AMD64"):
            inspect_build.PEImage(bytes(data))

    def test_excessive_directory_count_rejected(self):
        data, _ = fixture()
        struct.pack_into("<I", data, 0x98 + 108, 17)
        with self.assertRaisesRegex(inspect_build.PEError, "directory count"):
            inspect_build.PEImage(bytes(data))

    def test_raw_overlap_rejected(self):
        data, _ = fixture()
        struct.pack_into("<I", data, 0x188 + 40 + 20, 0x400)
        with self.assertRaisesRegex(inspect_build.PEError, "overlapping raw"):
            inspect_build.PEImage(bytes(data))

    def test_virtual_overlap_rejected(self):
        data, _ = fixture()
        struct.pack_into("<I", data, 0x188 + 40 + 12, 0x1200)
        with self.assertRaisesRegex(inspect_build.PEError, "overlapping virtual"):
            inspect_build.PEImage(bytes(data))

    def test_unbacked_virtual_tail_rejected(self):
        data, _ = fixture()
        struct.pack_into("<I", data, 0x188 + 8, 0x800)
        image = inspect_build.PEImage(bytes(data))
        with self.assertRaisesRegex(inspect_build.PEError, "file-backed"):
            image.read_rva(0x1400, 1)

    def test_import_name_out_of_bounds_rejected(self):
        data, profile = fixture()
        struct.pack_into("<I", data, 0x840 + 12, 0x5000)
        with self.assertRaises(inspect_build.PEError):
            inspect_build.inspect_bytes(bytes(data), profile)

    def test_missing_descriptor_terminator_rejected(self):
        data, profile = fixture()
        struct.pack_into("<II", data, 0x98 + 112 + 8, 0x3040, 20)
        with self.assertRaisesRegex(inspect_build.PEError, "unterminated"):
            inspect_build.inspect_bytes(bytes(data), profile)

    def test_bad_exception_range_rejected(self):
        data, profile = fixture()
        struct.pack_into("<III", data, 0x600, 0x1100, 0x1010, 0x3000)
        with self.assertRaisesRegex(inspect_build.PEError, "RUNTIME_FUNCTION"):
            inspect_build.inspect_bytes(bytes(data), profile)

    def test_empty_pattern_rejected(self):
        data, profile = fixture()
        profile["signatures"][0]["pattern"] = ""
        with self.assertRaisesRegex(inspect_build.PEError, "empty signature"):
            inspect_build.inspect_bytes(bytes(data), profile)


if __name__ == "__main__":
    unittest.main()
