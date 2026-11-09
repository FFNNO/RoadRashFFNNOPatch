#!/usr/bin/env python3
"""High-resolution texture pack upscaler for Road Rash 95.

Workflow:
1. Enable dumping in rr95gpu.ini:
   [TexturePack]
   Dump=1
2. Launch the game and play every track to capture all scenery, track textures,
   and dynamic sprite variations to the texture-dump/ directory.
3. Run this upscaler tool to generate 4x (or 2x / 8x) replacement textures:
   python tools/upscale.py --scale 4
4. Ensure the generated texture-pack/ folder is located next to RoadRash95.exe
   with [TexturePack] Enabled=1 in rr95gpu.ini.
"""

import argparse
import json
import os
import subprocess
import sys
import tempfile
import numpy as np
from PIL import Image


def bleed_colors(rgb: np.ndarray, mask: np.ndarray, iterations: int = 32) -> np.ndarray:
    """Iteratively bleeds the color of opaque texels into adjacent transparent texels.
    
    Prevents dark and colored fringing along color-keyed sprite and texture edges
    when resampling with Lanczos or other reconstruction filters.
    """
    r = rgb[..., 0].astype(np.float32)
    g = rgb[..., 1].astype(np.float32)
    b = rgb[..., 2].astype(np.float32)
    m = mask.copy()

    for _ in range(iterations):
        if np.all(m):
            break
        sum_r = np.zeros_like(r)
        sum_g = np.zeros_like(g)
        sum_b = np.zeros_like(b)
        count = np.zeros_like(r)

        for dy in (-1, 0, 1):
            for dx in (-1, 0, 1):
                if dy == 0 and dx == 0:
                    continue
                rolled_m = np.roll(np.roll(m, dy, axis=0), dx, axis=1)
                if dy == -1:
                    rolled_m[-1, :] = False
                elif dy == 1:
                    rolled_m[0, :] = False
                if dx == -1:
                    rolled_m[:, -1] = False
                elif dx == 1:
                    rolled_m[:, 0] = False

                rolled_r = np.roll(np.roll(r, dy, axis=0), dx, axis=1)
                rolled_g = np.roll(np.roll(g, dy, axis=0), dx, axis=1)
                rolled_b = np.roll(np.roll(b, dy, axis=0), dx, axis=1)

                valid = rolled_m & (~m)
                sum_r += np.where(valid, rolled_r, 0.0)
                sum_g += np.where(valid, rolled_g, 0.0)
                sum_b += np.where(valid, rolled_b, 0.0)
                count += valid.astype(np.float32)

        has_neighbors = count > 0
        safe_count = np.maximum(count, 1.0)
        r = np.where(has_neighbors, sum_r / safe_count, r)
        g = np.where(has_neighbors, sum_g / safe_count, g)
        b = np.where(has_neighbors, sum_b / safe_count, b)
        m = m | has_neighbors

    return np.stack([np.clip(r, 0, 255), np.clip(g, 0, 255), np.clip(b, 0, 255)], axis=-1).astype(np.uint8)


