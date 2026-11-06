# RR95 GPU Renderer

## Objective
Replace the 640x480 8-bit software rasterizer output of Road Rash 95 (Steam 30th Anniversary Edition) with a Direct3D 11 renderer injected through a proxy `ddraw.dll`, rendering world polygons at 1440x1080 (4:3 in 1080p) with bilinear/trilinear filtering, sub-pixel positions and optional high-resolution replacement textures.

## Problem
- The engine draws everything into a 640x480 8bpp framebuffer and upscales it with GDI `StretchDIBits`, so the image is blocky at 1080p.
- Scenery sprites snap to integer/2-pixel positions and pick the nearest mip level without blending, which makes buildings jitter and pop (motion sickness).
- Upscaled textures alone gain nothing while the framebuffer stays 640x480.

## Why
The user owns the game and cannot play it comfortably. A runtime renderer replacement is reversible (delete the DLL) and does not require repacking game archives.

## Scope
- Asset tooling: dump scenery textures (FAMILIES.RSC PDAT mip chains, GLOBAL.RSC cels) to PNG with stable IDs; upscale tool producing a replacement texture pack.
- Proxy `ddraw.dll` (32-bit) forwarding `DirectDrawCreate` to the system DLL and installing inline hooks after verifying the exe build.
- D3D11 presenter replacing the GDI present path.
- Capture of the engine draw list (`FUN_004688e0` / `FUN_0042e850`) and GPU rasterization of world polygons at the internal resolution, compositing 2D/HUD from the software buffer.
- Sub-pixel positions (16.16 corners before snapping) and blended mipmaps.
- Config file, install/uninstall scripts, README.

## Constraints
- Never modify game data archives; the Steam install only receives the DLL, config and texture pack (plus the existing exe byte patch, which stays optional).
- Hooks must verify expected bytes and refuse to install on an unknown exe build (both the original sha256 `9e40e257...` and the patched `40a0d85f...` are known).
- Keep 60 fps; the game logic runs on a fixed 60 Hz tick (`FUN_0042b6b0`).
- Copyrighted binaries, decompilation and dumps stay out of git (`.gitignore`).
- Toolchain: MSVC 14.51 x86 (VS 18 Community), Windows SDK 10.0.26100, CMake from VS.

## Key engine facts (from Ghidra analysis)
- Present: `FUN_00467b80` -> `FUN_0046e630` (8bpp -> 32-bit DIB via palette) -> `StretchDIBits` (race); menus `FUN_0046e470` -> `FUN_0046ded0` (RGB565).
- Main 8bpp surface `DAT_005cd4b0` 640x480 system memory; draw globals pitch `DAT_006036b8`, base `DAT_006036bc`; clip `DAT_006036c0..cc`.
- Viewport 320x240 at `.data` 0x591C04/0x591C08, shifted by HiRes flag `DAT_005a292a`.
- Cel placement writes 16.16 corners into CCB +0x44..+0x60 (`FUN_0046d790`); snapping in `FUN_00422630`/`FUN_004227f0` with `DAT_005aaa14`/`DAT_005aaa18`; quad list `FUN_004688e0` stores int16 vertices; rasterizer `FUN_0042e850` (nearest mip, span routines via `DAT_005ac760`).
- FAM payloads: PKWARE DCL implode (method 1), header `usize, csize, crc32, method`; PDAT magic 0xbc614e holds power-of-two 8-bit mip chains.

## Delivery
Local-only git repository (`mods-rr`, branch `feat/gpu-renderer`), no remote: pull-request slicing does not apply until a remote exists. Work-unit commit per task.

## Tasks
- [x] T0 Repository setup: git repo, `.gitignore`, existing scripts and tools committed. Route: inline. Check: `git status` clean except ignored paths. Evidence: commit `08f5f66`, status clean.
- [x] T1 Texture dump tool (`tools/texdump.py`): decode all PDAT mip chains and GLOBAL cels to PNG with stable IDs (crc32 of level-0 pixels + size) and a manifest. Route: inline (one file, understood format). Check: every PDAT found is dumped, IDs unique per content, spot-check images render recognisable buildings. Evidence: 302 unique textures (276 families, 16 global, 10 horizons) from 813 uses; contact sheet shows facades, road strips and colour-key transparency. Findings: palette is RGBx, index 0 is the colour key; horizons are 720x320 (hi) / 360x160 (lo) 8bpp, not 640x360; GLOBAL.RSC chunks are stored uncompressed (famrsc.get now validates the header).
- [x] T2 Proxy `ddraw.dll` skeleton: forwards `DirectDrawCreate`, logs to `rr95gpu.log`, verifies exe hash and hook-site bytes, inline hook helper. Route: delegated writer (2+ non-trivial files). Check: builds with MSVC x86; game runs unchanged with the DLL; log lists hooks. Evidence: forward_test and hook_test pass; dumpbin confirms 32-bit x86 DirectDrawCreate export; check_hook_sites confirms byte match on both original and patched exes; game launches and logs detected patched build with FUN_0046e630 hook installed. Implementation committed by Gemini in `ce9186c` (log/config/exe_identity/hook rewritten over the Claude writer's version; writer's tests reused as cross-check). Reviewer re-run 2026-11-05: build OK, forward_test 0 failures, hook_test 0 failures, check_hook_sites OK on both exes, installed DLL identical to build; log shows ASLR rebase (hook at 003BE630). In-game proof 2026-11-05 23:39: user played a race with the DLL; rr95gpu.log shows the detour running at a steady 60.0 fps (first 5 s window 48.0 is warm-up). Follow-ups: log opens in append mode (one log across sessions); DLL_PROCESS_DETACH frees ddraw and removes hooks inside DllMain (only on dynamic unload, which the game never does).
  - Native review: granted, lineage `review-3bd91a34343f968a`, lens review-reliability, approved and acknowledged (boundary advances to `ce9186c`).
