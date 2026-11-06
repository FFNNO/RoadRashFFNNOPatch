"""Probe Road Rash 95 resource files: palette, horizon BOBs and RSRC chunk tables."""
import os
import struct
import sys
from collections import Counter

from PIL import Image

GAME = sys.argv[1]
OUT = sys.argv[2]
os.makedirs(OUT, exist_ok=True)


def load_palette():
    raw = open(os.path.join(GAME, "DATA", "PALETTE.RAW"), "rb").read()
    # 256 entries x 4 bytes; try RGBx order first.
    return [tuple(raw[i * 4:i * 4 + 3]) for i in range(256)]


def dump_bob(path, width, pal, name):
    data = open(path, "rb").read()
    height = len(data) // width
    img = Image.frombytes("P", (width, height), data[: width * height])
    flat = [c for rgb in pal for c in rgb]
    img.putpalette(flat)
    img.convert("RGB").save(os.path.join(OUT, name))
    return width, height


def fourcc(b):
    return b[::-1].decode("latin1")


def rsrc_table(path):
    data = open(path, "rb").read()
    magic, hdr_len, count = struct.unpack_from("<4sII", data, 0)
    print(f"{os.path.basename(path)}: magic={fourcc(magic)} size={len(data)}")
    # Scan 0x20-byte entries: fourcc, id, offset, length.
    entries = []
    pos = 0
    while True:
        pos = data.find(b"RTBL", pos)  # stored little-endian as 'LBTR'? keep generic below
        break
    i = 0x38
    while i + 16 <= len(data):
        tag, rid, off, length = struct.unpack_from("<4sIII", data, i)
        if not all(32 <= c < 127 for c in tag):
            break
        entries.append((fourcc(tag), rid, off, length))
        i += 0x20
    kinds = Counter(e[0] for e in entries)
    print("  entries:", len(entries), dict(kinds))
    for e in entries[:5]:
        print("   ", e, data[e[2]:e[2] + 24].hex(" "))
    return data, entries


if __name__ == "__main__":
    pal = load_palette()
    for w, name in ((640, "CITY.BOB"), (320, "LCITY.BOB")):
        dims = dump_bob(os.path.join(GAME, "IMAGES", "HORIZONS", name), w, pal, name + ".png")
        print(name, "as", dims)
    for rsc in ("FAMILIES.RSC", "GLOBAL.RSC"):
        rsrc_table(os.path.join(GAME, "DATA", rsc))
