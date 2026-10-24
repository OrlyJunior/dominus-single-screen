#!/usr/bin/env python3
"""Read-only on-disk PE verification; never loads a DLL or inspects a process.

The result authorizes no runtime hook. A static match cannot validate object
lifetimes, calling threads, semantic surfaces, input or pause behavior.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys
from dataclasses import dataclass
from typing import Any

MAX_FILE_BYTES = 1024 * 1024 * 1024
DEFAULT_PROFILE = Path(__file__).resolve().parents[1] / "profiles" / "portrait-20281557.json"


class PEError(ValueError):
    """Malformed or unsupported PE image."""


@dataclass(frozen=True)
class Section:
    name: str
    rva: int
    virtual_size: int
    raw_offset: int
    raw_size: int
    characteristics: int


class PEImage:
    """Small bounded PE32+ parser operating only on immutable file bytes."""

    def __init__(self, data: bytes):
        self.data = data
        self.sections: list[Section] = []
        self.directories: list[tuple[int, int]] = []
        self._parse()

    def checked(self, offset: int, size: int) -> bytes:
        if offset < 0 or size < 0 or offset > len(self.data) - size:
            raise PEError(f"file range out of bounds: offset=0x{offset:X}, size=0x{size:X}")
        return self.data[offset:offset + size]

    def unpack(self, fmt: str, offset: int) -> tuple:
        return struct.unpack(fmt, self.checked(offset, struct.calcsize(fmt)))

    def _parse(self) -> None:
        if self.checked(0, 2) != b"MZ":
            raise PEError("missing DOS MZ signature")
        nt = self.unpack("<I", 0x3C)[0]
        if nt < 0x40 or self.checked(nt, 4) != b"PE\0\0":
            raise PEError("missing or invalid NT signature")
        machine, count, _, _, _, opt_size, _ = self.unpack("<HHIIIHH", nt + 4)
        if machine != 0x8664:
            raise PEError(f"expected AMD64 machine 0x8664, found 0x{machine:04X}")
        if not 1 <= count <= 96:
            raise PEError("section count must be between 1 and 96")
        opt = nt + 24
        self.checked(opt, opt_size)
        if opt_size < 112 or self.unpack("<H", opt)[0] != 0x20B:
            raise PEError("expected PE32+ optional header")
        self.machine = machine
        self.size_of_image = self.unpack("<I", opt + 56)[0]
        self.size_of_headers = self.unpack("<I", opt + 60)[0]
        table = opt + opt_size
        self.checked(table, count * 40)
        if self.size_of_headers < table + count * 40 or self.size_of_headers > len(self.data):
            raise PEError("invalid SizeOfHeaders")
        number_of_dirs = self.unpack("<I", opt + 108)[0]
        if number_of_dirs > (opt_size - 112) // 8:
            raise PEError("data directory count exceeds optional header")
        self.directories = [self.unpack("<II", opt + 112 + index * 8)
                            for index in range(number_of_dirs)]
        for index in range(count):
            entry = table + index * 40
            raw_name = self.checked(entry, 8).split(b"\0", 1)[0]
            name = raw_name.decode("ascii", errors="replace")
            virtual_size, rva, raw_size, raw_offset = self.unpack("<IIII", entry + 8)
            characteristics = self.unpack("<I", entry + 36)[0]
            extent = max(virtual_size, raw_size)
            if rva < self.size_of_headers or rva + extent > self.size_of_image:
                raise PEError(f"invalid virtual range for section {name}")
            if raw_size:
                if raw_offset < self.size_of_headers:
                    raise PEError(f"section {name} overlaps headers")
                self.checked(raw_offset, raw_size)
            section = Section(name, rva, virtual_size, raw_offset, raw_size, characteristics)
            for other in self.sections:
                if extent and max(other.virtual_size, other.raw_size):
                    if rva < other.rva + max(other.virtual_size, other.raw_size) and other.rva < rva + extent:
                        raise PEError("overlapping virtual sections")
                if raw_size and other.raw_size:
                    if raw_offset < other.raw_offset + other.raw_size and other.raw_offset < raw_offset + raw_size:
                        raise PEError("overlapping raw sections")
            self.sections.append(section)

    def offset_for_rva(self, rva: int, size: int = 1) -> int:
        if rva < 0 or size <= 0 or rva > 0xFFFFFFFF - size + 1:
            raise PEError("invalid RVA range")
        if rva < self.size_of_headers and rva + size <= self.size_of_headers:
            self.checked(rva, size)
            return rva
        for section in self.sections:
            delta = rva - section.rva
            if delta >= 0 and delta + size <= section.raw_size:
                offset = section.raw_offset + delta
                self.checked(offset, size)
                return offset
        raise PEError(f"RVA 0x{rva:X} is not completely file-backed")

    def read_rva(self, rva: int, size: int) -> bytes:
        return self.checked(self.offset_for_rva(rva, size), size)

    def cstring(self, rva: int, maximum: int = 512) -> str:
        chars = bytearray()
        for delta in range(maximum):
            byte = self.read_rva(rva + delta, 1)[0]
            if not byte:
                try:
                    return chars.decode("ascii")
                except UnicodeDecodeError as error:
                    raise PEError("non-ASCII import name") from error
            chars.append(byte)
        raise PEError("unterminated or oversized import name")

    def directory(self, index: int) -> tuple[int, int]:
        return self.directories[index] if index < len(self.directories) else (0, 0)

    def imports(self) -> list[dict[str, Any]]:
        rva, size = self.directory(1)
        if not rva and not size:
            return []
        if not rva or size < 20:
            raise PEError("invalid import directory")
        self.offset_for_rva(rva, size)
        result = []
        for index in range(min(size // 20, 4096)):
            desc = struct.unpack("<IIIII", self.read_rva(rva + index * 20, 20))
            if not any(desc):
                return result
            original, _, _, name_rva, first = desc
            dll = self.cstring(name_rva)
            thunk_rva = original or first
            if not thunk_rva:
                raise PEError("import descriptor has no thunk table")
            symbols = []
            for thunk_index in range(65536):
                thunk = struct.unpack("<Q", self.read_rva(thunk_rva + thunk_index * 8, 8))[0]
                if not thunk:
                    break
                if thunk & (1 << 63):
                    symbols.append({"ordinal": thunk & 0xFFFF})
                else:
                    if thunk > 0xFFFFFFFF:
                        raise PEError("import name RVA exceeds 32-bit range")
                    self.read_rva(thunk, 2)
                    symbols.append({"name": self.cstring(thunk + 2)})
            else:
                raise PEError("unterminated import thunk table")
            result.append({"dll": dll, "symbols": symbols})
        raise PEError("unterminated or oversized import descriptor table")

    def function_starts(self) -> set[int]:
        rva, size = self.directory(3)
        if not rva and not size:
            return set()
        if not rva or size % 12:
            raise PEError("invalid AMD64 exception directory size")
        self.offset_for_rva(rva, size)
        starts = set()
        for offset in range(0, size, 12):
            begin, end, unwind = struct.unpack("<III", self.read_rva(rva + offset, 12))
            if begin == end == unwind == 0:
                continue
            if begin >= end:
                raise PEError("invalid RUNTIME_FUNCTION range")
            self.offset_for_rva(begin, end - begin)
            self.offset_for_rva(unwind & ~1, 1)
            starts.add(begin)
        return starts

    def signature_matches(self, pattern: bytes) -> list[int]:
        if not pattern:
            raise PEError("empty signature")
        matches = []
        for section in self.sections:
            if section.name != ".text" or not section.characteristics & 0x20000000:
                continue
            payload = self.checked(section.raw_offset, section.raw_size)
            offset = payload.find(pattern)
            while offset >= 0:
                matches.append(section.rva + offset)
                offset = payload.find(pattern, offset + 1)
        return matches


def integer(value: Any) -> int:
    return int(value, 0) if isinstance(value, str) else int(value)


def inspect_bytes(data: bytes, profile: dict[str, Any]) -> dict[str, Any]:
    pe = PEImage(data)
    imports = pe.imports()
    starts = pe.function_starts()
    actual_hash = hashlib.sha256(data).hexdigest().upper()
    expected_hash = str(profile["sha256"]).upper()
    signatures = []
    for definition in profile["signatures"]:
        pattern = bytes.fromhex(definition["pattern"])
        matches = pe.signature_matches(pattern)
        candidates = [rva + integer(definition["function_offset"]) for rva in matches]
        expected_rva = integer(definition["expected_function_rva"])
        unique = len(matches) == 1
        pdata_match = unique and candidates[0] in starts
        expected_match = unique and candidates[0] == expected_rva
        signatures.append({
            "name": definition["name"],
            "match_count": len(matches),
            "match_rvas": [f"0x{rva:X}" for rva in matches],
            "candidate_function_rvas": [f"0x{rva:X}" for rva in candidates],
            "expected_function_rva": f"0x{expected_rva:X}",
            "unique": unique,
            "matches_expected_rva": expected_match,
            "pdata_corroborated": pdata_match,
            "passed": unique and expected_match and pdata_match,
        })
    imported_names = {item["dll"].lower() for item in imports}
    required_imports = {name.lower() for name in profile.get("required_imports", [])}
    missing = sorted(required_imports - imported_names)
    hash_match = actual_hash == expected_hash
    passed = hash_match and bool(signatures) and all(item["passed"] for item in signatures) and not missing
    return {
        "schema_version": 1,
        "operation": "static_read_only_file_inspection",
        "profile_id": profile["profile_id"],
        "machine": "AMD64",
        "pe_format": "PE32+",
        "size_bytes": len(data),
        "sha256": actual_hash,
        "expected_sha256": expected_hash,
        "sha256_matches": hash_match,
        "signatures": signatures,
        "imports": imports,
        "missing_required_imports": missing,
        "static_profile_matches": passed,
        "runtime_hooks_authorized": False,
        "runtime_status": "unvalidated_disabled",
        "notice": "A static match does not validate runtime calls, pause, input, context or surfaces.",
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path, help="Existing on-disk game.exe; opened read-only")
    parser.add_argument("--profile", type=Path, default=DEFAULT_PROFILE)
    args = parser.parse_args(argv)
    try:
        profile = json.loads(args.profile.read_text(encoding="utf-8-sig"))
        if args.executable.stat().st_size > MAX_FILE_BYTES:
            raise PEError("file exceeds the 1 GiB inspection bound")
        with args.executable.open("rb") as source:
            data = source.read(MAX_FILE_BYTES + 1)
        if len(data) > MAX_FILE_BYTES:
            raise PEError("file exceeds the 1 GiB inspection bound")
        result = inspect_bytes(data, profile)
        result["path"] = str(args.executable.resolve())
        code = 0 if result["static_profile_matches"] else 2
    except (OSError, ValueError, KeyError, TypeError, struct.error) as error:
        result = {"operation": "static_read_only_file_inspection", "error": str(error),
                  "static_profile_matches": False, "runtime_hooks_authorized": False}
        code = 1
    print(json.dumps(result, indent=2, sort_keys=True))
    return code


if __name__ == "__main__":
    sys.exit(main())
