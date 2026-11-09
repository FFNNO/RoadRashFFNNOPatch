# Road Rash 95 at 1080p: how it was built

This document tells the story of the project from the first commit to today: what was wrong, how each part was solved, which tools and AI models did the work, and how it was checked. Exact evidence (commits, test counts, measurements) lives in `odd/tasks/rr95-gpu-renderer.md` and `odd/tasks/rr95-save-feedback.md`.

## 1. The problem

Road Rash 95 (Steam 30th Anniversary Edition) draws everything in software into a 640x480, 8-bit framebuffer and stretches it to the window with GDI `StretchDIBits`. On a 1080p screen that means:

- a blocky image, because 640x480 is scaled 2.25x;
- buildings and scenery that jump 1 to 2 pixels at a time, because the engine snaps every sprite corner to whole pixels and picks one mip level with no blending. In motion this caused motion sickness;
- photo-based sprites (riders, cars, pedestrians) only 30 to 140 pixels wide, so the shape and length of a car were hard to read.

Higher-resolution textures alone could not help while the framebuffer stayed at 640x480.

## 2. The approach

Replace the renderer at run time instead of editing game files:

- The game loads `ddraw.dll` from its own folder first. A **proxy `ddraw.dll`** (32-bit C++, MSVC, CMake) forwards the real DirectDraw calls to Windows and, once loaded, installs hooks into the game code.
- Nothing in the game archives changes. Deleting the DLL restores the original game.
- Every hook checks the exact bytes it expects and the exe's SHA-256; on an unknown build it refuses to install instead of corrupting memory.

## 3. Reverse engineering

- **Ghidra** decompiled `RoadRash95.exe` into about 190,000 lines of C (`out/decomp.c`, not in git). The import table (`out/imports.tsv`) gave the entry points: `StretchDIBits`, `GetSaveFileNameA`, WSOCK32, TAPI32 and so on.
- Key findings:
  - present path: `FUN_00467b80` → `FUN_0046e630` (8bpp to 32-bit via the palette) → `StretchDIBits` in races, `FUN_0046ded0` (RGB565) in menus;
  - the engine's draw list: `FUN_004688e0` builds textured and flat quads, `FUN_0042e850` rasterizes them scanline by scanline;
  - sprite corners are computed in 16.16 fixed point (`FUN_0046d790`) and only then snapped to pixels (`FUN_00422630` / `FUN_004227f0`);
  - game data: FAM archives compressed with PKWARE DCL implode; PDAT blocks hold power-of-two 8-bit mip chains; the palette is RGBx with index 0 as the transparent colour;
  - game logic runs on a fixed 60 Hz tick (`FUN_0042b6b0`).
- Small Python tools decode the data: `tools/famrsc.py`, `tools/texdump.py` (302 unique textures from the archives), `scripts/rsc_probe.py`.

## 4. Building the renderer, step by step

Each step was a task with tests and an in-game check before the next one started.

| Step | What it does | Result |
|---|---|---|
| T2 Proxy DLL | Forwards `DirectDrawCreate`, writes `rr95gpu.log`, checks the exe hash, inline hook engine (5-byte jump plus trampoline) | Game runs unchanged with the DLL; hooks verified on the original and patched exes |
| T3 D3D11 presenter | Replaces `StretchDIBits` with Direct3D 11: uploads the 8-bit buffer and palette, scales with a filtered shader | 60 fps at 1920x1080, falls back to GDI if D3D fails |
| T4 Draw-list capture | Records every quad the engine draws (corners, UVs, texture, flags) and dumps frames to JSON/PNG | A tool re-renders the dumped quads and matches the game's own pixels at 99.3-99.6% precision |
| T5 GPU world | Draws the world on the GPU at 1440x1080 (4:3) in engine order; HUD and horizon are composited from the software buffer | Sharp world at 60 fps |
| T5c Road lines | Two triangles per quad bent road lines into zigzags. The shader now reproduces the engine's scanline edge walk per pixel | Proven by an independent integer model of `FUN_0042e850` (max error 0.0064 texel); lines stay straight |
| T6 Sub-pixel and mipmaps | Uses the 16.16 corners before snapping, and the engine's own mip chains with trilinear filtering and 8x anisotropy | Measured on video: edges no longer lock to the 640 grid (jitter gone); distant textures stop popping |
| T7 Texture pack | Dumps every texture the game uses at run time with a stable ID (`<w>x<h>_<crc32>.png`) and loads replacements 1x/2x/4x/8x the original size | Replacements swap in by ID; wrong sizes are rejected with a log line |

