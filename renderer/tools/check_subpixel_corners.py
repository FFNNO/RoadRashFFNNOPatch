"""Offline check of the T6 sub-pixel corner matching on captured frame dumps.

For every non-culled quad of a frame_<n>.json dump this applies the same rule as
ResolveSubPixelVertices in renderer/src/capture.cpp:

- FUN_00422630/FUN_004227f0 store poly vertex i as corner i >> snap_shift & snap_mask,
  FUN_00468d60 reorders those snapped vertices in place, and FUN_004688e0 copies
  them in that order, so the corner of vertex i is the corner whose snapped value
  (snapped_pre_fix) equals vertex i exactly;
- when two corners snap to the same pixel, the permutation with the smallest
  distance between each corner and the centre of its snapped pixel wins (ties: the
  first permutation in lexicographic order);
- quads without an exact match (rebuilt or clipped vertices), or with a matched
  corner more than 1.5 px (+1 px for a 2-px snap mask) from its vertex, fall back to
  the snapped vertices; single-scanline quads are not eligible.

It reports, per dump, the share of eligible quads drawn at their float corners, the
number of fallbacks, and the maximum corner-vs-snapped delta of the float quads.

Usage:
  py -I renderer/tools/check_subpixel_corners.py frame_120.json [...]
  py -I renderer/tools/check_subpixel_corners.py --all out/gemini/B4/frames
  py -I renderer/tools/check_subpixel_corners.py --self-test
Exit code 0 when every dump has at least --min-share float quads (default 0.90;
the Valle Napa start-line dump frame_10863 has 15 rebuilt quads out of 231).
"""
import argparse
import itertools
import json
import pathlib
import sys

MAX_DELTA_PX = 1.5


def to_int16(value):
    value &= 0xFFFF
    return value - 0x10000 if value & 0x8000 else value


def resolve_quad(quad, snap_shift, snap_mask):
    """Returns (eligible, use_corners, permutation, max_delta)."""
    verts = [(v["x"], v["y"]) for v in quad["vertices"]]
    ys = [y for _, y in verts]
    if min(ys) == max(ys) or (snap_shift & 0x1F) == 0:
        return False, False, None, 0.0
    scale = 1.0 / float(1 << (snap_shift & 0x1F))
    c16 = quad["corners_16_16"]
    snapped = quad["snapped_pre_fix"]
    corners = [(c16[2 * c] * scale, c16[2 * c + 1] * scale) for c in range(4)]
    snapped_xy = [(to_int16(snapped[2 * c]), to_int16(snapped[2 * c + 1])) for c in range(4)]

    best = None
    best_cost = 0.0
    for perm in itertools.permutations(range(4)):
        if any(snapped_xy[perm[i]] != verts[i] for i in range(4)):
            continue
        cost = sum(
            (corners[perm[i]][0] - (verts[i][0] + 0.5)) ** 2 + (corners[perm[i]][1] - (verts[i][1] + 0.5)) ** 2
            for i in range(4)
        )
        if best is None or cost < best_cost:
            best, best_cost = perm, cost
    if best is None:
        return True, False, None, 0.0

    max_allowed = MAX_DELTA_PX + (0.0 if (snap_mask & 1) else 1.0)
    max_delta = max(
        max(abs(corners[best[i]][0] - verts[i][0]), abs(corners[best[i]][1] - verts[i][1])) for i in range(4)
    )
    if max_delta > max_allowed:
        return True, False, best, max_delta
    return True, True, best, max_delta


def check_dump(path):
    data = json.loads(pathlib.Path(path).read_text())
    snap_shift = int(data["snap_shift"])
    snap_mask = int(data["snap_mask"], 16)
    stats = {"quads": 0, "eligible": 0, "float": 0, "fallback": 0, "max_delta": 0.0, "fallback_far": 0}
    for quad in data["quads"]:
        if quad["culled"]:
            continue
        stats["quads"] += 1
        eligible, use, _, delta = resolve_quad(quad, snap_shift, snap_mask)
        if not eligible:
            continue
        stats["eligible"] += 1
        if use:
            stats["float"] += 1
            stats["max_delta"] = max(stats["max_delta"], delta)
        else:
            stats["fallback"] += 1
    return stats


def self_test():
    def quad(corners_px, perm, mask=0xFFFFFFFF):
        c16 = [int(v * 32768.0) for v in corners_px]
        snapped = [((c >> 15) & mask) & 0xFFFFFFFF for c in c16]
        verts = [{"x": to_int16(snapped[2 * p]), "y": to_int16(snapped[2 * p + 1]), "u": 0, "v": 0} for p in perm]
        return {"culled": False, "corners_16_16": c16, "snapped_pre_fix": snapped, "vertices": verts}

    corners = [100.25, 50.75, 200.5, 50.125, 200.875, 150.25, 100.0, 150.96875]
    ok = True
    e, u, p, d = resolve_quad(quad(corners, (0, 1, 2, 3)), 15, 0xFFFFFFFF)
    ok &= e and u and p == (0, 1, 2, 3) and d < 1.0
    e, u, p, d = resolve_quad(quad(corners, (1, 0, 3, 2)), 15, 0xFFFFFFFF)
    ok &= e and u and p == (1, 0, 3, 2)
    rebuilt = quad(corners, (0, 1, 2, 3))
    rebuilt["vertices"][2] = {"x": 150, "y": 100, "u": 0, "v": 0}
    e, u, p, d = resolve_quad(rebuilt, 15, 0xFFFFFFFF)
    ok &= e and not u
    far = quad([103.9, 50.0, 200.0, 50.0, 200.0, 150.0, 100.0, 150.0], (0, 1, 2, 3), 0xFFFFFFFC)
    e, u, p, d = resolve_quad(far, 15, 0xFFFFFFFC)
    ok &= e and not u
    flat = quad([100.5, 50.5, 200.5, 50.5, 200.5, 50.5, 100.5, 50.5], (0, 1, 2, 3))
    e, u, p, d = resolve_quad(flat, 15, 0xFFFFFFFF)
    ok &= not e
    print("self-test:", "OK" if ok else "FAILED")
    return 0 if ok else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("dumps", nargs="*", help="frame_<n>.json files")
    parser.add_argument("--all", metavar="DIR", help="check every frame_*.json in DIR")
    parser.add_argument("--min-share", type=float, default=0.90, help="minimum share of eligible quads drawn at float corners per dump")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        return self_test()

    paths = [pathlib.Path(p) for p in args.dumps]
    if args.all:
        paths += sorted(pathlib.Path(args.all).glob("frame_*.json"))
    if not paths:
        parser.error("no dumps given")

    failed = 0
    total = {"eligible": 0, "float": 0, "fallback": 0, "max_delta": 0.0}
    for path in paths:
        st = check_dump(path)
        share = st["float"] / st["eligible"] if st["eligible"] else 1.0
        status = "OK" if share >= args.min_share else "LOW"
        failed += status != "OK"
        print(
            f"{path.name}: quads={st['quads']} eligible={st['eligible']} float={st['float']} "
            f"({share * 100.0:.1f}%) fallback={st['fallback']} max_delta={st['max_delta']:.3f} px {status}"
        )
        for key in ("eligible", "float", "fallback"):
            total[key] += st[key]
        total["max_delta"] = max(total["max_delta"], st["max_delta"])
    share = total["float"] / total["eligible"] if total["eligible"] else 1.0
    print(
        f"Summary: {len(paths) - failed}/{len(paths)} dumps OK; float {total['float']}/{total['eligible']} "
        f"({share * 100.0:.1f}%), fallback {total['fallback']}, max_delta {total['max_delta']:.3f} px"
    )
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
