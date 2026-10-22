#!/usr/bin/env python3
"""Package the native Windows x86-64 port of Tears of the Kingdom from YOUR OWN game files and keys, one folder per game version.

  python build_windows.py base.nsp --update update-1.4.3.nsp -k prod.keys -o "C:\\Games"      -> "Tears of the Kingdom 1.4.3"
  python build_windows.py base.nsp -k prod.keys -o "C:\\Games"                                 -> "Tears of the Kingdom 1.0.0"
  python build_windows.py --data-dir C:\\totk_extract --version 1.0.0 -o "C:\\Games"            (reuse an earlier import)

Steps: (a) tools/windows/import_game.py verifies and extracts ExeFS + RomFS from your NSP(s) with your prod.keys (it accepts only
the versions the runtime is patched for: 1.0.0, 1.1.1, 1.1.2, 1.2.0, 1.2.1, 1.4.3; 1.1.1 and 1.2.0 are Windows only);
(b) the runtime (games/totk, target totk-win) for that version is copied from a prebuilt folder or built with CMake + LLVM-MinGW +
Ninja (one build folder per version: the game-code patch tables are compiled in, runtime/game_version.h);
(c) a self-contained folder is laid out:

  Tears of the Kingdom <version>/
    Tears of the Kingdom.exe          double-click launcher (tools/windows/launcher, Dear ImGui): starts the game straight away; the setup
                                      UI (game files import, settings, saves, controls) opens on the first run, with --setup or Shift held
    Tears of the Kingdom <version> Settings.lnk   the launcher with --setup
    support/                          totk_setup.py + the import tools + an embeddable Python (with cryptography, lz4) for the setup UI
    Tears of the Kingdom <version>.bat / .lnk   fallback launcher (same environment, console)
    settings.bat                      the settings you may edit (window size, sound, data folder, translator command)
    bin/totk-win.exe, runtime-index.bin, translator/   the runtime and the shader translator (needs the .NET 10 runtime)
    data/extracted/<ver>/{exefs,romfs} your game data, written by the import (about 17 GB; never share it)
    user-data/ shader-cache/ logs/

Packages of several versions in the same output folder share the translator files (hard links, so no extra disk space).
Nothing from the game or keys is downloaded or distributed. Python standard library only.
"""
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent
APP = 'Tears of the Kingdom'
# Game versions with a Windows patch table (runtime/game_version.h): display -> extracted/<dir>, assets/expected-v<display>.json.
# 'tested' = boots to rendered frames on Windows (Vulkan); the others have tables from the Mac port but no Windows smoke test.
VERSIONS = {'1.0.0': ('v0', True), '1.1.1': ('v111', True), '1.1.2': ('v112', False), '1.2.0': ('v120', True),
            '1.2.1': ('v121', False), '1.4.3': ('v143', True)}
TRANSLATOR_DEFAULT = REPO / 'build' / 'totk_xlate-win'
TRANSLATOR_AOT_DEFAULT = TRANSLATOR_DEFAULT / 'aot-win-x64'   # build_win.ps1 -Aot: no .NET runtime needed


def say(msg):
    print(f'[build_windows] {msg}', flush=True)


def die(msg):
    sys.exit(f'build_windows: {msg}')


def run(cmd, **kw):
    say(' '.join(f'"{c}"' if ' ' in str(c) else str(c) for c in cmd))
    if subprocess.call([str(c) for c in cmd], **kw):
        die(f'command failed: {cmd[0]}')


