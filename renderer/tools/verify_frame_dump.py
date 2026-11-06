"""Verify a captured Road Rash 95 frame dump (frame_<n>.json + frame_<n>.png).

Loads the JSON draw-list and the 8-bit indexed PNG dump (using only the Python
standard library so it runs under `py -I`), rasterizes every non-culled quad
clipped to its per-quad clip_rect, and checks pixel coverage across the 3D
world region vs non-rasterizer regions (horizon scrollFUN_00469770 and
dashboard FUN_00469530).

Usage:
  py -I renderer/tools/verify_frame_dump.py frame_1.json [frame_1.png]
  py -I renderer/tools/verify_frame_dump.py --self-test
"""
import argparse
import json
import pathlib
import struct
import sys
import zlib


PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"


def read_indexed_png(path: pathlib.Path):
    data = path.read_bytes()
    if not data.startswith(PNG_SIGNATURE):
        raise ValueError(f"{path}: invalid PNG signature")

    pos = len(PNG_SIGNATURE)
    width = height = bit_depth = color_type = None
    plte = None
    idat_chunks = []

    while pos < len(data):
        length = struct.unpack_from(">I", data, pos)[0]
        ctype = data[pos + 4 : pos + 8]
        chunk_data = data[pos + 8 : pos + 8 + length]
        crc_expected = struct.unpack_from(">I", data, pos + 8 + length)[0]
        crc_actual = zlib.crc32(data[pos + 4 : pos + 8 + length]) & 0xFFFFFFFF
        if crc_actual != crc_expected:
            raise ValueError(f"{path}: CRC mismatch in chunk {ctype!r}")
        pos += 12 + length

        if ctype == b"IHDR":
            width, height, bit_depth, color_type, comp, filt, interlace = struct.unpack(
                ">IIBBBBB", chunk_data
            )
            if bit_depth != 8 or color_type != 3 or comp != 0 or filt != 0 or interlace != 0:
                raise ValueError(
                    f"{path}: expected 8-bit indexed PNG (color_type=3), got depth={bit_depth} type={color_type}"
                )
        elif ctype == b"PLTE":
            plte = chunk_data
        elif ctype == b"IDAT":
            idat_chunks.append(chunk_data)
        elif ctype == b"IEND":
            break

    if width is None or height is None or plte is None or len(plte) != 768:
        raise ValueError(f"{path}: missing IHDR or 256-entry PLTE")

    raw = zlib.decompress(b"".join(idat_chunks))
    stride = 1 + width
    if len(raw) != height * stride:
        raise ValueError(f"{path}: decompressed size {len(raw)} != {height * stride}")

    rows = []
    for y in range(height):
        row_start = y * stride
        if raw[row_start] != 0:
            raise ValueError(f"{path}: unsupported row filter {raw[row_start]} on row {y}")
        rows.append(raw[row_start + 1 : row_start + stride])

    return width, height, plte, rows


def rasterize_quad_scanlines(mask, width, height, verts, clip_rect):
    c_xmin, c_ymin, c_xmax, c_ymax = clip_rect
    c_xmin = max(0, c_xmin)
    c_ymin = max(0, c_ymin)
    c_xmax = min(width - 1, c_xmax)
    c_ymax = min(height - 1, c_ymax)
    if c_xmin > c_xmax or c_ymin > c_ymax:
        return 0

    ys = [v["y"] for v in verts]
    y_min = max(c_ymin, min(ys))
    y_max = min(c_ymax, max(ys))
    if y_min >= y_max:
        return 0

    edges = []
    for i in range(4):
        x0, y0 = verts[i]["x"], verts[i]["y"]
        x1, y1 = verts[(i + 1) & 3]["x"], verts[(i + 1) & 3]
        y1 = verts[(i + 1) & 3]["y"]
        if y0 == y1:
            continue
        if y0 > y1:
            x0, y0, x1, y1 = x1, y1, x0, y0
        edges.append((x0, y0, x1, y1))

    if not edges:
        return 0

    pixels_written = 0
    for y in range(y_min, y_max + 1):
        xs = []
        for x0, y0, x1, y1 in edges:
            if y0 <= y <= y1:
                x = x0 + (x1 - x0) * (y - y0) / (y1 - y0)
                xs.append(x)
        if len(xs) < 2:
            continue
        span_l = max(c_xmin, int(min(xs)))
        span_r = min(c_xmax, int(max(xs)))
        if span_l <= span_r:
            row = mask[y]
            for x in range(span_l, span_r + 1):
                if row[x] == 0:
                    row[x] = 1
                    pixels_written += 1
    return pixels_written


