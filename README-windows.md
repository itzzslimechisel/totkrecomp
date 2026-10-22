# Windows 11 build (native x86-64 port)

`build_windows.py` makes a self-contained `Tears of the Kingdom <version>/` folder (one per game version) from **your own** NSP files and `prod.keys`. The game's ARM64 code is run by a JIT (dynarmic) inside a native Windows runtime that re-implements the Switch OS services; graphics go through a Vulkan backend. No emulator is downloaded, and nothing from the game, no keys, no firmware are distributed. You must own the game; the result is for your own use, do not share it.

## Requirements

- Windows 11 x64, a GPU and driver with **Vulkan 1.3**, a sound device, ~20 GB free disk, 16 GB RAM or more (the process takes about 6 GB).
- The shader translator ships as a native (NativeAOT) `bin\xlate\totk_xlate.exe`, so no .NET runtime is needed. Only when a package lacks it (or `TOTK_XLATE=dotnet.exe` is set) is the **.NET 10 runtime** used with `bin\translator\totk_xlate.dll`.
- Python 3.8+ with `pip install cryptography` (for the import).
- Game files: the base NSP (1.0.0) and optionally one update NSP, plus your `prod.keys`. Versions with a Windows patch table: **1.0.0** (base alone), **1.1.1**, **1.2.0**, **1.4.3** (smoke-tested: boot to rendered frames through Vulkan), 1.1.2 and 1.2.1 (tables from the Mac port, not tested on Windows). 1.4.2 and other updates are refused by the import (no patch table).
- Only to build the runtime yourself: LLVM-MinGW (UCRT, x86_64), CMake, Ninja, Python; MSYS2 `make`/`bash` for the cutscene decoder (libvpx), otherwise cutscenes use a built-in decoder that stops at the first one.

## Run

```
python build_windows.py base.nsp --update update-1.4.3.nsp -k prod.keys --version 1.4.3 -o D:\Games   # -> "Tears of the Kingdom 1.4.3"
python build_windows.py base.nsp -k prod.keys --version 1.0.0 -o D:\Games                            # -> "Tears of the Kingdom 1.0.0"
python build_windows.py -o D:\Games --data-dir <earlier import output> --version 1.2.0               # no import, no copy
python build_windows.py ... --prebuilt <folder with totk-win.exe + runtime-index.bin built for that version>
```

The runtime is built per version (`build/totk-win-x64-vk-<ver>`, `-DTOTK_GAME_VERSION=v0|v111|v120|v143|...`): the patch tables in `runtime/game_version.h` are compiled in. One import folder can hold several versions (`extracted/<ver>`); packages in the same output folder share the translator files as hard links. Then start `Tears of the Kingdom <version>.bat` (or the `.lnk`). The output folder must be outside the repository. Edit `settings.bat` for the window size, sound off, mouse-as-stick and the data folder; the log is `logs\totk.log`. `--translator <dir>` points at the published `totk_xlate.dll` (default `build\totk_xlate-win`; built from `games/totk/tools/totk_xlate`, needs the vendored Ryujinx shader sources).

## Launcher and setup screen

`Tears of the Kingdom.exe` (Dear ImGui, Direct3D 11) starts the game straight away. The setup screen opens on the first run (no game files yet), when **Shift** is held while starting it, with `--setup` (also the `... Settings.lnk` shortcut), or with `--tab 0-4` for a given page. Pages:

- **Play**: checks game files, Vulkan 1.3 and the shader translator, shows whether shaders are prepared (with a **Prepare shaders** button); Play button.
- **Game files**: import from NSP/XCI (+ optional update NSP) with your `prod.keys` (runs `support\totk_setup.py` on the bundled Python; progress bar and log), or adopt an already extracted `exefs` + `romfs` pair (the romfs folder must hold `romfs.bin`). The version is detected from the ExeFS module hashes. Each package runs one version: files of another version are imported for the sibling package `Tears of the Kingdom <ver>` next to it (its `settings.bat` is pointed at them), or refused with a clear message when there is none. Unsupported versions such as 1.4.2 are refused before the long RomFS step.
- **Settings**: written to `settings.bat` (the `.bat` launcher reads it too): window size, fullscreen, VSync, render resolution, GPU, 60 FPS patch and frame-rate cap, mute and volume, language, mouse-as-stick, rumble, gyro, face buttons, translator command. The shader cache cannot be switched off (it holds the translated shaders); it can be cleared. There is no log-level setting.
- **Saves**: import (folder or zip), export (zip), open folders.
- **Controls**: key map and controller notes.

