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
import re
import subprocess
import sys
import tempfile
import numpy as np
from PIL import Image, ImageFilter


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


_PLACEHOLDER_RE = re.compile(r"""(["']?)\{(in|input|out|output)\}(["']?)""")


def build_external_command(template: str, in_path: str, out_path: str) -> str:
    """Fills {in}/{input} and {out}/{output} in an --external command template.

    Paths are wrapped in double quotes, unless the template already wraps the placeholder
    in matching quotes. Other braces in the template are left untouched.
    """
    def substitute(match):
        path = in_path if match.group(2) in ("in", "input") else out_path
        left, right = match.group(1), match.group(3)
        if left and left == right:
            return f"{left}{path}{right}"
        return f'{left}"{path}"{right}'

    return _PLACEHOLDER_RE.sub(substitute, template)


def resolve_color_key(value, im: Image.Image) -> bool:
    """Returns the manifest color_key, or detects it from the PNG when it is missing.

    Entries the dumper recovered from a disk scan have no color_key. Such a texture is
    colour-keyed when any texel has alpha 0.
    """
    if value is not None:
        return bool(value)
    if "A" not in im.getbands():
        return False
    alpha = np.array(im.getchannel("A"))
    return bool((alpha == 0).any())


def collect_items(dump_dir: str, manifest: dict, selected_ids):
    """Lists (id, png_path, color_key) to upscale. color_key is None when it is unknown."""
    items = []
    if manifest:
        for tid, meta in manifest.items():
            if selected_ids and tid not in selected_ids:
                continue
            png_path = os.path.join(dump_dir, f"{tid}.png")
            if os.path.exists(png_path):
                items.append((tid, png_path, meta.get("color_key")))
    elif os.path.exists(dump_dir):
        for fname in sorted(os.listdir(dump_dir)):
            if fname.lower().endswith(".png"):
                tid = fname[:-4]
                if selected_ids and tid not in selected_ids:
                    continue
                items.append((tid, os.path.join(dump_dir, fname), None))
    return items


def edge_band_px(scale: int) -> int:
    """Width of the recoloured outer band: about 1.5 source texels at the output size."""
    return max(1, round(1.5 * scale))


def recolor_edge_band(rgb: np.ndarray, mask: np.ndarray, band_px: int) -> np.ndarray:
    """Replaces the colours of the outer band_px texels of mask with colours bled from its core.

    Photo cut-out sprites carry speckled halo texels along their edges; after upscaling they read
    as stray pixels. The silhouette is kept. Parts thinner than the band (poles, wires) have no
    core and keep their own colours.
    """
    if band_px <= 0 or not mask.any():
        return rgb
    mask_img = Image.fromarray(np.where(mask, 255, 0).astype(np.uint8), "L")
    core = np.array(mask_img.filter(ImageFilter.MinFilter(2 * band_px + 1))) > 0
    core &= mask
    if not core.any():
        return rgb
    bled = bleed_colors(rgb, core, iterations=band_px + 2)
    band = mask & ~core
    # Band texels the bleed did not reach (far from any core) keep their colour.
    reached = np.array(Image.fromarray(np.where(core, 255, 0).astype(np.uint8), "L")
                       .filter(ImageFilter.MaxFilter(2 * (band_px + 2) + 1))) > 0
    return np.where((band & reached)[..., None], bled, rgb).astype(np.uint8)


def run_external(img: Image.Image, external_cmd: str, target_w: int, target_h: int) -> Image.Image:
    """Runs the --external tool on an RGB image and returns its RGB output at the target size."""
    with tempfile.TemporaryDirectory() as tmp_dir:
        tmp_in = os.path.join(tmp_dir, "input.png")
        tmp_out = os.path.join(tmp_dir, "output.png")
        img.save(tmp_in, format="PNG")
        subprocess.run(build_external_command(external_cmd, tmp_in, tmp_out), shell=True, check=True)
        out_im = Image.open(tmp_out).convert("RGB")
        if out_im.size != (target_w, target_h):
            out_im = out_im.resize((target_w, target_h), Image.Resampling.LANCZOS)
        out_im.load()
        return out_im


