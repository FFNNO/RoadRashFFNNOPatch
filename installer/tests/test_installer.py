"""Silent install/uninstall tests for the Road Rash 95 FFNNO Patch setup.

Run with:  python -I installer/tests/test_installer.py [path\\to\\RoadRashFFNNOPatch-Setup-x.y.z.exe]

Without an argument the newest setup exe in installer/Output/ is used. Every case installs
into a temporary fake game folder under %TEMP% (never the real Steam folder), runs the
uninstaller, waits for it to finish and removes the temporary folders.
"""

import glob
import os
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
OUTPUT_DIR = os.path.normpath(os.path.join(HERE, "..", "Output"))

DLL = "ddraw.dll"
INI = "rr95gpu.ini"
BACKUP_SUFFIX = ".pre-ffnno"
PACK_NAMES = ["64x256_0a1b2c3d.png", "128x128_deadbeef.png"]
NON_PACK_NAME = "readme.png"  # does not match <w>x<h>_<crc32>.png

failures = []


def check(cond, message):
    print(("PASS " if cond else "FAIL ") + message)
    if not cond:
        failures.append(message)


def find_setup():
    if len(sys.argv) > 1:
        return os.path.abspath(sys.argv[1])
    candidates = glob.glob(os.path.join(OUTPUT_DIR, "RoadRashFFNNOPatch-Setup-*.exe"))
    if not candidates:
        sys.exit("No setup exe in %s; run installer\\build.bat first" % OUTPUT_DIR)
    return max(candidates, key=os.path.getmtime)


def parse_ini(path):
    """Tolerant INI parser: section -> {key: value}, keys lower-cased, ';' comments skipped."""
    data = {}
    section = None
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for raw in f:
            line = raw.strip()
            if not line or line.startswith(";") or line.startswith("#"):
                continue
            if line.startswith("[") and "]" in line:
                section = line[1:line.index("]")].strip().lower()
                data.setdefault(section, {})
                continue
            if "=" in line and section is not None:
                key, value = line.split("=", 1)
                value = value.split(";", 1)[0].strip()
                data[section][key.strip().lower()] = value
    return data


def ini_value(ini, section, key):
    return ini.get(section.lower(), {}).get(key.lower())


def make_game_dir(with_exe=True):
    path = tempfile.mkdtemp(prefix="rrffnno-game-")
    assert "steamapps" not in path.lower(), path
    if with_exe:
        open(os.path.join(path, "RoadRash95.exe"), "wb").close()
    return path


def make_pack_dir():
    path = tempfile.mkdtemp(prefix="rrffnno-pack-")
    for name in PACK_NAMES:
        with open(os.path.join(path, name), "wb") as f:
            f.write(b"\x89PNG dummy " + name.encode())
    with open(os.path.join(path, NON_PACK_NAME), "wb") as f:
        f.write(b"not a pack texture")
    return path


def run_setup(setup, game_dir, extra, log_name):
    log = os.path.join(tempfile.gettempdir(), log_name)
    args = [setup, "/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART", "/CURRENTUSER",
            "/DIR=" + game_dir, "/LOG=" + log] + extra
    proc = subprocess.run(args, timeout=300)
    return proc.returncode, log


def uninstall(game_dir, timeout=120):
    """Run unins*.exe silently and wait until it has removed itself (it re-spawns a child)."""
    unins = sorted(glob.glob(os.path.join(game_dir, "unins*.exe")))
    if not unins:
        return False
    subprocess.run([unins[-1], "/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART"], timeout=timeout)
    deadline = time.time() + timeout
    while time.time() < deadline:
        if not glob.glob(os.path.join(game_dir, "unins*.*")):
            time.sleep(0.5)  # let the post-uninstall restore settle
            return True
        time.sleep(0.25)
    return False


def read_bytes(path):
    with open(path, "rb") as f:
        return f.read()


def cleanup(*paths):
    for p in paths:
        shutil.rmtree(p, ignore_errors=True)