`support\` holds `totk_setup.py`, the import tools, `prepare_shaders.py` and an embeddable Python (python.org, with `cryptography`, `lz4` and `zstandard`; `build_windows.py` downloads it once into `build\python-embed`). Building the launcher fetches Dear ImGui v1.91.9 (MIT) into `build\imgui`. New runtime variables: `TOTK_FULLSCREEN=1`, `TOTK_VOLUME=0-100`.

## Prepare shaders

The repository ships only shader *identifiers* (`games/totk/assets/shader_list.tsv`, see `docs/shader-list.md`), never shader code. After an import or adopt, the launcher runs **Prepare shaders** (`support\prepare_shaders.py`, also a button on the Play page): it finds every listed shader in **your** RomFS and translates each listed bind state to SPIR-V into `<package>\shader-cache\`, one translator process per core (up to 8, below-normal priority). It is resumable: stop it at any time and the next run skips the variants already made; `shader-cache\prepared.json` marks it done for the current list. A shader the list lacks is still translated in the background during play (the render thread never waits) and written to `user-data\shader-list-contribution.txt` for the shared list. Pipelines are built on worker threads through a `VkPipelineCache` saved next to the shaders (`vk-pipelines-<vendor>-<device>-<driver>.bin`), so later runs reuse them.

Measured on 1.0.0 (RTX 3070, prologue cave from a save, 180 s, `tools/windows/bench_run.py --autoload`, same runtime binary):

| | empty cache | prepared cache |
|---|---|---|
| Prepare shaders (one time) | - | 9 min 23 s: 35,580 variants (65 fail in the translator), 1.7 GB |
| Shaders translated during play | 500+ (their effects missing until ready) | 8 |
| Gameplay 40-180 s: average / 1% low FPS | 34.4 / 20.6 | 34.7 / 17.0 |

Gameplay frame rate is CPU-bound and does not change; the step removes missing effects and background translation the first time things appear. The long frames in the first 35 s of both runs are the title screen and the save load.

## Saves

The runtime stores the save in `<package>\user-data\save\<account id>\`; the id is `user-data\account.uid` (16 bytes) written as `hi:016x` + `lo:016x` (little-endian halves). That folder is the Switch save root: `slot_00\{progress,caption,footprint,direct_file_save_related}.sav`, `album\`, ... **Import** takes a folder or zip from Ryujinx (`bis\user\save\000...\0`), yuzu / Eden / Suyu (`nand\user\save\0000...\<user>\0100F2C0115B6000`) or Checkpoint: the shallowest folder holding `slot_NN`, `album` or `*.sav` is used. The current save is zipped to `user-data\save-backups\` first, then `slot_*`, `album` and `*.sav` are replaced. **Export** zips the same folder contents (re-importable). Quit the game first.

## Controls

Keyboard: the same layout as the Mac build (`keymap_sdl.h`, remappable through `controls.txt`). Controllers: any SDL3-supported pad (XInput, DualSense, Switch Pro), rumble and motion. **F11 / Alt+Enter**: fullscreen. **F1 or Esc** (controller: **Minus + Plus** together) opens the in-game menu: Resume, the FPS counter (off / FPS / FPS + frame time, saved in `settings.ini` as `TOTK_FPS_OVERLAY`) and Quit (asks first; a save being written finishes). The game gets no input while the menu is open. Arrows / D-pad move, Enter / A chooses, B goes back; the mouse works too. Closing the window waits for a running save.

## What works / what is untested

- Works (verified on the author's PC): import of both NSPs, boot, the prologue rendering in the window at 1920x1080 through Vulkan, sound through WASAPI (4 start-up underruns, none after), window events.
- Untested by hand: keyboard and gamepad input in game (the code is wired and the controller / keyboard devices initialize, but nobody has played with them), rumble, motion, fullscreen toggling, window resize, text entry, saving, long play sessions, other GPUs (tested on one NVIDIA card only).
- Performance is not good yet: about 35 FPS in the prologue on an RTX 3070 (CPU-bound in the JIT, not the GPU). No picture-side FPS overlay on Windows.
- 1.1.1 and 1.2.0 (Windows only) have no cloth/hair real-time clock patch (cloth follows the game clock at frame rates other than 30). The tests and the CMake build for ARM64 Windows are experimental.