def prepare_external(arr: np.ndarray, color_key: bool) -> dict:
    """Builds the --external tool inputs: the bled colours and, for colour-keyed sprites, the mask."""
    rgb = arr[..., :3] if arr.shape[-1] >= 3 else np.repeat(arr[..., :1], 3, axis=-1)
    keyed = bool(color_key) and arr.shape[-1] >= 4
    prep = {"arr": arr, "keyed": keyed, "mask": None, "rgb": None}
    if keyed:
        mask = arr[..., 3] > 0
        rgb = bleed_colors(rgb, mask) if (not np.all(mask) and np.any(mask)) else rgb
        # The silhouette is upscaled by the same tool as the colours, so both follow the same
        # smoothed outline. A Lanczos-resized alpha keeps the original staircase while the colour
        # edge moves, which leaves a band of bled and speckled edge texels inside the sprite.
        prep["mask"] = Image.fromarray(np.where(mask, 255, 0).astype(np.uint8), "L").convert("RGB")
    prep["rgb"] = Image.fromarray(np.ascontiguousarray(rgb), "RGB")
    return prep


def finish_external(prep: dict, out_im: Image.Image, mask_out: Image.Image, scale: int) -> Image.Image:
    """Combines the --external outputs for one texture into the final RGBA replacement."""
    arr = prep["arr"]
    target_w, target_h = arr.shape[1] * scale, arr.shape[0] * scale
    out_im = out_im.convert("RGB")
    if out_im.size != (target_w, target_h):
        out_im = out_im.resize((target_w, target_h), Image.Resampling.LANCZOS)
    if not prep["keyed"]:
        return Image.merge("RGBA", (*out_im.split(), Image.new("L", (target_w, target_h), 255)))
    mask_up = np.zeros((target_h, target_w), dtype=bool)
    if mask_out is not None:
        mask_out = mask_out.convert("L")
        if mask_out.size != (target_w, target_h):
            mask_out = mask_out.resize((target_w, target_h), Image.Resampling.LANCZOS)
        mask_up = np.array(mask_out) >= 128
    if not mask_up.any():
        # The tool erased a tiny sprite (or gave no mask): keep the plain thresholded alpha.
        alpha_img = Image.fromarray(arr[..., 3], "L")
        mask_up = np.array(alpha_img.resize((target_w, target_h), Image.Resampling.LANCZOS)) >= 128
    rgb_up = recolor_edge_band(np.array(out_im), mask_up, edge_band_px(scale))
    alpha_up = Image.fromarray(np.where(mask_up, 255, 0).astype(np.uint8), "L")
    return Image.merge("RGBA", (*Image.fromarray(rgb_up, "RGB").split(), alpha_up))


def batch_chunks_by_size(pending, max_items: int):
    """Splits [(tid, png_path, color_key)] into chunks whose textures share one original size.

    realesrgan-ncnn-vulkan v0.2.5 in directory mode returned striped garbage for about 9% of the
    textures when one call mixed many image sizes (observed on 1375 Road Rash 95 textures);
    same-size calls matched the per-image results. The size comes from the texture ID. Yields
    (index of the first item, chunk).
    """
    groups = {}
    for item in pending:
        w, h = parse_id_size(item[0])
        groups.setdefault((w, h), []).append(item)
    index = 0
    for size in sorted(groups):
        items = groups[size]
        for start in range(0, len(items), max_items):
            chunk = items[start:start + max_items]
            yield index, chunk
            index += len(chunk)


def parse_id_size(tid: str):
    """Returns (w, h) from a texture ID "<w>x<h>_<crc>", or (0, 0) when it does not parse."""
    m = re.match(r"^(\d+)x(\d+)_", tid)
    return (int(m.group(1)), int(m.group(2))) if m else (0, 0)


