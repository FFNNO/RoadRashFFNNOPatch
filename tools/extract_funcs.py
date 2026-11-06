"""Build a context bundle of decompiled functions for an external model.

Usage:
    py -I extract_funcs.py OUT_DIR ADDR [ADDR ...]

ADDR is a hex VA such as 0046e630. Reads out/decomp.c (Ghidra export, local only) and the
two exe copies, then writes into OUT_DIR:
    decomp_excerpts.c  full decompilation of each requested function
    xrefs.md           every function that calls each requested function (with the call line)
    prologues.md       first 32 bytes at each VA in the original and patched exe
"""
import os
import re
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DECOMP = os.path.join(ROOT, "out", "decomp.c")
EXES = {
    "original": os.path.join(ROOT, "original", "RoadRash95.exe"),
    "patched": os.path.join(ROOT, "patched", "RoadRash95.exe"),
}
HEADER = re.compile(r"^// ==== FUNC ([0-9a-f]{8}) (\S+) size=(\d+)")


def split_functions(path):
    """Return {addr: (name, text)} for every function in the Ghidra export."""
    funcs = {}
    addr = name = None
    lines = []
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = HEADER.match(line)
            if m:
                if addr:
                    funcs[addr] = (name, "".join(lines))
                addr, name, lines = m.group(1), m.group(2), [line]
            elif addr:
                lines.append(line)
    if addr:
        funcs[addr] = (name, "".join(lines))
    return funcs


def va_to_offset(data, va):
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    count = struct.unpack_from("<H", data, pe + 6)[0]
    opt = struct.unpack_from("<H", data, pe + 20)[0]
    base = struct.unpack_from("<I", data, pe + 24 + 28)[0]
    rva = va - base
    for i in range(count):
        o = pe + 24 + opt + i * 40
        vsize, vaddr, rsize, raw = struct.unpack_from("<IIII", data, o + 8)
        if vaddr <= rva < vaddr + max(vsize, rsize):
            return rva - vaddr + raw
    raise ValueError(f"VA {va:#x} not in any section")


def main():
    out_dir, addrs = sys.argv[1], [a.lower().zfill(8) for a in sys.argv[2:]]
    os.makedirs(out_dir, exist_ok=True)
    funcs = split_functions(DECOMP)

    with open(os.path.join(out_dir, "decomp_excerpts.c"), "w", encoding="utf-8") as f:
        f.write("// Ghidra 12.1.4 decompilation of RoadRash95.exe (Steam 30th Anniversary), selected functions.\n\n")
        for a in addrs:
            if a not in funcs:
                f.write(f"// ==== FUNC {a} NOT FOUND in export\n\n")
                continue
            f.write(funcs[a][1] + "\n")

    with open(os.path.join(out_dir, "xrefs.md"), "w", encoding="utf-8") as f:
        f.write("# Callers of the selected functions\n\n")
        for a in addrs:
            # Calls usually go through Ghidra-named jump thunks (thunk_FUN_xxxxxxxx).
            pattern = re.compile(rf"(?<![0-9A-Za-z_])(?:thunk_)?FUN_{a}\b")
            f.write(f"## FUN_{a}\n\n")
            hits = 0
            for caller, (name, text) in funcs.items():
                if caller == a:
                    continue
                for line in text.splitlines():
                    if pattern.search(line):
                        f.write(f"- `{caller} {name}`: `{line.strip()}`\n")
                        hits += 1
            if not hits:
                f.write("- no direct callers in the decompilation (may be called through a thunk or pointer)\n")
            f.write("\n")

    with open(os.path.join(out_dir, "prologues.md"), "w", encoding="utf-8") as f:
        f.write("# First 32 bytes at each function VA\n\n")
        blobs = {k: open(p, "rb").read() for k, p in EXES.items() if os.path.exists(p)}
        for a in addrs:
            va = int(a, 16)
            f.write(f"## {a}\n\n")
            for label, data in blobs.items():
                off = va_to_offset(data, va)
                f.write(f"- {label} (file offset {off:#x}): `{data[off:off + 32].hex(' ')}`\n")
            f.write("\n")

    found = sum(1 for a in addrs if a in funcs)
    print(f"wrote bundle to {out_dir}: {found}/{len(addrs)} functions found")


if __name__ == "__main__":
    main()