def case_default(setup):
    print("== (a) default tasks")
    game = make_game_dir()
    try:
        code, _ = run_setup(setup, game, [], "rrffnno-a.log")
        check(code == 0, "(a) setup exit code 0 (got %s)" % code)
        check(os.path.isfile(os.path.join(game, DLL)), "(a) ddraw.dll installed")
        check(os.path.isfile(os.path.join(game, INI)), "(a) rr95gpu.ini installed")
        if os.path.isfile(os.path.join(game, INI)):
            ini = parse_ini(os.path.join(game, INI))
            check(ini_value(ini, "World", "GpuWorld") == "1", "(a) GpuWorld=1")
            check(ini_value(ini, "Save", "Enabled") == "1", "(a) Save Enabled=1")
            check(ini_value(ini, "TexturePack", "Dump") == "0", "(a) Dump=0")
        check(not os.path.exists(os.path.join(game, "texture-pack")), "(a) no texture-pack")
        check(uninstall(game), "(a) uninstall finished")
        check(not os.path.exists(os.path.join(game, DLL)), "(a) ddraw.dll removed")
        check(not os.path.exists(os.path.join(game, INI)), "(a) rr95gpu.ini removed")
        check(os.path.isfile(os.path.join(game, "RoadRash95.exe")), "(a) game exe kept")
    finally:
        cleanup(game)


def case_core_only(setup):
    print("== (b) core only")
    game = make_game_dir()
    try:
        code, _ = run_setup(setup, game, ["/MERGETASKS=!gpuworld,!save,!texturepack,!dump"],
                            "rrffnno-b.log")
        check(code == 0, "(b) setup exit code 0 (got %s)" % code)
        if os.path.isfile(os.path.join(game, INI)):
            ini = parse_ini(os.path.join(game, INI))
            check(ini_value(ini, "World", "GpuWorld") == "0", "(b) GpuWorld=0")
            check(ini_value(ini, "Save", "Enabled") == "0", "(b) Save Enabled=0")
            check(ini_value(ini, "TexturePack", "Dump") == "0", "(b) Dump=0")
        else:
            check(False, "(b) rr95gpu.ini installed")
        check(os.path.isfile(os.path.join(game, DLL)), "(b) ddraw.dll installed")
        check(uninstall(game), "(b) uninstall finished")
    finally:
        cleanup(game)


def case_pack_and_dump(setup):
    print("== (c) texture pack + dump")
    game = make_game_dir()
    pack = make_pack_dir()
    try:
        code, _ = run_setup(setup, game,
                            ["/TASKS=gpuworld,save,texturepack,dump", "/PACKDIR=" + pack],
                            "rrffnno-c.log")
        check(code == 0, "(c) setup exit code 0 (got %s)" % code)
        dest = os.path.join(game, "texture-pack")
        for name in PACK_NAMES:
            ok = os.path.isfile(os.path.join(dest, name)) and \
                read_bytes(os.path.join(dest, name)) == read_bytes(os.path.join(pack, name))
            check(ok, "(c) pack file copied: " + name)
        check(not os.path.exists(os.path.join(dest, NON_PACK_NAME)), "(c) non-matching file not copied")
        if os.path.isfile(os.path.join(game, INI)):
            ini = parse_ini(os.path.join(game, INI))
            check(ini_value(ini, "TexturePack", "Enabled") == "1", "(c) TexturePack Enabled=1")
            check(ini_value(ini, "TexturePack", "MaxMiB") == "1536", "(c) MaxMiB=1536")
            check(ini_value(ini, "TexturePack", "Dump") == "1", "(c) Dump=1")
        else:
            check(False, "(c) rr95gpu.ini installed")
        check(uninstall(game), "(c) uninstall finished")
        check(not os.path.exists(dest), "(c) texture-pack removed")
        check(os.path.isfile(os.path.join(pack, PACK_NAMES[0])), "(c) source pack untouched")
    finally:
        cleanup(game, pack)


def case_backup_restore(setup):
    print("== (d) backup and restore of existing files")
    game = make_game_dir()
    old_dll = b"MZ original third-party ddraw\x00\x01\x02"
    old_ini = b"[General]\r\nEnabled=0\r\n; user settings\r\n"
    try:
        with open(os.path.join(game, DLL), "wb") as f:
            f.write(old_dll)
        with open(os.path.join(game, INI), "wb") as f:
            f.write(old_ini)
        code, _ = run_setup(setup, game, [], "rrffnno-d.log")
        check(code == 0, "(d) setup exit code 0 (got %s)" % code)
        bdll = os.path.join(game, DLL + BACKUP_SUFFIX)
        bini = os.path.join(game, INI + BACKUP_SUFFIX)
        check(os.path.isfile(bdll) and read_bytes(bdll) == old_dll, "(d) ddraw.dll backed up")
        check(os.path.isfile(bini) and read_bytes(bini) == old_ini, "(d) rr95gpu.ini backed up")
        check(os.path.isfile(os.path.join(game, DLL)) and read_bytes(os.path.join(game, DLL)) != old_dll,
              "(d) new ddraw.dll installed")
        # A reinstall over our own install must not replace the user's backup.
        code, _ = run_setup(setup, game, [], "rrffnno-d2.log")
        check(code == 0, "(d) reinstall exit code 0 (got %s)" % code)
        check(os.path.isfile(bdll) and read_bytes(bdll) == old_dll, "(d) backup kept on reinstall")
        check(uninstall(game), "(d) uninstall finished")
        check(os.path.isfile(os.path.join(game, DLL)) and read_bytes(os.path.join(game, DLL)) == old_dll,
              "(d) ddraw.dll restored byte-identical")
        check(os.path.isfile(os.path.join(game, INI)) and read_bytes(os.path.join(game, INI)) == old_ini,
              "(d) rr95gpu.ini restored byte-identical")
        check(not os.path.exists(bdll) and not os.path.exists(bini), "(d) backups consumed")
    finally:
        cleanup(game)


