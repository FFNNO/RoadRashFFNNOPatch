"""Verify the hook-site table in renderer/src/hook_sites.cpp against exe copies.

For every entry it maps the VA to a file offset through the PE section table
and checks that the expected bytes match each exe given on the command line
(default: ../../original and ../../patched RoadRash95.exe).

It also checks the draw distance patch table in renderer/src/draw_distance.cpp:
the expected bytes of every site, that every base relocation into the moved
engine arrays is either patched or on the deliberate keep list, and that every
0x1bff / 0x1c00 immediate in the reciprocal table readers is patched.

And it checks graphics menu 19, which renderer/src/options_menu.cpp relocates into the DLL: the
shipped item table, the item count and menu table entry, that the menu table pointer is the only
base relocation into the item array, and that the dwords the mod writes lie in a writable section.

Usage: py -I check_hook_sites.py [exe ...]
Exit code 0 when every site matches every exe.
"""
import pathlib
import re
import struct
import sys

HERE = pathlib.Path(__file__).resolve().parent
TABLE = HERE.parent / "src" / "hook_sites.cpp"
DRAW_TABLE = HERE.parent / "src" / "draw_distance.cpp"
MENU_TABLE = HERE.parent / "src" / "options_menu.cpp"
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


DRAW_ENTRY = re.compile(
    r"\{\s*0x(?P<va>[0-9A-Fa-f]{8}),\s*(?P<len>\d+),\s*\{(?P<bytes>[^}]*)\},\s*(?P<abs>-?\d+),\s*"
    r"(?P<fo>\d+),\s*(?P<fs>\d+),\s*DrawField::(?P<field>\w+)\s*\}"
)

# Moved engine arrays: base relocations whose target lies in one of these ranges must be patched.
DRAW_RANGES = [(0x00597C90, 0x0059EC90), (0x00608320, 0x006083E0), (0x006084A0, 0x006127C8)]
# Relocation sites (address of the 4-byte operand) that keep the engine's arrays on purpose.
DRAW_KEEP = {0x0045AAC4, 0x0045AB35, 0x0045AB3C, 0x0044EE47, 0x0041EDA0, 0x0041EF6E, 0x0041EFC8, 0x0045A494}
# Functions that index the reciprocal table (start VA, size from out/decomp.c).
DRAW_RECIP_READERS = [(0x0041CFC0, 795), (0x0041D3B0, 1082), (0x0044E870, 1119), (0x00459170, 2273),
                      (0x00459CB0, 2032), (0x0045A6A0, 102), (0x004666A0, 766), (0x00466A60, 685),
                      (0x00466EB0, 793), (0x00467290, 792), (0x00467670, 706)]
DRAW_CLAMP_PATTERNS = (bytes([0xFF, 0x1B, 0x00, 0x00]), bytes([0x00, 0x1C, 0x00, 0x00]))


# Graphics menu 19 (options_menu.h): items, the count WM_CREATE copies, the menu table entry.
MENU_ITEMS = 0x005A08A8
MENU_ITEMS_SIZE = 8 * 0x38
MENU_COUNT = 0x005A0A70
MENU_TABLE_COUNT = 0x005946D4
MENU_TABLE_ITEMS = 0x005946D8
# Every base relocation into these ranges, by operand address (anything else would also need rewriting).
MENU_RELOCATIONS = {
    (MENU_ITEMS, MENU_ITEMS + MENU_ITEMS_SIZE): {MENU_TABLE_ITEMS},  # the table pointer, rewritten
    (MENU_COUNT, MENU_COUNT + 4): {0x0043E579},                      # WM_CREATE reads the count
    (MENU_TABLE_COUNT, MENU_TABLE_COUNT + 4): {0x0043E57E},          # WM_CREATE writes the table count
}
IMAGE_SCN_MEM_WRITE = 0x80000000


def parse_menu_table(text):
    m = re.search(r"kShippedGraphicsMenuItems\[kGraphicsMenuItemsSize\]\s*=\s*\{(?P<body>.*?)\};", text, re.S)
    if not m:
        return b""
    body = re.sub(r"//[^\n]*", "", m.group("body"))
    return bytes(int(b, 16) for b in re.findall(r"0x([0-9A-Fa-f]{2})\b", body))


def section_characteristics(pe, va):
    lfanew = struct.unpack_from("<I", pe, 0x3C)[0]
    nsec = struct.unpack_from("<H", pe, lfanew + 6)[0]
    opt_size = struct.unpack_from("<H", pe, lfanew + 20)[0]
    image_base = struct.unpack_from("<I", pe, lfanew + 24 + 28)[0]
    sec = lfanew + 24 + opt_size
    for i in range(nsec):
        s = sec + 40 * i
        vsize, vaddr = struct.unpack_from("<II", pe, s + 8)
        if vaddr <= va - image_base < vaddr + vsize:
            return struct.unpack_from("<I", pe, s + 36)[0]
    return 0