- [x] T2b Hardening from T2 advisory findings (non-blocking, fold into B2 or do separately): DllMain detach must not FreeLibrary/unpatch under the loader lock (pin the DLL instead); test `Enabled=0`; set `/INCREMENTAL:NO` on hook_test so Debug builds do not hit the ILT thunk; add a RemoveHook test (bytes restored, trampoline nulled, original result returned); initialise the log critical section eagerly; ValidateHookDecoding should reject prefixed relative instructions. Route: delegated writer. Evidence: forward_test tests Enabled=0 in child process and Enabled=1 in parent; hook_test tests prefixed relative instructions and RemoveHook; DllMain pins module via GET_MODULE_HANDLE_EX_FLAG_PIN.
- [x] T3 D3D11 presenter: hook the race/menu present, upload 8bpp buffer + palette, draw with a filtered scaling shader into the window. Route: delegated writer. Check: game playable, 60 fps, image correct, toggle via config. Evidence: build OK (/W4 /WX), forward_test and hook_test pass, check_hook_sites OK on all 3 sites across both exes, in-game log confirms D3D11 presenter initialized on 1920x1080 HWND (feature level 11_0) and the first frame presented with gdi_blits=0. Implementation by Gemini in `f35165d`; reviewer re-run 2026-11-06 confirmed build, tests, hook sites and calling conventions (`ret 4` on FUN_0046e630/FUN_00464140). Pending: race + menu run with `FPS:` lines near 60 (the reviewed log ended before the first 5 s window).
  - Native review: granted, lineage `review-6ef98a6554a8b088`, lens review-reliability, approved and acknowledged (boundary advances to `f35165d`).
- [ ] T3b Hardening from T3 advisory findings (non-blocking, fold into B3): latch presenter failure after N attempts and stay on GDI instead of re-creating the device every frame (presenter.cpp:204-209); on ResizeBackbuffer/CreateSourceTextures failure release the swap chain before GDI fallback (presenter.cpp:210-220); null-guard in race/menu detours must call the original instead of silently dropping the frame (hook_sites.cpp:188-190, 246-248); log when the StretchDIBits IAT hook is not installed and print `gdi_blits=n/a` (hook_sites.cpp:341); unit-test filter parsing and letterbox math, warn on unknown Filter values (config.cpp:32-41). Route: delegated writer (Gemini B3).
- [ ] T4 Draw-list capture: record quads/cels per frame with full-precision corners, texture pointer, UVs and flags; dump one frame for analysis; identify non-rasterizer framebuffer writers (horizon, HUD, bikes). Route: delegated explorer + writer.
- [ ] T5 GPU world rendering at 1440x1080 in engine draw order, HUD composited from the software buffer. Route: delegated writer.
- [ ] T6 Sub-pixel corners and blended mipmaps. Route: delegated writer.
- [ ] T7 High-resolution texture pack loading by ID plus upscale tool (Pillow Lanczos baseline; Real-ESRGAN optional, needs download approval). Route: delegated writer.
- [ ] T8 Config, install/uninstall scripts, README. Route: inline.

## Acceptance criteria
- With the DLL installed the game runs at 60 fps, renders the world at 1440x1080 with filtered textures, and scenery no longer snaps or pops.
- Removing the DLL restores the original behaviour.
- Replacement textures load when present and fall back to originals otherwise.

## Progress
- 2026-11-05: Exe byte patch (sub-pixel HiRes snap + HALFTONE present) installed and confirmed by the user to improve jitter with unchanged performance.

## Next step
User race + menu run to confirm T3 at 60 fps; then T4 Draw-list capture (Batch B3) with the T3b hardening folded in.