def case_uninstall_keeps_user_data(setup):
    print("== (e) uninstall keeps user data")
    game = make_game_dir()
    pack = make_pack_dir()
    try:
        dump = os.path.join(game, "texture-dump")
        os.makedirs(dump)
        with open(os.path.join(dump, "manifest.json"), "w") as f:
            f.write("{}")
        save = os.path.join(game, "Partida 1.RRS")
        with open(save, "wb") as f:
            f.write(b"save data")
        other = os.path.join(game, "notes.txt")
        with open(other, "w") as f:
            f.write("unrelated")
        code, _ = run_setup(setup, game, ["/TASKS=gpuworld,save,texturepack", "/PACKDIR=" + pack],
                            "rrffnno-e.log")
        check(code == 0, "(e) setup exit code 0 (got %s)" % code)
        with open(os.path.join(game, "rr95gpu.log"), "w") as f:
            f.write("log written by the game")
        check(os.path.isdir(os.path.join(game, "texture-pack")), "(e) texture-pack created")
        check(uninstall(game), "(e) uninstall finished")
        check(os.path.isfile(os.path.join(dump, "manifest.json")), "(e) texture-dump kept")
        check(os.path.isfile(save) and read_bytes(save) == b"save data", "(e) Partida 1.RRS kept")
        check(os.path.isfile(other), "(e) unrelated file kept")
        check(not os.path.exists(os.path.join(game, "texture-pack")), "(e) texture-pack removed")
        check(not os.path.exists(os.path.join(game, "rr95gpu.log")), "(e) rr95gpu.log removed")
    finally:
        cleanup(game, pack)


def case_existing_pack_backup(setup):
    print("== (g) existing texture-pack is moved aside and restored")
    game = make_game_dir()
    pack = make_pack_dir()
    try:
        old = os.path.join(game, "texture-pack")
        os.makedirs(old)
        with open(os.path.join(old, "32x32_00000000.png"), "wb") as f:
            f.write(b"user pack")
        code, _ = run_setup(setup, game, ["/TASKS=texturepack", "/PACKDIR=" + pack], "rrffnno-g.log")
        check(code == 0, "(g) setup exit code 0 (got %s)" % code)
        backup = old + BACKUP_SUFFIX
        check(os.path.isfile(os.path.join(backup, "32x32_00000000.png")), "(g) old pack moved to backup")
        check(sorted(os.listdir(old)) == sorted(PACK_NAMES), "(g) new pack holds only the copied files")
        check(uninstall(game), "(g) uninstall finished")
        check(os.path.isfile(os.path.join(old, "32x32_00000000.png")) and not os.path.exists(backup),
              "(g) old pack restored")
        check(sorted(os.listdir(old)) == ["32x32_00000000.png"], "(g) restored pack unchanged")
    finally:
        cleanup(game, pack)


def case_no_game_exe(setup):
    print("== (f) folder without RoadRash95.exe")
    game = make_game_dir(with_exe=False)
    try:
        code, _ = run_setup(setup, game, [], "rrffnno-f.log")
        check(code != 0, "(f) setup fails with non-zero exit (got %s)" % code)
        check(not os.path.exists(os.path.join(game, DLL)), "(f) nothing installed")
        if glob.glob(os.path.join(game, "unins*.exe")):
            uninstall(game)
    finally:
        cleanup(game)


def main():
    setup = find_setup()
    print("Setup: " + setup)
    for case in (case_default, case_core_only, case_pack_and_dump, case_backup_restore,
                 case_uninstall_keeps_user_data, case_existing_pack_backup, case_no_game_exe):
        case(setup)
    if failures:
        print("FAILED (%d failure(s))" % len(failures))
        return 1
    print("OK (0 failure(s))")
    return 0


if __name__ == "__main__":
    sys.exit(main())