def check_menu(exe, pe, items):
    failures = 0
    name = f"{exe.parent.name}/{exe.name}"
    off = va_to_offset(pe, MENU_ITEMS)
    actual = pe[off:off + MENU_ITEMS_SIZE]
    ok = len(items) == MENU_ITEMS_SIZE and actual == items
    failures += not ok
    print(f"{'OK  ' if ok else 'FAIL'} {name} menu 19 items VA 0x{MENU_ITEMS:08X} ({len(items)} table bytes)")
    dword = lambda va: struct.unpack_from("<I", pe, va_to_offset(pe, va))[0]
    count, table_count, table_items = dword(MENU_COUNT), dword(MENU_TABLE_COUNT), dword(MENU_TABLE_ITEMS)
    ok = count == 8 and table_count == 0 and table_items == MENU_ITEMS
    failures += not ok
    print(f"{'OK  ' if ok else 'FAIL'} {name} menu 19 count 0x{MENU_COUNT:08X} = {count}, table count = {table_count}, "
          f"table items = 0x{table_items:08X}")
    relocs = relocation_sites(pe)
    for (lo, hi), expected in MENU_RELOCATIONS.items():
        found = {site for site, target in relocs if lo <= target < hi}
        ok = found == expected
        failures += not ok
        print(f"{'OK  ' if ok else 'FAIL'} {name} menu relocations into 0x{lo:08X}..0x{hi:08X}: "
              f"{sorted(hex(v) for v in found)} expected {sorted(hex(v) for v in expected)}")
    writable = all(section_characteristics(pe, va) & IMAGE_SCN_MEM_WRITE for va in (MENU_COUNT, MENU_TABLE_COUNT, MENU_TABLE_ITEMS))
    failures += not writable
    print(f"{'OK  ' if writable else 'FAIL'} {name} menu dwords the mod writes are in a writable section")
    return failures


def parse_draw_table(text):
    sites = []
    for m in DRAW_ENTRY.finditer(text):
        data = bytes(int(b, 16) for b in m.group("bytes").replace(" ", "").split(",") if b)
        sites.append((int(m.group("va"), 16), int(m.group("len")), data, int(m.group("abs")),
                      int(m.group("fo")), int(m.group("fs")), m.group("field")))
    return sites


def relocation_sites(pe):
    """(operand VA, preferred target) of every IMAGE_REL_BASED_HIGHLOW base relocation."""
    lfanew = struct.unpack_from("<I", pe, 0x3C)[0]
    image_base = struct.unpack_from("<I", pe, lfanew + 24 + 28)[0]
    rva, size = struct.unpack_from("<II", pe, lfanew + 24 + 96 + 5 * 8)
    p = va_to_offset(pe, image_base + rva)
    end = p + size
    out = []
    while p < end:
        page, block = struct.unpack_from("<II", pe, p)
        if block == 0:
            break
        for i in range((block - 8) // 2):
            entry = struct.unpack_from("<H", pe, p + 8 + 2 * i)[0]
            if entry >> 12 == 3:
                site = image_base + page + (entry & 0xFFF)
                out.append((site, struct.unpack_from("<I", pe, va_to_offset(pe, site))[0]))
        p += block
    return out


def check_draw_table(exe, pe, sites):
    failures = 0
    for va, length, expected, absoff, fo, fs, field in sites:
        off = va_to_offset(pe, va)
        actual = pe[off:off + length]
        ok = actual == expected and fo + fs <= length and (absoff < 0 or absoff + 4 <= length)
        failures += not ok
        print(f"{'OK  ' if ok else 'FAIL'} {exe.parent.name}/{exe.name} draw {field} VA 0x{va:08X} "
              f"expected {expected.hex(' ')} actual {actual.hex(' ')}")
    patched_operands = {va + fo for va, _, _, _, fo, _, field in sites if field.endswith("Addr")}
    moved = {site for site, target in relocation_sites(pe) if any(a <= target < b for a, b in DRAW_RANGES)}
    missing = sorted(moved - patched_operands - DRAW_KEEP)
    extra = sorted(patched_operands - moved)
    ok = not missing and not extra
    failures += not ok
    print(f"{'OK  ' if ok else 'FAIL'} {exe.parent.name}/{exe.name} draw relocations: {len(moved)} into the moved "
          f"arrays, {len(patched_operands)} patched, {len(DRAW_KEEP)} kept; missing {[hex(v) for v in missing]}, "
          f"not relocated {[hex(v) for v in extra]}")
    patched_fields = {va + fo for va, _, _, _, fo, _, _ in sites}
    unpatched = []
    for start, size in DRAW_RECIP_READERS:
        off = va_to_offset(pe, start)
        blob = pe[off:off + size]
        for pattern in DRAW_CLAMP_PATTERNS:
            i = blob.find(pattern)
            while i >= 0:
                if start + i not in patched_fields:
                    unpatched.append(start + i)
                i = blob.find(pattern, i + 1)
    ok = not unpatched
    failures += not ok
    print(f"{'OK  ' if ok else 'FAIL'} {exe.parent.name}/{exe.name} draw clamps: every 0x1bff/0x1c00 immediate in "
          f"the reciprocal table readers is patched; unpatched {[hex(v) for v in unpatched]}")
    return failures


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
    draw_sites = parse_draw_table(DRAW_TABLE.read_text(encoding="utf-8"))
    if not draw_sites:
        print(f"FAIL no draw distance sites parsed from {DRAW_TABLE}")
        return 1
    menu_items = parse_menu_table(MENU_TABLE.read_text(encoding="utf-8"))
    if len(menu_items) != MENU_ITEMS_SIZE:
        print(f"FAIL {len(menu_items)} menu item bytes parsed from {MENU_TABLE}, expected {MENU_ITEMS_SIZE}")
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
        failures += check_draw_table(exe, pe, draw_sites)
        failures += check_menu(exe, pe, menu_items)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