Smaller pieces along the way: a configurable toggle key (Scroll Lock, because F12 is Steam's screenshot key), a palette-fade factor so replacements darken with the game's fades, and a fix for a hang at exit (which turned out to be the vanilla game's own behaviour when closed mid-race).

## 5. The upscaled texture pack

### Collecting the textures
The textures the archives contain did not match what the game actually uploads (0 of 118 IDs matched), so the DLL itself dumps each texture the first time the game draws it (`[TexturePack] Dump=1`), converted with the game's base palette (`DATA/PALETTE.RAW`). Playing different tracks collects more textures: 1573 so far.

### Upscaling
- First baseline: Pillow Lanczos 4x. Smoother, but cars and riders were still hard to read.
- Then **Real-ESRGAN x4plus** (`realesrgan-ncnn-vulkan` v0.2.5, running on the GPU). It reconstructs detail, so car shapes and rider poses become readable.
- In game, x4plus looked smooth and "plastic" on the digitized photo sprites. Measured with Laplacian variance, it removed half of the fine detail on riders (about 752 down to 327).
- The current pack uses **4x_Remacri** through the same ncnn-vulkan runner. It keeps the photo grain: about 943 on riders, and about 861 against 335 on asphalt. The edge band was narrowed to 1 px (`--edge-band`), because the 6 px band also washed out edges.
- `tools/upscale.py` drives the model:
  1. transparent pixels get the neighbouring colours "bled" into them, so no dark fringe appears;
  2. the silhouette (alpha mask) is upscaled by the same model, so it follows the same smoothed outline as the colours;
  3. the outer band (1 px with Remacri; 1.5 original pixels with x4plus) takes colours from the sprite's interior, which removes the speckled halo the photo cut-outs carry. Thin parts such as poles keep their colours;
  4. images go to Real-ESRGAN in batches grouped by size.

### Problems found and fixed
- **Stray pixels around people:** the first version resized the alpha with Lanczos, so the outline kept the original staircase while the colours moved. Fixed by steps 2 and 3 above.
- **Striped garbage:** batching many image sizes into one Real-ESRGAN call corrupted about 9% of the outputs (123 of 1375). Batching by size gives output identical, pixel for pixel, to one-by-one runs, and is about 4x faster than per-image calls (1375 textures in 23 minutes).
- **Bike shimmer at race start:** replacements loaded lazily and swapped in on first draw. Now the whole pack preloads on a worker thread; the D3D textures are created there too, so loading does not wait for frames. 1375 textures (862 MiB) preload in 11.3 seconds, during the intro.

## 6. Quality-of-life: saving

The game asked for a file name in a Windows dialog on every save and load, and showed nothing after saving. The DLL now:
- answers the save and open dialogs with one fixed file (`Partida 1.RRS`, configurable), so saving and loading are one click;
- hooks the game's save and load routines, checks that they succeeded (valid 160-byte block, checksum, dirty flag cleared) and shows "Partida guardada" / "Partida cargada" for 3 seconds in the black side margin.

## 7. How the AI work was organised

- **Claude Code (Claude Opus)** acted as orchestrator and reviewer: mapping the decompilation, writing task plans, implementing several parts, running every build and test, and checking each change in the real game with screenshots, videos and log measurements.
- **Gemini through Antigravity** implemented several batches from written briefs (`out/gemini/B2..B6/brief.md`): the proxy DLL skeleton, the D3D11 presenter, the draw-list capture, the GPU world and the texture pack.
- Every batch was reviewed before it was accepted. Some came back with blocking defects (for example a crash at process exit, or a cache that was cleared on every texture load) and went through a correction round.
- **Gentle AI** ran an independent review on each work-unit commit (one to four reviewer "lenses": risk, resilience, readability, reliability). Findings that did not block were recorded as follow-ups.
- **Engram** kept persistent memory across sessions, and a feature document (`odd/tasks/*.md`) tracked every task, its evidence and the next step, so work could resume after interruptions.

## 8. Testing

- `hook_test`: 306 unit tests (hook engine, scanline model of the engine rasterizer, texture IDs, palette fade, pack loading, save redirect, toast layout).
- `forward_test`: the proxy forwards DirectDraw correctly with the renderer on and off.
- `check_hook_sites.py`: every hook site matches the expected bytes in both known exe builds.
- `verify_frame_dump.py`: re-renders captured frames and compares them with the game's own output.
- `upscale.py --self-test`: edges, holes, thin parts, batch-versus-single equality.
- In game: the game's **demo mode** (it starts racing by itself after about 5 minutes) was used for hands-free tests, measuring 60.0 fps, load times and jitter from the log and from 60 fps video.

## 9. Numbers

- About 8,000 lines of C++ in the renderer plus 2,200 lines of tests; 46 commits in 4 working days (5 to 9 November 2026).
- Output: world at 1440x1080 inside a 1920x1080 window, locked 60 fps.
- Texture pack: 1573 textures at 4x with 4x_Remacri, 1025 MiB of GPU memory with mipmaps (`MaxMiB=1536`), preloaded in 12.8 s during the intro, 60.0 fps in race.
- Before/after captures: `out/screens/2026-11-09-showcase/` (vanilla vs current, a car close-up, and the same sprites as original, x4plus and Remacri).
- Install: copy `ddraw.dll`, `rr95gpu.ini` and `texture-pack/` next to `RoadRash95.exe`. Uninstall: delete them.

## 10. What is next

- Keep collecting textures from tracks not played yet and upscale them with Remacri.
- Install and uninstall scripts and a README (T8).
- A simplified online multiplayer built on the game's own network code (WSOCK32) and the proxy DLL.
