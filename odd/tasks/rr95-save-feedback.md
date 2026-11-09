# Feature: RR95 fixed save path and on-screen save feedback

Locator: `odd/tasks/rr95-save-feedback.md` (Engram mirror topic `odd/rr95-save-feedback/tasks`).

## Objective
Saving writes the game to one default file without a file dialog, loading reads that same file, and a short "game saved" message appears on screen (in the side margin when there is one).

## Problem
Road Rash 95 asks for a file name in a Windows dialog on the first save, on every "Salvar Juego Como..." and on every load. After a successful save it shows nothing, so the player gets no feedback.

## Why
The user wants saving and loading to be one step and wants visual confirmation that the save worked.

## Findings (read-only map, 2026-11-09)
- Save routine `FUN_0043f7c0` and load routine `FUN_0043f580` (`out/decomp.c:32522`, `:32433`) take no arguments and use the global file name buffer at `0x604320` (MAX 0x103 bytes). The save file is 160 bytes (`0xa0`), magic `0xCAFEDEAD` plus checksum. On success the save only clears the dirty flag `0x5cd47e`; errors go through `FUN_00458a30` (MessageBoxA).
- Dialogs: COMDLG32 `GetSaveFileNameA` (IAT slot `0x61c0b8`, call sites `0x45ae07` Salvar Juego, `0x45aef6` Salvar Juego Como, `0x45b77e` save-before-quit) and `GetOpenFileNameA` (IAT slot `0x61c0b4`, call site `0x45ae8f` Cargar Juego). `lpstrFile` is `0x604320`, default extension `RRS`, initial folder from `HKLM\SOFTWARE\Electronic Arts\RoadRash 95\Path` (the game folder).
- Existing save on this machine: `<game folder>\Partida 1.RRS`.
- The mod has one IAT helper (`InstallIatStretchDIBitsHook`, `renderer/src/hook_sites.cpp:80`) and no text drawing in the presenter.

## Scope
- Default save file: `<game folder>\Partida 1.RRS`, configurable with `[Save] File=` in `rr95gpu.ini` (relative paths resolve against the game folder). Using the existing name keeps the player's current progress loadable.
- Every save dialog (Salvar Juego, Salvar Juego Como, save-before-quit) is answered with the default file without showing the dialog.
- The load dialog is answered with the default file when it exists; when it does not exist, the real dialog opens as before.
- After a successful save, the text "Partida guardada" is drawn for about 3 s; after a successful load, "Partida cargada". It goes in the side margin (pillarbox) when the margin is wide enough, otherwise in a corner of the image over a translucent background. It works in the menu present path and the race present path.
- `[Save] Enabled=0` restores the original dialogs and shows no message.

## Constraints
- No change to the save file format or to the game's own save and load routines.
- Hooks follow the existing `HookSite`/IAT patterns, are verified against the expected bytes, and are restored on uninstall.
- Toast text is Spanish to match the game's language; code and comments stay in English.
- No teardown at process exit (leaked singletons, as in the renderer).

## Tasks
- [ ] S1 Redirect the COMDLG32 save and open dialogs to the default file (generalized IAT helper, config, path resolution) with unit tests. Route: delegated writer (2+ non-trivial files).
- [ ] S2 Detect a successful save and load and draw the toast in both present paths (GDI-rendered text texture, margin placement) with unit tests for placement and timing. Route: delegated writer, same work unit as S1 if it stays coherent.
- [ ] S3 In-game check: save from the menu without a dialog, file written at the default path, toast visible; load from the menu without a dialog; missing file falls back to the dialog.

## Acceptance criteria
- Saving from any menu entry shows no dialog and updates `<game folder>\Partida 1.RRS` (size 160, new timestamp).
- Loading shows no dialog when the file exists and restores that game.
- "Partida guardada" / "Partida cargada" is visible on screen after the action and disappears after about 3 s.
- `[Save] Enabled=0` gives the original behaviour.
- hook_test, forward_test and check_hook_sites pass.

## Checks
- `cmd //c "renderer\\build.bat"`, `renderer/build/Release/hook_test.exe`, `renderer/build/Release/forward_test.exe`, `python renderer/tools/check_hook_sites.py`.
- In-game: Steam game folder, screenshots in `out/screens/2026-11-09-save/`.

## Progress
- 2026-11-09: Feature document created after a read-only map of the save and load flow.

## Next step
S1 and S2 through one delegated writer.