def upscale_image(im: Image.Image, scale: int = 4, color_key: bool = True,
                  external_cmd: str = None) -> Image.Image:
    """Upscales an image by scale factor (2, 4, or 8) preserving alpha edges."""
    orig_w, orig_h = im.size
    target_w, target_h = orig_w * scale, orig_h * scale
    arr = np.array(im)

    if external_cmd:
        with tempfile.TemporaryDirectory() as tmp_dir:
            tmp_in = os.path.join(tmp_dir, "input.png")
            tmp_out = os.path.join(tmp_dir, "output.png")
            im.save(tmp_in, format="PNG")

            cmd = external_cmd.format(input=tmp_in, output=tmp_out, **{"in": tmp_in, "out": tmp_out})
            subprocess.run(cmd, shell=True, check=True)

            out_im = Image.open(tmp_out).convert("RGBA")
            if out_im.size != (target_w, target_h):
                out_im = out_im.resize((target_w, target_h), Image.Resampling.LANCZOS)

            out_arr = np.array(out_im)
            rgb_up = Image.fromarray(out_arr[..., :3], "RGB")
            if not color_key:
                alpha_up = Image.new("L", (target_w, target_h), 255)
            else:
                alpha_raw = out_arr[..., 3]
                alpha_thresh = np.where(alpha_raw >= 128, 255, 0).astype(np.uint8)
                alpha_up = Image.fromarray(alpha_thresh, "L")
            return Image.merge("RGBA", (*rgb_up.split(), alpha_up))

    if not color_key or arr.shape[-1] < 4:
        rgb = arr[..., :3] if arr.shape[-1] >= 3 else np.repeat(arr[..., :1], 3, axis=-1)
        rgb_img = Image.fromarray(rgb, "RGB")
        rgb_up = rgb_img.resize((target_w, target_h), Image.Resampling.LANCZOS)
        alpha_up = Image.new("L", (target_w, target_h), 255)
        return Image.merge("RGBA", (*rgb_up.split(), alpha_up))

    rgb = arr[..., :3]
    alpha = arr[..., 3]
    mask = alpha > 0

    if not np.all(mask) and np.any(mask):
        rgb_bled = bleed_colors(rgb, mask)
    else:
        rgb_bled = rgb

    rgb_img = Image.fromarray(rgb_bled, "RGB")
    rgb_up = rgb_img.resize((target_w, target_h), Image.Resampling.LANCZOS)

    alpha_img = Image.fromarray(alpha, "L")
    alpha_up_raw = np.array(alpha_img.resize((target_w, target_h), Image.Resampling.LANCZOS))
    alpha_thresh = np.where(alpha_up_raw >= 128, 255, 0).astype(np.uint8)
    alpha_up = Image.fromarray(alpha_thresh, "L")

    return Image.merge("RGBA", (*rgb_up.split(), alpha_up))


def run_self_test():
    """Builds synthetic textures and verifies upscale properties."""
    scale = 4

    # 1. Opaque gradient: assert exact size, fully opaque
    grad_1d = np.linspace(20, 220, 16 * 16, dtype=np.uint8).reshape(16, 16)
    grad_rgb = np.stack([grad_1d, grad_1d, grad_1d], axis=-1)
    im_grad = Image.fromarray(grad_rgb, "RGB")
    up_grad = upscale_image(im_grad, scale=scale, color_key=False)
    arr_grad = np.array(up_grad)
    assert arr_grad.shape == (64, 64, 4), f"Expected (64, 64, 4), got {arr_grad.shape}"
    assert (arr_grad[..., 3] == 255).all(), "Opaque gradient must remain fully opaque (alpha 255)"

    # 2. Colour-keyed sprite with a hole: assert exact size, binary alpha, no edge fringing
    hole = np.zeros((16, 16, 4), dtype=np.uint8)
    hole[2:14, 2:14] = [200, 100, 50, 255]
    hole[6:10, 6:10] = [0, 0, 0, 0]
    im_hole = Image.fromarray(hole, "RGBA")
    up_hole = upscale_image(im_hole, scale=scale, color_key=True)
    arr_hole = np.array(up_hole)
    assert arr_hole.shape == (64, 64, 4), f"Expected (64, 64, 4), got {arr_hole.shape}"
    unique_a = set(np.unique(arr_hole[..., 3]))
    assert unique_a.issubset({0, 255}), f"Alpha must be binary {0, 255}, got {unique_a}"

    opaque_hole = arr_hole[arr_hole[..., 3] == 255]
    min_in = np.array([200, 100, 50])
    min_out = opaque_hole[:, :3].min(axis=0)
    assert (min_out >= min_in - 5).all(), f"Dark fringe detected: {min_out} < {min_in}"

    # 3. 2x8 strip: assert exact size, binary alpha, opaque stays opaque
    strip = np.zeros((8, 2, 4), dtype=np.uint8)
    strip[:4, :] = [180, 120, 60, 255]
    im_strip = Image.fromarray(strip, "RGBA")
    up_strip = upscale_image(im_strip, scale=scale, color_key=True)
    arr_strip = np.array(up_strip)
    assert arr_strip.shape == (32, 8, 4), f"Expected (32, 8, 4), got {arr_strip.shape}"
    assert set(np.unique(arr_strip[..., 3])).issubset({0, 255}), "Strip alpha must be binary"

    opaque_strip = arr_strip[arr_strip[..., 3] == 255]
    min_strip_out = opaque_strip[:, :3].min(axis=0)
    assert (min_strip_out >= [180 - 5, 120 - 5, 60 - 5]).all(), f"Strip fringe: {min_strip_out}"

    print("upscale self-test: OK")


