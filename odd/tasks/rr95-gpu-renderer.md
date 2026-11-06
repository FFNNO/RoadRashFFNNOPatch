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
- [ ] T0 Repository setup: git repo, `.gitignore`, existing scripts and tools committed. Route: inline. Check: `git status` clean except ignored paths.
- [ ] T1 Texture dump tool (`tools/texdump.py`): decode all PDAT mip chains and GLOBAL cels to PNG with stable IDs (crc32 of level-0 pixels + size) and a manifest. Route: inline (one file, understood format). Check: every PDAT found is dumped, IDs unique per content, spot-check images render recognisable buildings.
- [ ] T2 Proxy `ddraw.dll` skeleton: forwards `DirectDrawCreate`, logs to `rr95gpu.log`, verifies exe hash and hook-site bytes, inline hook helper. Route: delegated writer (2+ non-trivial files). Check: builds with MSVC x86; game runs unchanged with the DLL; log lists hooks.
- [ ] T3 D3D11 presenter: hook the race/menu present, upload 8bpp buffer + palette, draw with a filtered scaling shader into the window. Route: delegated writer. Check: game playable, 60 fps, image correct, toggle via config.
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
T0 then T1.
