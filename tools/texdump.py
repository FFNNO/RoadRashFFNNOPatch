"""Dump Road Rash 95 textures to PNG with stable content IDs.

Usage:
    py -I texdump.py [GAME_DIR] [OUT_DIR]

Outputs OUT_DIR/<kind>/<id>.png plus OUT_DIR/manifest.json.

Texture ID format: "<w>x<h>_<crc32 of level-0 indices as 8 hex digits>". The same ID is
computed by the GPU renderer at runtime to look up replacement textures, so it must only
depend on the original level-0 pixel indices and size.

Palette index 0 is exported as fully transparent (colour-key), every other index opaque.
"""
import json
import os
import struct
import sys
import zlib

from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from famrsc import get, read_rsc  # noqa: E402

DEFAULT_GAME = r"C:\Program Files (x86)\Steam\steamapps\common\RoadRash"
DEFAULT_OUT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "dumps", "textures")

PDAT_TAG = b"TADP"  # 'PDAT' stored byte-reversed
PDAT_MIP_MAGIC = 0xBC614E
HORIZON_WIDTHS = {230400: 720, 57600: 360}


def load_palette(game_dir):
    raw = open(os.path.join(game_dir, "DATA", "PALETTE.RAW"), "rb").read()
    return [tuple(raw[i * 4:i * 4 + 3]) for i in range(256)]


def texture_id(width, height, pixels):
    return f"{width}x{height}_{zlib.crc32(pixels) & 0xFFFFFFFF:08x}"


def to_rgba(width, height, pixels, palette, key_zero=True):
    img = Image.new("RGBA", (width, height))
    lut = [palette[i] + ((0,) if key_zero and i == 0 else (255,)) for i in range(256)]
    img.putdata([lut[p] for p in pixels])
    return img


def iter_pdat_mips(payload):
    """Yield (chunk_offset, width, height, levels, level0_pixels) for PC mip-chain PDATs."""
    pos = 0
    while True:
        p = payload.find(PDAT_TAG, pos)
        if p < 0:
            return
        pos = p + 4
        if p + 0x44 > len(payload) or struct.unpack_from("<I", payload, p + 8)[0] != PDAT_MIP_MAGIC:
            continue
        width, height, levels = struct.unpack_from("<III", payload, p + 0x28)
        if not (1 <= width <= 1024 and 1 <= height <= 1024 and 1 <= levels <= 12):
            continue
        offset = struct.unpack_from("<iii", payload, p + 0x38)[2]
        start = p + offset
        pixels = payload[start:start + width * height]
        if len(pixels) != width * height:
            continue
        yield p, width, height, levels, pixels


def dump_rsc(path, kind, palette, out_dir, manifest):
    data, entries = read_rsc(path)
    count = 0
    for fourcc, rid, off, length in entries:
        if length <= 24:
            continue
        payload, info = get(data, (fourcc, rid, off, length))
        if info and info[2] != info[3]:
            raise ValueError(f"{os.path.basename(path)} id {rid}: CRC mismatch")
        for chunk_off, width, height, levels, pixels in iter_pdat_mips(payload):
            tid = texture_id(width, height, pixels)
            entry = manifest.setdefault(tid, {"kind": kind, "width": width, "height": height,
                                              "levels": levels, "uses": []})
            entry["uses"].append({"file": os.path.basename(path), "chunk": fourcc.strip(),
                                  "id": rid, "pdat_offset": chunk_off})
            png = os.path.join(out_dir, kind, tid + ".png")
            if not os.path.exists(png):
                to_rgba(width, height, pixels, palette).save(png)
                count += 1
    return count


def dump_horizons(game_dir, palette, out_dir, manifest):
    folder = os.path.join(game_dir, "IMAGES", "HORIZONS")
    count = 0
    for name in sorted(os.listdir(folder)):
        pixels = open(os.path.join(folder, name), "rb").read()
        width = HORIZON_WIDTHS.get(len(pixels))
        if width is None:
            continue
        height = len(pixels) // width
        tid = texture_id(width, height, pixels)
        manifest.setdefault(tid, {"kind": "horizons", "width": width, "height": height,
                                  "levels": 1, "uses": []})["uses"].append({"file": name})
        to_rgba(width, height, pixels, palette, key_zero=False).save(os.path.join(out_dir, "horizons", tid + ".png"))
        count += 1
    return count


def main():
    game_dir = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_GAME
    out_dir = sys.argv[2] if len(sys.argv) > 2 else DEFAULT_OUT
    for kind in ("families", "global", "horizons"):
        os.makedirs(os.path.join(out_dir, kind), exist_ok=True)
    palette = load_palette(game_dir)
    manifest = {}
    n_fam = dump_rsc(os.path.join(game_dir, "DATA", "FAMILIES.RSC"), "families", palette, out_dir, manifest)
    n_glb = dump_rsc(os.path.join(game_dir, "DATA", "GLOBAL.RSC"), "global", palette, out_dir, manifest)
    n_hor = dump_horizons(game_dir, palette, out_dir, manifest)
    with open(os.path.join(out_dir, "manifest.json"), "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=1, sort_keys=True)
    uses = sum(len(v["uses"]) for v in manifest.values())
    per_kind = {k: sum(1 for v in manifest.values() if v["kind"] == k) for k in ("families", "global", "horizons")}
    print(f"unique per kind={per_kind} new_files={n_fam + n_glb + n_hor} unique={len(manifest)} uses={uses}")


if __name__ == "__main__":
    main()