def main():
    parser = argparse.ArgumentParser(description="High-resolution texture pack upscaler for Road Rash 95")
    parser.add_argument("--dump", default="texture-dump", help="Input texture dump directory")
    parser.add_argument("--out", default="texture-pack", help="Output texture pack directory")
    parser.add_argument("--scale", type=int, choices=[2, 4, 8], default=4, help="Scale factor (2, 4, or 8)")
    parser.add_argument("--only", help="Comma-separated texture IDs to process")
    parser.add_argument("--limit", type=int, help="Limit number of textures to process")
    parser.add_argument("--force", action="store_true", help="Overwrite existing output files")
    parser.add_argument("--external", help="External upscaler command template with {in} and {out}")
    parser.add_argument("--self-test", action="store_true", help="Run synthetic self-test suite")

    args = parser.parse_args()

    if args.self_test:
        run_self_test()
        return

    os.makedirs(args.out, exist_ok=True)

    manifest_path = os.path.join(args.dump, "manifest.json")
    manifest = {}
    if os.path.exists(manifest_path):
        try:
            with open(manifest_path, "r", encoding="utf-8") as f:
                manifest = json.load(f)
        except Exception as e:
            print(f"Warning: could not read {manifest_path}: {e}", file=sys.stderr)

    selected_ids = None
    if args.only:
        selected_ids = set(part.strip() for part in args.only.split(",") if part.strip())

    # Find candidates from manifest or directly in dump directory
    items = []
    if manifest:
        for tid, meta in manifest.items():
            if selected_ids and tid not in selected_ids:
                continue
            png_name = f"{tid}.png"
            png_path = os.path.join(args.dump, png_name)
            if os.path.exists(png_path):
                color_key = meta.get("color_key", True)
                items.append((tid, png_path, color_key))
    else:
        if os.path.exists(args.dump):
            for fname in os.listdir(args.dump):
                if fname.lower().endswith(".png"):
                    tid = fname[:-4]
                    if selected_ids and tid not in selected_ids:
                        continue
                    png_path = os.path.join(args.dump, fname)
                    items.append((tid, png_path, True))

    if args.limit and args.limit > 0:
        items = items[:args.limit]

    print(f"Upscaling {len(items)} textures (scale={args.scale}x)...")
    processed = 0
    skipped = 0

    for tid, in_png, color_key in items:
        out_png = os.path.join(args.out, f"{tid}.png")
        if os.path.exists(out_png) and not args.force:
            skipped += 1
            continue

        try:
            with Image.open(in_png) as im:
                im = im.convert("RGBA")
                up = upscale_image(im, scale=args.scale, color_key=color_key, external_cmd=args.external)
                up.save(out_png, format="PNG")
                processed += 1
                print(f"  [{processed}/{len(items)}] {tid}: {im.size[0]}x{im.size[1]} -> {up.size[0]}x{up.size[1]}")
        except Exception as e:
            print(f"Error upscaling {tid}: {e}", file=sys.stderr)

    print(f"Done: {processed} upscaled, {skipped} skipped.")


if __name__ == "__main__":
    main()
