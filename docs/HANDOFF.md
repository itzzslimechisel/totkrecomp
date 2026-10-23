# totkrecomp handoff (2026-10-23)

## Goal
Native Windows port of Zelda: Tears of the Kingdom 1.0.0 (user's PC: Windows 11, RTX 3070). Current priority from the user: **at least 60 FPS outdoors on the ground**, and a UI that works (menus, titles, audio).

## Locations
- Repo: `C:\Users\odwye\Desktop\Projects\MacNativeGames`, remote `totkrecomp` = github.com/itzzslimechisel/totkrecomp (PRs go to `main`). Remote `origin` is the old MacNativeGames fork, not used.
- Package (the user plays this): `C:\Users\odwye\totk_pkg\Tears of the Kingdom 1.0.0\` (`bin\totk-win.exe`, `settings.ini`, `logs\totk.log`, `user-data\save\`). Backup of the exe before #25: `bin\totk-win.exe.before-25`. Only 1.0.0 is kept (other versions deleted for space).
- Extract: `C:\Users\odwye\totk_extract` (game data; never commit). Keys: the user's `prod.keys` in Downloads (never commit, never copy).
- Test bench (not in the repo): `C:\Users\odwye\totk_bench\` — `act.py` (timed input + in-game shot + FPS), `pano.py`, `walkgrid.py`, `follow.py`, `ab2.sh` (interleaved A/B from a fresh profile copy), `win.py` (windowed FPS stats). Profiles: `prof-roa-base` (Room of Awakening), `prof-eden-base` (surface, from the Eden save). Prologue saves: `play\saves\stage1-statue-room`, `stage2-room-of-awakening`.
- Eden save: original `%APPDATA%\eden\nand\user\save\0000000000000000\C18D4B038621DB95E63D3654672CC3A1\0100F2C0115B6000` (never write there); backup `totk_bench\eden_save_backup\<time>\`; 1.0.0 format (progress.sav 2,307,552 bytes).
- Screenshots: `C:\Users\odwye\totk_shots\playthrough\` (01-13).
- Branches / stashes: `wip/static-recomp` (static recompiler, PR #7, not merged); local stash `perthread2` (per-thread scheduler stats).

## Architecture (as of #27)
- CPU: dynarmic JIT runs the original NSOs; 3 virtual cores (`runtime/threads.c`, NCORE 3, lock `G`); exclusive accesses without the global monitor by default (`TOTK_JIT_GLOBAL_MONITOR=1` restores it).
- Graphics: NVN -> Vulkan (`third_party/d2r_gfx/gfx/nvn/nvn_vk_*.c`); shaders to SPIR-V by `bin\xlate\totk_xlate.exe`; pipelines built in place up to `TOTK_PIPE_BUDGET_MS` (8) per frame, then on 3 workers; `VkPipelineCache` saved per GPU.
- Prepare shaders (`support\prepare_shaders.py`): writes `shader-cache\<key>.variants.tsv` plus one combined `index.tsv`.
- `settings.ini` next to the package, read by the launcher and by the runtime at start (environment variables win).
- Launcher (`Tears of the Kingdom.exe`): setup screen on first run, then Play / Game files / Settings / Saves / Controls; `--play` skips the window.
- In-game menu (F1 or Esc; pad Minus+Plus): Game, Video, Audio, Controls (incl. Remap buttons, writes `controls.txt`), Interface, Advanced (`TOTK_MENU_ADVANCED=1`). Game keys: WASD left stick, arrows camera, J A, K B, I X, U Y, Enter +, Tab -, F/T/H/G D-pad. The game's own menus move with WASD / D-pad, not the arrows.
- Diagnostics: focus-free input file `TOTK_INPUT_FILE` (`sdlkey`, `oshot <abs path>`, `key`, `down/up`, `threads`, `close`); `TOTK_VK_DUMP_RT=<present>,<dir>` (every render target of one frame + textures/samplers per pass in the log); `TOTK_PROF=<start>,<secs>,<file>` + `tools/windows/prof_report.py`; `TOTK_SCHED_STATS=1`; `TOTK_FS_TIMING=1`.

## Done (PRs on main)
#15-#17 focus-free tests, in-game menu, audio device; #22-#23 combined shader index (fast start); #24 button remap; #25 3D render targets get every slice (menus were navy/blue). Open, waiting for a merge by the user: #26 window title loses the boot status after the first frame; #27 `TOTK_AUDIO_OFF=0` no longer mutes (no sound with settings.ini).

## Measured (1.0.0, RTX 3070)
Prologue cave 61-66 FPS; ruins 58-63; cutscenes 55-105; Room of Awakening 39-40; surface (Eden save) 25-32 standing / 23 running, spikes to 500 ms. Ruled out as the limit: GPU (960x540 vs 1080p same FPS), translation thread (+3 %), the guest frame-pacing interval, FS reads (<1 ms/s), pipeline skips (icons still late with all pipelines built at once). The limit is the game's own threads under the JIT: core owners mostly run guest code or wait on each other (address-arbiter waits ~3,000/s).

## Next, in priority order
1. Open-world 60 FPS: JIT tuning (hot code, block linking, host-trap overhead), then scheduler (core hand-off cost, host thread priorities, 1 ms timer resolution not tried yet), then the static recompiler (#7) in small steps behind `TOTK_CPU=static`.
2. UI: location titles glitch, pause-menu backdrop black (scene blur pass), cutscene captions out of sync, audio stutter (check after #27; consider a bigger output buffer). Arrow keys / Enter in the game's menus would need a "menu open" signal from the game (none yet).
3. Untested: real controller by hand, other GPUs, long sessions. Colour comparison needs a reference picture from the user.

## Workflow
One small PR per change on `main` (never stacked); commit as `odwye <ojack6597@gmail.com>` with `core.autocrlf=false`. Merge (squash) when ctest and CI are green. The auto-mode safety check sometimes blocks `gh pr merge`, copies of the exe into the package (also impossible while the game runs) and copies of the Eden save: then the user approves at the PC or does it. Never touch the user's real save or running game window, never send desktop keys, never commit game data, keys or saves. Keep outputs short: the user's usage is limited.

## Build / test / run
- Build: `cmake --build build\totk-win-x64-vk-v0` (game exe, 1.0.0). Tests: `cd build\totk-win-x64-vkgame; cmake --build . --target test_keyboard_remap test_settings_ini; & "C:\Program Files\CMake\bin\ctest.exe"` (16 tests; use this ctest from PowerShell, the devkitPro/MSYS one breaks paths; the full build there fails on the deleted 1.4.3 data, the tests do not need it).
- Run a measured session: `python tools/windows/bench_run.py --pkg "<package>" --exe build/totk-win-x64-vk-v0/totk-win.exe --seconds 100 --out <dir> --autoload --keep-profile <copy of a profile>`.
