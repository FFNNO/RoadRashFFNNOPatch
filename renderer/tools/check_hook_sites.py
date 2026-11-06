"""Verify the hook-site table in renderer/src/hook_sites.cpp against exe copies.

For every entry it maps the VA to a file offset through the PE section table
and checks that the expected bytes match each exe given on the command line
(default: ../../original and ../../patched RoadRash95.exe).

Usage: py -I check_hook_sites.py [exe ...]
Exit code 0 when every site matches every exe.
"""
import pathlib
import re
import struct
import sys

HERE = pathlib.Path(__file__).resolve().parent
TABLE = HERE.parent / "src" / "hook_sites.cpp"
ROOT = HERE.parent.parent
DEFAULT_EXES = [ROOT / "original" / "RoadRash95.exe", ROOT / "patched" / "RoadRash95.exe"]

ENTRY = re.compile(
    r'\{\s*"(?P<name>\w+)",\s*0x(?P<va>[0-9A-Fa-f]+),\s*\{(?P<bytes>[^}]*)\},\s*(?P<len>\d+),\s*\{(?P<insn>[^}]*)\}'
)


def parse_table(text):
    sites = []
    for m in ENTRY.finditer(text):
        data = bytes(int(b, 16) for b in m.group("bytes").replace(" ", "").split(",") if b)
        insn = [int(x) for x in m.group("insn").replace(" ", "").split(",") if x]
        sites.append((m.group("name"), int(m.group("va"), 16), data, int(m.group("len")), insn))
    return sites


def va_to_offset(pe, va):
    lfanew = struct.unpack_from("<I", pe, 0x3C)[0]
    nsec = struct.unpack_from("<H", pe, lfanew + 6)[0]
    opt_size = struct.unpack_from("<H", pe, lfanew + 20)[0]
    image_base = struct.unpack_from("<I", pe, lfanew + 24 + 28)[0]
    rva = va - image_base
    sec = lfanew + 24 + opt_size
    for i in range(nsec):
        s = sec + 40 * i
        vsize, vaddr, rsize, raw = struct.unpack_from("<IIII", pe, s + 8)
        if vaddr <= rva < vaddr + max(vsize, rsize):
            return raw + (rva - vaddr)
    raise ValueError(f"VA 0x{va:08X} is not inside any section")


def main(argv):
    exes = [pathlib.Path(a) for a in argv] or DEFAULT_EXES
    sites = parse_table(TABLE.read_text(encoding="utf-8"))
    if not sites:
        print(f"FAIL no hook sites parsed from {TABLE}")
        return 1
    failures = 0
    for exe in exes:
        pe = exe.read_bytes()
        for name, va, expected, length, insn in sites:
            off = va_to_offset(pe, va)
            actual = pe[off:off + length]
            ok = actual == expected[:length] and len(expected) == length and sum(insn) == length
            failures += not ok
            print(f"{'OK  ' if ok else 'FAIL'} {exe.parent.name}/{exe.name} {name} VA 0x{va:08X} "
                  f"file 0x{off:X} len {length} insn {insn} expected {expected.hex(' ')} actual {actual.hex(' ')}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