def verify_dump(json_path: pathlib.Path, png_path: pathlib.Path, min_world_coverage: float = 0.70) -> int:
    meta = json.loads(json_path.read_text(encoding="utf-8"))
    width = int(meta["width"])
    height = int(meta["height"])
    quads = meta["quads"]

    png_w, png_h, _plte, _rows = read_indexed_png(png_path)
    if (png_w, png_h) != (width, height):
        print(f"FAIL PNG dimensions {png_w}x{png_h} != JSON dimensions {width}x{height}")
        return 1

    mask = [[0] * width for _ in range(height)]
    textured_count = 0
    flat_count = 0
    culled_count = 0
    world_ymin = height
    world_ymax = -1

    for q in quads:
        if q.get("kind") == "flat":
            flat_count += 1
        else:
            textured_count += 1
        if q.get("culled", False):
            culled_count += 1
            continue

        verts = q["vertices"]
        clip_rect = q["clip_rect"]
        ys = [v["y"] for v in verts]
        if min(ys) < max(ys):
            q_ymin = max(0, clip_rect[1], min(ys))
            q_ymax = min(height - 1, clip_rect[3], max(ys))
            if q_ymin <= q_ymax:
                world_ymin = min(world_ymin, q_ymin)
                world_ymax = max(world_ymax, q_ymax)

        rasterize_quad_scanlines(mask, width, height, verts, clip_rect)

    if world_ymax < world_ymin:
        world_ymin, world_ymax = 0, height - 1

    world_total = (world_ymax - world_ymin + 1) * width
    world_covered = sum(sum(mask[y]) for y in range(world_ymin, world_ymax + 1))
    coverage = (world_covered / world_total) if world_total > 0 else 0.0

    print(
        f"Frame {meta['frame_number']}: {width}x{height} "
        f"shift={meta['snap_shift']} mask={meta['snap_mask']} "
        f"quads={len(quads)} (textured={textured_count}, flat={flat_count}, culled={culled_count}, "
        f"overflow={meta.get('overflow_count', 0)})"
    )
    print(
        f"Regions: horizon=[0..{max(0, world_ymin - 1)}], "
        f"3d_world=[{world_ymin}..{world_ymax}], "
        f"dashboard=[{min(height, world_ymax + 1)}..{height - 1}]"
    )
    print(
        f"3D world coverage: {world_covered}/{world_total} pixels ({coverage * 100.0:.1f}%, "
        f"threshold {min_world_coverage * 100.0:.1f}%)"
    )

    ok = coverage >= min_world_coverage and meta.get("overflow_count", 0) == 0
    print("OK" if ok else "FAIL")
    return 0 if ok else 1