def find_tool(name, globs=()):
    found = shutil.which(name)
    if found and 'msys' not in found.lower() and 'cygwin' not in found.lower():   # MSYS2 cmake mangles Windows paths
        return found
    for base, pattern in globs:
        for p in sorted(Path(base).glob(pattern)) if Path(base).exists() else []:
            if p.is_file():
                return str(p)
    return None


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(8 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def main_digest(display):
    return json.loads((REPO / f'games/totk/assets/expected-v{display}.json').read_text())['exefs_sha256']['main']


def detect_version(data):
    """The game versions whose extracted/<ver>/exefs/main matches the expected hash and whose romfs.bin exists."""
    found = []
    for display, (vdir, _) in VERSIONS.items():
        folder = data / 'extracted' / vdir
        main = folder / 'exefs' / 'main'
        if main.is_file() and (folder / 'romfs' / 'romfs.bin').is_file() and sha256(main) == main_digest(display):
            found.append(display)
    return found


def build_runtime(vdir, data_version_dir, build_dir):
    """Configure + build totk-win (Vulkan backend) for one game version with LLVM-MinGW, CMake and Ninja; returns the build dir."""
    winget = Path(os.environ.get('LOCALAPPDATA', '')) / 'Microsoft/WinGet/Packages'
    clang = find_tool('x86_64-w64-mingw32-clang', [(winget, '*LLVM-MinGW*/*/bin/x86_64-w64-mingw32-clang.exe')])
    cmake = find_tool('cmake', [('C:/Program Files/CMake/bin', 'cmake.exe')])
    ninja = find_tool('ninja', [(winget, '*Ninja*/ninja.exe')])
    if not (clang and cmake and ninja):
        die('building needs LLVM-MinGW (x86_64-w64-mingw32-clang), CMake and Ninja; or pass --prebuilt <folder>')
    env = os.environ.copy()
    env['PATH'] = os.pathsep.join([str(Path(clang).parent), str(Path(cmake).parent), str(Path(ninja).parent), env['PATH']])
    run([cmake, '-S', REPO / 'games/totk', '-B', build_dir, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=RelWithDebInfo',
         '-DCMAKE_C_COMPILER=x86_64-w64-mingw32-clang', '-DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-clang++',
         '-DTOTK_GFX=vk', f'-DTOTK_DATA_DIR={data_version_dir.as_posix()}', f'-DTOTK_GAME_VERSION={vdir}',
         '-DTOTK_BUILD_TESTS=OFF'], env=env)
    run([cmake, '--build', build_dir, '--target', 'totk-win'], env=env)
    return build_dir


def link_tree(src, dst, shared):
    """Copy src to dst; files identical to one already in `shared` (another version's package) become hard links."""
    if dst.exists():
        shutil.rmtree(dst)
    for p in src.rglob('*'):
        rel = p.relative_to(src)
        if p.is_dir() or p.suffix in ('.pdb', '.nupkg'):
            continue
        target = dst / rel
        target.parent.mkdir(parents=True, exist_ok=True)
        other = shared / rel if shared else None
        if other and other.is_file() and other.stat().st_size == p.stat().st_size and sha256(other) == sha256(p):
            try:
                os.link(other, target)
                continue
            except OSError:
                pass
        shutil.copy2(p, target)


SETTINGS = r'''@echo off
rem Settings for "@APP@". Edit and relaunch.
rem Folder holding extracted\@VER@\exefs and extracted\@VER@\romfs (the import output, game version @DISPLAY@).
set "TOTK_DATA=@DATA@"
rem Window size at start (F11 or Alt+Enter toggles fullscreen).
set "TOTK_WINDOW_SIZE=1920x1080"
rem Set to 1 to run without sound.
set "TOTK_AUDIO_OFF=0"
rem Mouse as right stick and ZR/ZL/R3 buttons: remove "rem" on the next line.
rem set "TOTK_MOUSE=1"
rem Shader translator: empty = the bundled native one (bin\xlate). dotnet.exe = the .NET build in bin\translator (needs the .NET 10 runtime).
set "TOTK_XLATE="
'''

LAUNCHER = r'''@echo off
setlocal
set "HERE=%~dp0"
call "%HERE%settings.bat"
if not exist "%HERE%logs" mkdir "%HERE%logs"
set "TOTK_ROMFS=%TOTK_DATA%\extracted\@VER@\romfs"
set "TOTK_ROMFS_INDEX=%HERE%bin\runtime-index.bin"
set "TOTK_PROFILE_DIR=%HERE%user-data"
set "TOTK_SHADER_CACHE=%HERE%shader-cache"
set "TOTK_XLATE_CMD="%HERE%bin\xlate\totk_xlate.exe""
if defined TOTK_XLATE set "TOTK_XLATE_CMD="%TOTK_XLATE%" "%HERE%bin\translator\totk_xlate.dll""
if "%TOTK_AUDIO_OFF%"=="0" set "TOTK_AUDIO_OFF="
if not exist "%TOTK_ROMFS%\romfs.bin" (echo Game data @DISPLAY@ not found in "%TOTK_DATA%\extracted\@VER@". Edit settings.bat. & pause & exit /b 1)
pushd "%HERE%"
"%HERE%bin\totk-win.exe" "%TOTK_DATA%\extracted\@VER@\exefs" boot 2> "%HERE%logs\totk.log"
set RC=%ERRORLEVEL%
popd
if not "%RC%"=="0" (echo The game exited with code %RC%. See logs\totk.log & pause)
exit /b %RC%
'''


def build_launcher(dest):
    sys.path.insert(0, str(REPO / 'tools/windows/launcher'))
    import build as launcher_build
    cache = REPO / 'build' / 'totk-launcher.exe'
    cache.parent.mkdir(exist_ok=True)
    srcs = [REPO / 'tools/windows/launcher' / f for f in ('launcher.cpp', 'launcher.rc', 'launcher.manifest', 'totk.ico', 'build.py')]
    if not cache.is_file() or cache.stat().st_mtime < max(f.stat().st_mtime for f in srcs if f.is_file()):
        say('building the launcher exe')
        launcher_build.build(cache)
    shutil.copy2(cache, dest)


def embedded_python():
    """build/python-embed/<X.Y.Z>/: the python.org embeddable Python of the running version + cryptography + lz4 (downloaded once)."""
    import urllib.request
    import zipfile
    v = '%d.%d.%d' % sys.version_info[:3]
    root = REPO / 'build' / 'python-embed' / v
    if (root / 'site-packages' / 'zstandard').is_dir() and (root / 'site-packages' / 'lz4').is_dir() and (root / 'python.exe').is_file():
        return root
    shutil.rmtree(root, ignore_errors=True)
    root.mkdir(parents=True)
    url = f'https://www.python.org/ftp/python/{v}/python-{v}-embed-amd64.zip'
    say(f'downloading {url}')
    z = root / 'embed.zip'
    urllib.request.urlretrieve(url, z)
    with zipfile.ZipFile(z) as zf:
        zf.extractall(root)
    z.unlink()
    run([sys.executable, '-m', 'pip', 'install', '--quiet', '--target', root / 'site-packages', '--only-binary=:all:', '--platform', 'win_amd64',
         '--python-version', '%d.%d' % sys.version_info[:2], 'cryptography', 'lz4', 'zstandard'])
    for pth in root.glob('python*._pth'):   # embeddable Python ignores PYTHONPATH: list what the helper needs (paths relative to this file)
        pth.write_text(pth.read_text().replace('#import site', 'import site') + 'site-packages\n..\n../games/totk/scripts\n')
    return root


def build_support(pkg, shared_with=None):
    """pkg/support: totk_setup.py, the import tools (import_game.py, build_app.py, ExeFS/RomFS readers, expected hashes) and the Python they run on."""
    sup = pkg / 'support'
    py = embedded_python()
    for src, dst in [(REPO / 'tools/windows/launcher/support/totk_setup.py', 'totk_setup.py'), (REPO / 'tools/windows/import_game.py', 'tools/windows/import_game.py'),
                     (REPO / 'build_app.py', 'build_app.py')] + \
                    [(REPO / 'games/totk/scripts' / f, f'games/totk/scripts/{f}') for f in ('extract_exefs.py', 'extract_romfs.py', 'prepare_modules.py', 'prepare_shader_seed.py', 'shader_list.py')] + \
                    [(REPO / 'tools/windows/prepare_shaders.py', 'prepare_shaders.py'), (REPO / 'games/totk/assets/shader_list.tsv', 'games/totk/assets/shader_list.tsv')] + \
                    [(p, f'games/totk/assets/{p.name}') for p in sorted((REPO / 'games/totk/assets').glob('expected-v*.json'))] +                     [(REPO / 'games/botw/assets' / f, f'games/botw/assets/{f}') for f in ('expected-v1.6.0.json', 'shader_list.tsv')]:   # build_app.py reads them at import
        (sup / dst).parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, sup / dst)
    target = sup / 'python'
    if not (target / 'python.exe').is_file() or not (target / 'site-packages' / 'zstandard').exists():
        link_tree(py, target, shared_with / 'python' if shared_with else None)
    return sup


