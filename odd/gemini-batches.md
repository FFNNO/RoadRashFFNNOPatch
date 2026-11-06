# RR95 GPU Renderer: batch plan for an external model

How to use: paste **B0** once at the start of the chat with the external model, then one batch per message, in order. Attach the files listed under each batch's *Attach* section. Return each batch's output to the reviewer (Claude), who applies it on branch `feat/gpu-renderer`, builds, runs the checks and commits. Do not start a batch until the previous one passed review.

The external model cannot see the exe, the Ghidra project or the game. Everything it needs must be attached as files. Decompilation excerpts come from `out/decomp.c` (local only, never committed).

---

## B0: Shared context (paste once)

```
You are helping write a Direct3D 11 renderer injected into an old Windows game through a proxy ddraw.dll.

Game: Road Rash 95 (EA, 1996), Steam "30th Anniversary Edition". RoadRash95.exe is 32-bit x86, image base 0x400000, PE sections .text 0x401000, .rdata 0x503000, .data 0x58e000. .text file offset to VA: VA = off + 0x400C00. No SteamStub DRM. It imports only DirectDrawCreate from DDRAW.dll.

Two exe builds must be supported, identified by SHA-256 of the file:
- original 9e40e25737149ab095061b7154797a35495faea97644fafc5a1e616f1400c968
- patched  40a0d85f7b8ed8c8e9bcb267954a645228d545e0b4a3f86241c6d936dfe88150 (differs only at file offsets 0x21f94, 0x21f9a, 0x6d416, 0x6dcb3)
Any other build: forward DirectDrawCreate only, install no hooks.

Engine facts (from Ghidra):
- Rendering is pure software into a 640x480 8bpp system-memory DirectDraw surface (DAT_005cd4b0). DirectDraw uses DDSCL_NORMAL only; there is no SetDisplayMode and no Flip.
- Draw globals: pitch DAT_006036b8, base pointer DAT_006036bc, clip rect DAT_006036c0..DAT_006036cc.
- Present (race): FUN_00467b80 -> FUN_0046e630 expands 8bpp through the 256-entry palette into a 32-bit DIB (DAT_005ce864, size DAT_005ce85c x DAT_005ce860 = 640x480), then SetStretchBltMode + StretchDIBits into an aspect-fitted rect (DAT_005ce894..DAT_005ce8a0, computed by FUN_0046e0f0). Menus: FUN_0046e470 (RGB565 -> 32-bit) -> FUN_0046ded0.
- 3D viewport: 320x240 at .data 0x591C04/0x591C08, shifted left by the HiRes flag DAT_005a292a (HiRes renders at 640x480).
- Cels (3DO-style CCB) get 16.16 fixed-point screen corners written at CCB +0x44..+0x60 by FUN_0046d790. FUN_00422630/FUN_004227f0 convert them to pixels using shift DAT_005aaa14 and mask DAT_005aaa18. FUN_004688e0 builds a quad list with int16 vertices and calls the scanline rasterizer FUN_0042e850 (edge table of 480 rows at DAT_005ac768, 640-entry reciprocal table DAT_0059253c, span routines via function pointer DAT_005ac760, nearest mip level, point sampling).
- Textures: PDAT chunks with magic 0xbc614e hold power-of-two 8bpp mip chains (width at +0x28, height +0x2c, level count +0x30, level table at +0x38 of 12-byte entries (w-1, -2w, offset from chunk start)). Palette: DATA/PALETTE.RAW, 256 x RGBx; index 0 is the transparent colour key.
- Texture ID used for replacement packs: "<w>x<h>_<crc32 of level-0 indices, 8 lowercase hex>".
- Game logic runs on a fixed 60 Hz tick (FUN_0042b6b0, QueryPerformanceCounter busy-wait). Must stay at 60 fps.

Constraints for all code:
- C++17, MSVC 14.51 x86 (VS 18 2026), Windows SDK 10.0.26100, CMake, static CRT (/MT). No third-party libraries and no downloads.
- Hooks: 5-byte JMP rel32 with a trampoline; verify the expected bytes in memory before patching and refuse on mismatch; never relocate relative instructions.
- Never LoadLibrary from DllMain.
- Everything must be switchable off from rr95gpu.ini; any failure falls back to the original game path and logs to rr95gpu.log.
- English for code, comments and docs.
Return complete files (not fragments) plus a short list of the exact checks you expect the reviewer to run.
```

---

## B1: Proxy ddraw.dll skeleton + hook engine (task T2)

Status: completed; verified with forward_test, hook_test, check_hook_sites, and live game run.

- **Goal:** CMake project `renderer/` producing a 32-bit proxy `ddraw.dll`: forwards `DirectDrawCreate` to the system DLL, `rr95gpu.ini` (`[General] Enabled, LogLevel`), `rr95gpu.log`, exe SHA-256 check, inline hook engine, and one pass-through hook on `FUN_0046e630` that logs FPS every 5 s. Includes `renderer/build.bat` and `renderer/tests/forward_test.cpp` (loads the DLL in a non-game process, expects S_OK and a "hooks skipped" log line).
- **Attach:** decomp excerpts of `FUN_0046e630`, `FUN_00467b80` and their call sites; first 32 bytes at each hook VA from both exes.
- **Review checks:** build succeeds; `forward_test` exits 0; `dumpbin /exports` shows `DirectDrawCreate`; `dumpbin /headers` shows x86; hook bytes match both exes; game starts with the DLL and the log shows hooks installed plus about 60 fps.

## B2: D3D11 presenter (task T3)