def upscale_batch_external(jobs, external_cmd: str, scale: int):
    """Upscales [(tid, im, color_key)] with one --external call on directories.

    {in} and {out} are directories here. Returns {tid: RGBA image}; a texture whose output is
    missing is left out.
    """
    results = {}
    with tempfile.TemporaryDirectory() as tmp_dir:
        in_dir = os.path.join(tmp_dir, "in")
        out_dir = os.path.join(tmp_dir, "out")
        os.makedirs(in_dir)
        os.makedirs(out_dir)
        preps = {}
        for tid, im, color_key in jobs:
            prep = prepare_external(np.array(im), color_key)
            prep["rgb"].save(os.path.join(in_dir, f"{tid}.png"))
            if prep["mask"] is not None:
                prep["mask"].save(os.path.join(in_dir, f"{tid}__mask.png"))
            preps[tid] = prep
        subprocess.run(build_external_command(external_cmd, in_dir, out_dir), shell=True, check=True)
        for tid, prep in preps.items():
            out_path = os.path.join(out_dir, f"{tid}.png")
            if not os.path.exists(out_path):
                print(f"Error upscaling {tid}: no output from the external tool", file=sys.stderr)
                continue
            mask_path = os.path.join(out_dir, f"{tid}__mask.png")
            with Image.open(out_path) as out_im:
                mask_out = Image.open(mask_path) if os.path.exists(mask_path) else None
                try:
                    results[tid] = finish_external(prep, out_im, mask_out, scale)
                finally:
                    if mask_out is not None:
                        mask_out.close()
    return results