def write_bat(path, text):
    path.write_bytes(text.replace(chr(13), '').replace(chr(10), chr(13) + chr(10)).encode('utf-8'))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('base', nargs='?', type=Path, help='base game NSP (1.0.0); omit with --data-dir')
    ap.add_argument('--update', type=Path, help='cumulative update NSP (1.1.2, 1.2.1 or 1.4.3); without it the package is 1.0.0')
    ap.add_argument('-k', '--keys', type=Path, help='your prod.keys')
    ap.add_argument('--version', choices=sorted(VERSIONS), help='game version to package (default: the one the import/data holds)')
    ap.add_argument('-o', '--out', type=Path, default=Path('.'), help=f'folder that gets "{APP} <version>/" (outside the repository)')
    ap.add_argument('--name', help=f'package folder name (default "{APP} <version>")')
    ap.add_argument('--data-dir', type=Path, help='keep the game data in this import output folder (holding extracted/) instead of <package>/data; without base/-k it only reuses it')
    ap.add_argument('--prebuilt', type=Path, help='folder with totk-win.exe and runtime-index.bin (or gen/runtime-index.bin) built for this version: copy instead of building')
    ap.add_argument('--translator', type=Path, default=TRANSLATOR_DEFAULT, help='folder with the published totk_xlate.dll')
    ap.add_argument('--translator-aot', type=Path, default=TRANSLATOR_AOT_DEFAULT, help='folder with the NativeAOT totk_xlate.exe (build_win.ps1 -Aot)')
    ap.add_argument('--romfs', choices=('full', 'index', 'none'), default='full', help='passed to import_game.py')
    ap.add_argument('--no-shortcut', action='store_true')
    ap.add_argument('--dry-run', action='store_true', help='print the plan only')
    a = ap.parse_args()

    out = a.out.expanduser().resolve()
    if out == REPO or REPO in out.parents:
        die('choose an output folder outside the repository (it will hold game data)')
    importing = bool(a.base)
    if importing and not a.keys:
        die('give the base NSP (and --update for 1.1.2/1.2.1/1.4.3) with -k prod.keys, or only --data-dir of an earlier import')
    if not importing and not a.data_dir:
        die('give the base NSP and -k, or --data-dir of an earlier import')
    if importing and not a.version:
        die('give --version (1.0.0 without --update, else the update\'s version) so the package folder can be named before the import')
    if not importing and not a.version:
        found = detect_version(a.data_dir.expanduser().resolve())
        if len(found) != 1:
            die(f'{a.data_dir} holds {", ".join(found) or "no supported version"}: pick one with --version')
        a.version = found[0]
    display = a.version
    vdir, tested = VERSIONS[display]
    if importing and (display == '1.0.0') != (a.update is None):
        die('1.0.0 is packaged from the base NSP alone; every other version needs its --update NSP')
    name = a.name or f'{APP} {display}'
    pkg = out / name
    data = a.data_dir.expanduser().resolve() if a.data_dir else pkg / 'data'
    vfolder = data / 'extracted' / vdir
    say(f'package {pkg} (game {display}, {vdir}); game data {data}')
    if not tested:
        say(f'warning: {display} has a patch table but has not been smoke-tested on Windows')
    if a.dry_run:
        return

    # (a) import
    if importing:
        cmd = [sys.executable, REPO / 'tools/windows/import_game.py', '--base', a.base, '--keys', a.keys, '--output', data,
               '--romfs', a.romfs]
        if a.update:
            cmd += ['--update', a.update]
        run(cmd)
    if not (vfolder / 'romfs' / 'romfs.bin').is_file():
        die(f'no {vfolder}\\romfs\\romfs.bin: the runtime needs the full RomFS of {display} (import with --romfs full)')
    if sha256(vfolder / 'exefs' / 'main') != main_digest(display):
        die(f'{vfolder}\\exefs\\main is not the {display} main module')

    # (b) runtime (per version: the patch tables are compiled in)
    prebuilt = REPO / f'prebuilt/windows-x64-{vdir}'
    src = a.prebuilt.resolve() if a.prebuilt else (prebuilt if (prebuilt / 'totk-win.exe').is_file() else None)
    if src is None:
        src = build_runtime(vdir, vfolder, REPO / 'build' / f'totk-win-x64-vk-{vdir}')
    exe = src / 'totk-win.exe'
    index = next((p for p in (src / 'runtime-index.bin', src / 'gen/runtime-index.bin') if p.is_file()), None)
    if not exe.is_file() or not index:
        die(f'{src} lacks totk-win.exe or runtime-index.bin')
    if not (a.translator / 'totk_xlate.dll').is_file():
        die(f'translator not found at {a.translator} (publish games/totk/tools/totk_xlate, or pass --translator)')

    # (c) layout
    (pkg / 'bin').mkdir(parents=True, exist_ok=True)
    for d in ('user-data', 'shader-cache', 'logs'):
        (pkg / d).mkdir(exist_ok=True)
    shutil.copy2(exe, pkg / 'bin/totk-win.exe')
    shutil.copy2(index, pkg / 'bin/runtime-index.bin')
    (pkg / 'bin/game-version.txt').write_text(f'{display} {vdir}\n')
    siblings = [p / 'bin/translator' for p in out.iterdir() if p.is_dir() and p != pkg and (p / 'bin/translator/totk_xlate.dll').is_file()]
    link_tree(a.translator, pkg / 'bin/translator', siblings[0] if siblings else None)
    aot = a.translator_aot / 'totk_xlate.exe'
    if aot.is_file():   # the native translator the game uses by default (TOTK_XLATE empty)
        (pkg / 'bin/xlate').mkdir(exist_ok=True)
        shutil.copy2(aot, pkg / 'bin/xlate/totk_xlate.exe')
    else:
        say(f'warning: no NativeAOT translator at {aot} (build_win.ps1 -Aot): the package needs the .NET 10 runtime')
    rel = '%~dp0data' if not a.data_dir else str(data)
    settings = pkg / 'settings.bat'
    if not settings.exists():   # keep the player's edits on a re-run
        write_bat(settings, SETTINGS.replace('@APP@', name).replace('@VER@', vdir).replace('@DISPLAY@', display).replace('@DATA@', rel))
    elif (pkg / 'bin/xlate/totk_xlate.exe').is_file():   # older packages: dotnet.exe was the only translator host
        old = settings.read_text(encoding='utf-8', errors='replace').replace('\r\n', '\n')
        old = old.replace('rem Shader translator host (needs the .NET 10 runtime: https://dotnet.microsoft.com/download/dotnet/10.0).\nset "TOTK_XLATE=dotnet.exe"\n',
                          SETTINGS[SETTINGS.index('rem Shader translator:'):])
        write_bat(settings, old)
    launcher = pkg / f'{name}.bat'
    write_bat(launcher, LAUNCHER.replace('@VER@', vdir).replace('@DISPLAY@', display))
    if not a.no_shortcut and os.name == 'nt':
        ps = (f"$s=(New-Object -ComObject WScript.Shell).CreateShortcut('{pkg / (name + '.lnk')}');"
              f"$s.TargetPath='{launcher}';$s.WorkingDirectory='{pkg}';$s.WindowStyle=7;$s.Save()")
        subprocess.call(['powershell', '-NoProfile', '-Command', ps])
    build_launcher(pkg / f'{APP}.exe')
    other = [p / 'support' for p in out.iterdir() if p.is_dir() and p != pkg and (p / 'support/python/python.exe').is_file()]
    build_support(pkg, other[0] if other else None)
    if not a.no_shortcut and os.name == 'nt':   # "<name> Settings.lnk": the launcher with --setup (game files, settings, saves)
        ps = (f"$s=(New-Object -ComObject WScript.Shell).CreateShortcut('{pkg / (name + ' Settings.lnk')}');"
              f"$s.TargetPath='{pkg / (APP + '.exe')}';$s.Arguments='--setup';$s.WorkingDirectory='{pkg}';$s.Save()")
        subprocess.call(['powershell', '-NoProfile', '-Command', ps])
    say(f'done. Launch "{pkg / (APP + ".exe")}" (or "{launcher}" / the .lnk). Log: {pkg / "logs/totk.log"}')


if __name__ == '__main__':
    main()