- **Goal:** replace the GDI present. Hook the race present (`FUN_0046e630`/`FUN_00467b80`) and the menu present (`FUN_0046e470`/`FUN_0046ded0`). Create a D3D11 device and a flip-model swap chain on the game window ("RashRoot"). Each frame upload the 640x480 8bpp buffer as an R8_UINT texture plus the 256-entry palette as a 256x1 texture. Expand it in a pixel shader, aspect-fit to the window with black bars, filter selectable in the ini (`Filter=nearest|bilinear|sharp-bilinear`). Upload menus as RGB565. Handle window resize and the windowed/borderless toggle (`FUN_00460b00`). On any D3D failure, fall back to the original GDI path.
- **Attach:** B1 output; decomp excerpts of `FUN_00467b80`, `FUN_0046e630`, `FUN_0046e470`, `FUN_0046ded0`, `FUN_0046e0f0`, `FUN_00460b00`, `FUN_0046e390`; the present-path report from B1.
- **Review checks:** build; race and menus display correctly; 60 fps; `Enabled=0` restores GDI; alt-tab and window toggle do not crash; no GDI `StretchDIBits` calls while the presenter is active (log counter).

## B3: Draw-list capture (task T4)

- **Goal:** hook `FUN_004688e0` (quad list) and the cel placement path (`FUN_0046d790`, `FUN_00422630`, `FUN_004227f0`). Record per frame: draw order, full-precision 16.16 corners (before snapping), texture pointer + PDAT header, UV box, mip info, flags, clip rect. Find every code path that writes to the 8bpp framebuffer *outside* the rasterizer (horizon blit, HUD, bikes, dashboard, text). Add a hotkey (F11) that dumps the current frame's list to `frame_<n>.json` and the framebuffer to `frame_<n>.png`.
- **Attach:** B2 output; decomp excerpts of `FUN_004688e0`, `FUN_0042e850`, `FUN_0042e4d0`, `FUN_0046d790`, `FUN_00422630`, `FUN_004227f0`, `FUN_00422b40`, `FUN_0045ac50` (race frame) and its callees list; CCB struct notes.
- **Review checks:** a dumped frame JSON re-rendered by a Python script matches the framebuffer PNG (drawn-pixel coverage within tolerance); a list of non-rasterizer writers with function addresses.

## B4: GPU world rendering at 1440x1080 (task T5)

- **Goal:** when `GpuWorld=1`, skip the software rasterizer for world quads and draw them with D3D11 into a 1440x1080 render target (ini `InternalHeight=1080`, 4:3) in engine draw order (painter's algorithm, no depth buffer). Convert textures from 8bpp + palette to RGBA8 with alpha from index 0, cached by PDAT pointer and invalidated on level/load change. Composite the non-rasterizer layers from the 8bpp buffer on top through a mask (index-0 transparency or a layer flag found in B3). Keep the sky/horizon as a separate textured layer when B3 shows it is a blit.
- **Attach:** B3 output plus 3 dumped frames (JSON + PNG) from different tracks.
- **Review checks:** visual parity side by side with the software renderer on the dumped frames; 60 fps at 1080p; HUD and bikes correct; toggling `GpuWorld` at runtime (hotkey F10) works.

## B5: Sub-pixel positions and blended mipmaps (task T6)

- **Goal:** use the captured 16.16 corners directly (float vertices) instead of snapped int16 values. Generate full mip chains on the GPU (or reuse the PDAT levels) and use trilinear plus optional anisotropic filtering (ini `Anisotropy=0..16`). Remove popping between levels.
- **Attach:** B4 output; short screen recordings or frame sequences showing the jitter before the change.
- **Review checks:** a 120-frame capture of the same straight road shows monotonic, sub-pixel building motion (no 1-2 px steps); no mip popping; 60 fps.

## B6: High-resolution texture pack (task T7)

- **Goal:** at texture creation, compute the texture ID; if `texture-pack/<id>.png` exists (any power-of-two multiple of the original size, RGBA), load it with WIC and use it instead. Keep the original UV box in normalized coordinates. Add a Python tool `tools/upscale.py` that reads `dumps/textures/manifest.json` and produces 4x upscales (Pillow Lanczos baseline; optional external upscaler command configurable on the command line), preserving alpha edges from index 0.
- **Attach:** B5 output; `tools/texdump.py`; one sample of the manifest.
- **Review checks:** a pack with 3 replaced textures loads (log lines with IDs); missing textures fall back; memory stays reasonable with the full pack (report VRAM estimate); no load-time stutter at track start beyond 1 s.

## B7: Config, install and docs (task T8)

- **Goal:** final `rr95gpu.ini` with comments; `tools/install.py install|uninstall` that copies `ddraw.dll`, `rr95gpu.ini` and `texture-pack/` into the Steam folder with backups and removes them cleanly; `README.md` with requirements, install, options, troubleshooting and the note that Steam "Verify integrity" does not delete extra files but restores the exe.
- **Attach:** all previous outputs.
- **Review checks:** install then uninstall leaves the Steam folder byte-identical to before (hash listing); README steps work from a clean state.

---

## Reviewer protocol (Claude, per batch)

1. Apply the returned files on `feat/gpu-renderer`, only inside the batch's surface (`renderer/**`, `tools/**`).
2. Build with `renderer/build.bat`; run the unit tests.
3. Re-check every hook site's expected bytes against both exe copies.
4. Review hook safety (relocated instructions, calling conventions, stack cleanup), D3D resource lifetime, fallbacks, and threading.
5. Install into the game for a short run when the batch needs in-game proof; read `rr95gpu.log`.
6. Commit one work unit per batch with a Conventional Commit message; update `odd/tasks/rr95-gpu-renderer.md`.
7. Send findings back to the external model as a numbered list when a batch fails review; at most one correction round per batch before escalating to the user.