def upscale_image(im: Image.Image, scale: int = 4, color_key: bool = True,
                  external_cmd: str = None) -> Image.Image:
    """Upscales an image by scale factor (2, 4, or 8) preserving alpha edges."""
    orig_w, orig_h = im.size
    target_w, target_h = orig_w * scale, orig_h * scale
    arr = np.array(im)

    if external_cmd:
        prep = prepare_external(arr, color_key)
        out_im = run_external(prep["rgb"], external_cmd, target_w, target_h)
        mask_out = None
        if prep["mask"] is not None:
            mask_out = run_external(prep["mask"], external_cmd, target_w, target_h)
        return finish_external(prep, out_im, mask_out, scale)

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

    # 4. External mock tool: verifies argument replacement, quoting, and binary alpha preservation
    py_exec = sys.executable.replace("\\", "/")
    mock_cmd = f'"{py_exec}" -c "import sys; from PIL import Image; Image.open(sys.argv[1]).resize((64, 64)).save(sys.argv[2])" {{in}} {{out}}'
    up_ext = upscale_image(im_hole, scale=scale, color_key=True, external_cmd=mock_cmd)
    arr_ext = np.array(up_ext)
    assert arr_ext.shape == (64, 64, 4), f"Expected (64, 64, 4), got {arr_ext.shape}"
    assert set(np.unique(arr_ext[..., 3])).issubset({0, 255}), "External alpha must be binary"

    # 5. --external quoting: placeholders the template already quotes are not quoted again
    cmd_plain = build_external_command("tool {in} {out}", "C:/t/in.png", "C:/t/out.png")
    assert cmd_plain == 'tool "C:/t/in.png" "C:/t/out.png"', cmd_plain
    cmd_quoted = build_external_command("tool -i \"{in}\" -o '{out}'", "C:/t/in.png", "C:/t/out.png")
    assert cmd_quoted == "tool -i \"C:/t/in.png\" -o 'C:/t/out.png'", cmd_quoted
    cmd_long = build_external_command("tool --input={input} --output=\"{output}\"", "a b.png", "c.png")
    assert cmd_long == 'tool --input="a b.png" --output="c.png"', cmd_long
    cmd_braces = build_external_command("tool {in} {out} --json {x: 1}", "i.png", "o.png")
    assert cmd_braces == 'tool "i.png" "o.png" --json {x: 1}', cmd_braces

    # 6. Missing color_key (entries recovered from a disk scan) is detected from the PNG alpha
    opaque_rgba = Image.fromarray(np.dstack([grad_rgb, np.full((16, 16), 255, np.uint8)]), "RGBA")
    assert resolve_color_key(None, im_hole) is True, "Unknown color_key with alpha 0 must be colour-keyed"
    assert resolve_color_key(None, opaque_rgba) is False, "Unknown color_key with no alpha 0 must be opaque"
    assert resolve_color_key(None, im_grad) is False, "Unknown color_key on an RGB image must be opaque"
    assert resolve_color_key(False, im_hole) is False, "A known color_key must be kept"
    assert resolve_color_key(True, opaque_rgba) is True, "A known color_key must be kept"

    with tempfile.TemporaryDirectory() as dump_dir:
        im_hole.save(os.path.join(dump_dir, "16x16_0000000a.png"))
        im_hole.save(os.path.join(dump_dir, "16x16_0000000b.png"))
        manifest = {
            "16x16_0000000a": {"width": 16, "height": 16, "color_key": False, "kind": "texture"},
            "16x16_0000000b": {"width": 16, "height": 16, "mip_levels": 1},
        }
        items = {tid: ck for tid, _, ck in collect_items(dump_dir, manifest, None)}
        assert items == {"16x16_0000000a": False, "16x16_0000000b": None}, items
        no_manifest = {tid: ck for tid, _, ck in collect_items(dump_dir, {}, None)}
        assert no_manifest == {"16x16_0000000a": None, "16x16_0000000b": None}, no_manifest

    unknown_up = np.array(upscale_image(im_hole, scale=scale, color_key=resolve_color_key(None, im_hole)))
    assert (unknown_up[..., 3] == 0).any(), "A scanned colour-keyed sprite must keep its holes"

    # 7. Edge band: a speckled outer ring takes the core colour; parts thinner than the band keep theirs
    ring = np.zeros((40, 40, 3), dtype=np.uint8)
    ring_mask = np.zeros((40, 40), dtype=bool)
    ring_mask[5:35, 5:35] = True
    ring[5:35, 5:35] = [200, 100, 50]
    ring[5:7, 5:35] = [20, 40, 220]          # 2-texel speckled top edge
    ring_mask[0:40, 38:40] = True             # 2-texel wide pole, far from the square
    ring[0:40, 38:40] = [90, 90, 90]
    fixed = recolor_edge_band(ring, ring_mask, 3)
    assert (fixed[5:35, 5:35] == [200, 100, 50]).all(), "The edge band must take the core colour"
    assert (fixed[0:40, 38:40] == [90, 90, 90]).all(), "A part thinner than the band must keep its colour"
    assert (fixed[~ring_mask] == 0).all(), "Texels outside the mask must not change"
    assert edge_band_px(4) == 6 and edge_band_px(2) == 3, "Band is 1.5 source texels"

    # 8. External path: the silhouette comes from the tool output, not from a resized alpha
    ext_sq = np.array(upscale_image(im_hole, scale=scale, color_key=True, external_cmd=mock_cmd))
    assert ext_sq[..., 3][ext_sq[..., 3] == 255].size > 0, "External mask pass must keep the sprite"
    assert (ext_sq[..., 3][28:36, 28:36] == 0).all(), "External mask pass must keep the hole"

    # 9. Batch mode: one call on directories gives the same result as per-texture calls
    dir_tool = ("import os, sys; from PIL import Image; i, o = sys.argv[1], sys.argv[2]; "
                "[Image.open(os.path.join(i, f)).resize((Image.open(os.path.join(i, f)).width * 4, "
                "Image.open(os.path.join(i, f)).height * 4)).save(os.path.join(o, f)) for f in os.listdir(i)]")
    with tempfile.TemporaryDirectory() as tool_dir:
        tool_path = os.path.join(tool_dir, "dirtool.py")
        with open(tool_path, "w", encoding="utf-8") as f:
            f.write(dir_tool)
        batch_cmd = f'"{py_exec}" -I "{tool_path.replace(chr(92), "/")}" {{in}} {{out}}'
        single_cmd = mock_cmd
        batch = upscale_batch_external([("a", im_hole, True), ("b", im_grad.convert("RGBA"), False)],
                                       batch_cmd, scale)
        assert set(batch) == {"a", "b"}, f"Batch must return every texture, got {set(batch)}"
        single = np.array(upscale_image(im_hole, scale=scale, color_key=True, external_cmd=single_cmd))
        assert (np.array(batch["a"]) == single).all(), "Batch and single external results must match"
        assert (np.array(batch["b"])[..., 3] == 255).all(), "An opaque texture stays opaque in a batch"

    # 10. Batches never mix original sizes and keep every item
    pend = [("16x16_a", "", True), ("8x4_b", "", True), ("16x16_c", "", True), ("16x16_d", "", True)]
    chunks = list(batch_chunks_by_size(pend, 2))
    assert all(len({parse_id_size(t)[0:2] for t, _, _ in c}) == 1 for _, c in chunks), "Chunks mix sizes"
    assert sorted(t for _, c in chunks for t, _, _ in c) == sorted(t for t, _, _ in pend), "Chunks lose items"
    assert max(len(c) for _, c in chunks) <= 2, "Chunks exceed the batch size"

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
    parser.add_argument("--external-batch", type=int, default=0,
                        help="With --external: {in}/{out} are directories and up to N textures go in one call")
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
    items = collect_items(args.dump, manifest, selected_ids)

    if args.limit and args.limit > 0:
        items = items[:args.limit]

    print(f"Upscaling {len(items)} textures (scale={args.scale}x)...")
    processed = 0
    skipped = 0

    pending = []
    for tid, in_png, color_key in items:
        out_png = os.path.join(args.out, f"{tid}.png")
        if os.path.exists(out_png) and not args.force:
            skipped += 1
            continue
        pending.append((tid, in_png, color_key))

    if args.external and args.external_batch > 0:
        for start, chunk in batch_chunks_by_size(pending, args.external_batch):
            jobs = []
            for tid, in_png, color_key in chunk:
                with Image.open(in_png) as im:
                    im = im.convert("RGBA")
                    jobs.append((tid, im, resolve_color_key(color_key, im)))
            try:
                results = upscale_batch_external(jobs, args.external, args.scale)
            except Exception as e:
                print(f"Error upscaling batch at {start}: {e}", file=sys.stderr)
                continue
            for tid, _, _ in jobs:
                if tid in results:
                    results[tid].save(os.path.join(args.out, f"{tid}.png"), format="PNG")
                    processed += 1
            print(f"  [{processed}/{len(items)}] batch of {len(jobs)} done")
        print(f"Done: {processed} upscaled, {skipped} skipped.")
        return

    for tid, in_png, color_key in pending:
        out_png = os.path.join(args.out, f"{tid}.png")

        try:
            with Image.open(in_png) as im:
                im = im.convert("RGBA")
                color_key = resolve_color_key(color_key, im)
                up = upscale_image(im, scale=args.scale, color_key=color_key, external_cmd=args.external)
                up.save(out_png, format="PNG")
                processed += 1
                print(f"  [{processed}/{len(items)}] {tid}: {im.size[0]}x{im.size[1]} -> {up.size[0]}x{up.size[1]}")
        except Exception as e:
            print(f"Error upscaling {tid}: {e}", file=sys.stderr)

    print(f"Done: {processed} upscaled, {skipped} skipped.")


if __name__ == "__main__":
    main()
