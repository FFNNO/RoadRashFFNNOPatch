"""Byte patcher for Road Rash 95 (Steam 30th Anniversary Edition).

Usage:
    py -I patch.py build    # writes ../patched/RoadRash95.exe from ../original/RoadRash95.exe
    py -I patch.py install  # backs up the Steam exe to RoadRash95.exe.orig, copies the patched exe in
    py -I patch.py restore  # puts RoadRash95.exe.orig back

Every patch checks the original bytes first, so a different exe build is refused instead of corrupted.
"""
import hashlib
import os
import shutil
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ORIGINAL = os.path.join(ROOT, "original", "RoadRash95.exe")
PATCHED = os.path.join(ROOT, "patched", "RoadRash95.exe")
GAME_DIR = r"C:\Program Files (x86)\Steam\steamapps\common\RoadRash"
GAME_EXE = os.path.join(GAME_DIR, "RoadRash95.exe")
BACKUP_EXE = GAME_EXE + ".orig"

ORIGINAL_SHA256 = "9e40e25737149ab095061b7154797a35495faea97644fafc5a1e616f1400c968"

# (name, file offset, original bytes, patched bytes, description)
PATCHES = [
    (
        "hires_snap_mask",
        0x21F94,
        bytes([0xFE]),
        bytes([0xFF]),
        "FUN_00422b40: HiRes corner mask 0xfffffffe -> 0xffffffff (stop snapping sprites to even pixels)",
    ),
    (
        "hires_snap_shift",
        0x21F9A,
        bytes([0x11]),
        bytes([0x10]),
        "FUN_00422b40: lea ecx,[eax+0x11] -> [eax+0x10], keeps shift at 15 after the mask change",
    ),
    (
        "halftone_race",
        0x6D416,
        bytes([0x03]),
        bytes([0x04]),
        "SetStretchBltMode COLORONCOLOR -> HALFTONE for the race presenter (filtered upscale)",
    ),
    (
        "halftone_menu",
        0x6DCB3,
        bytes([0x03]),
        bytes([0x04]),
        "SetStretchBltMode COLORONCOLOR -> HALFTONE for the menu presenter (filtered upscale)",
    ),
]


def sha256(path):
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


def build(selected=None):
    data = bytearray(open(ORIGINAL, "rb").read())
    digest = hashlib.sha256(data).hexdigest()
    if digest != ORIGINAL_SHA256:
        sys.exit(f"Refusing: unexpected exe hash {digest}")
    for name, off, old, new, desc in PATCHES:
        if selected and name not in selected:
            continue
        current = bytes(data[off:off + len(old)])
        if current != old:
            sys.exit(f"Refusing: {name} expected {old.hex()} at {off:#x}, found {current.hex()}")
        data[off:off + len(new)] = new
        print(f"applied {name}: {desc}")
    os.makedirs(os.path.dirname(PATCHED), exist_ok=True)
    with open(PATCHED, "wb") as f:
        f.write(data)
    print(f"wrote {PATCHED} sha256={hashlib.sha256(data).hexdigest()}")


def install():
    if not os.path.exists(PATCHED):
        sys.exit("Run build first.")
    if not os.path.exists(BACKUP_EXE):
        if sha256(GAME_EXE) != ORIGINAL_SHA256:
            sys.exit("Refusing: Steam exe is not the known original and no backup exists.")
        shutil.copy2(GAME_EXE, BACKUP_EXE)
        print(f"backup: {BACKUP_EXE}")
    shutil.copy2(PATCHED, GAME_EXE)
    print(f"installed patched exe into {GAME_EXE}")


def restore():
    if not os.path.exists(BACKUP_EXE):
        sys.exit("No backup found; use Steam 'Verify integrity of game files' instead.")
    shutil.copy2(BACKUP_EXE, GAME_EXE)
    print(f"restored original exe into {GAME_EXE}")


if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else "build"
    if cmd == "build":
        build(set(sys.argv[2:]) or None)
    elif cmd == "install":
        install()
    elif cmd == "restore":
        restore()
    else:
        sys.exit(__doc__)