def run_self_test() -> int:
    import tempfile

    width, height = 640, 480
    with tempfile.TemporaryDirectory() as tmp:
        tmp_dir = pathlib.Path(tmp)
        json_path = tmp_dir / "frame_1.json"
        png_path = tmp_dir / "frame_1.png"

        # Build synthetic 8-bit indexed PNG matching capture.cpp's stored-block PNG format.
        raw_rows = bytearray()
        for y in range(height):
            raw_rows.append(0)
            raw_rows.extend([y & 0xFF] * width)
        compressed = zlib.compress(bytes(raw_rows), level=0)

        def make_chunk(ctype: bytes, payload: bytes) -> bytes:
            crc = zlib.crc32(ctype + payload) & 0xFFFFFFFF
            return struct.pack(">I", len(payload)) + ctype + payload + struct.pack(">I", crc)

        ihdr = struct.pack(">IIBBBBB", width, height, 8, 3, 0, 0, 0)
        plte = bytes([i for i in range(256) for _ in range(3)])
        png_bytes = (
            PNG_SIGNATURE
            + make_chunk(b"IHDR", ihdr)
            + make_chunk(b"PLTE", plte)
            + make_chunk(b"IDAT", compressed)
            + make_chunk(b"IEND", b"")
        )
        png_path.write_bytes(png_bytes)

        sample_json = {
            "frame_number": 1,
            "width": width,
            "height": height,
            "snap_shift": 15,
            "snap_mask": "0xFFFFFFFE",
            "quad_count": 2,
            "overflow_count": 0,
            "non_rasterizer_writers": [],
            "quads": [
                {
                    "draw_order": 0,
                    "batch": 1,
                    "kind": "flat",
                    "culled": False,
                    "pdat": "0x005AA9D4",
                    "pdat_magic": "0x00000010",
                    "texture_id": "flat_12",
                    "width": 0,
                    "height": 0,
                    "mip_levels": 0,
                    "chosen_mip": -1,
                    "pdat_flags": "0x00000010",
                    "ccb_flags": "0x00000000",
                    "flat_color": 12,
                    "sprite_clut": 0,
                    "sprite_index": -1,
                    "u_flip": 0,
                    "v_flip": 0,
                    "clip_rect": [0, 120, 639, 359],
                    "uv_box": [0, 0, 0, 0],
                    "corners_16_16": [0, 7864320, 41943040, 7864320, 41943040, 15728640, 0, 15728640],
                    "snapped_pre_fix": [0, 240, 640, 240, 640, 480, 0, 480],
                    "vertices": [
                        {"x": 0, "y": 120, "u": 0, "v": 0},
                        {"x": 639, "y": 120, "u": 0, "v": 0},
                        {"x": 639, "y": 240, "u": 0, "v": 0},
                        {"x": 0, "y": 240, "u": 0, "v": 0},
                    ],
                },
                {
                    "draw_order": 1,
                    "batch": 1,
                    "kind": "textured",
                    "culled": False,
                    "pdat": "0x01234560",
                    "pdat_magic": "0x00BC614E",
                    "texture_id": "128x64_deadbeef",
                    "width": 128,
                    "height": 64,
                    "mip_levels": 4,
                    "chosen_mip": 0,
                    "pdat_flags": "0x00000040",
                    "ccb_flags": "0x00000000",
                    "flat_color": 0,
                    "sprite_clut": 0,
                    "sprite_index": 0,
                    "u_flip": 0,
                    "v_flip": 0,
                    "clip_rect": [0, 120, 639, 359],
                    "uv_box": [0, 0, 128, 64],
                    "corners_16_16": [0, 7864320, 41943040, 7864320, 41943040, 15728640, 0, 15728640],
                    "snapped_pre_fix": [0, 240, 640, 240, 640, 480, 0, 480],
                    "vertices": [
                        {"x": 0, "y": 240, "u": 1, "v": 1},
                        {"x": 639, "y": 240, "u": 127, "v": 1},
                        {"x": 639, "y": 359, "u": 127, "v": 63},
                        {"x": 0, "y": 359, "u": 1, "v": 63},
                    ],
                },
            ],
        }
        json_path.write_text(json.dumps(sample_json, indent=2), encoding="utf-8")
        return verify_dump(json_path, png_path, min_world_coverage=0.95)


def main(argv):
    parser = argparse.ArgumentParser(description="Verify Road Rash 95 frame dump JSON + PNG")
    parser.add_argument("json_file", nargs="?", help="Path to frame_<n>.json")
    parser.add_argument("png_file", nargs="?", help="Path to frame_<n>.png (defaults to matching .png)")
    parser.add_argument("--min-coverage", type=float, default=0.70, help="Minimum 3D world coverage ratio")
    parser.add_argument("--self-test", action="store_true", help="Run built-in verification self-test")
    args = parser.parse_args(argv)

    if args.self_test:
        return run_self_test()

    if not args.json_file:
        parser.error("json_file is required unless --self-test is specified")

    json_path = pathlib.Path(args.json_file)
    png_path = pathlib.Path(args.png_file) if args.png_file else json_path.with_suffix(".png")
    return verify_dump(json_path, png_path, min_world_coverage=args.min_coverage)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
