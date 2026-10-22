#!/usr/bin/env python3
"""Helper the launcher ('Tears of the Kingdom.exe') runs for the long jobs: game import, folder adoption, save import/export.

  totk_setup.py detect  --base X.nsp [--update U.nsp] --keys prod.keys --output DATA   identify the game version (ExeFS module hashes)
  totk_setup.py import  --base X.nsp [--update U.nsp] --keys prod.keys --output DATA   verify + extract everything (tools/windows/import_game.py)
  totk_setup.py adopt   --exefs DIR --romfs DIR --output DATA                          use an already extracted exefs + romfs folder pair
  totk_setup.py save-import --src FOLDER_OR_ZIP --profile user-data                    back up the current save, then import
  totk_setup.py save-export --dest FILE.zip --profile user-data
  totk_setup.py save-path --profile user-data                                          print the folder the runtime reads saves from

Machine-readable lines on stdout: "@@progress 0.4", "@@version <dir> <display>", "@@data <path>", "@@error <text>", "@@done <text>".
Everything else is log text. Python standard library plus (import only) cryptography and lz4. Exit code 0 = success.
"""
import argparse
import warnings
import hashlib
import json
import os
import re
import secrets
import shutil
import sys
import time
import zipfile
from pathlib import Path

warnings.filterwarnings('ignore')
HERE = Path(__file__).resolve().parent                    # <package>/support  (also the "REPO" of the bundled import_game.py)
sys.path[:0] = [str(HERE), str(HERE / 'tools/windows'), str(HERE / 'games/totk/scripts')]
DIRS = {'1.0.0': 'v0', '1.1.1': 'v111', '1.1.2': 'v112', '1.2.0': 'v120', '1.2.1': 'v121', '1.4.3': 'v143'}
SUPPORTED = 'Tears of the Kingdom 1.0.0, 1.1.1, 1.2.0 and 1.4.3 (1.1.2 and 1.2.1 have untested tables)'


def out(msg):
    print(msg, flush=True)


def fail(msg):
    out(f'@@error {msg}')
    sys.exit(1)


def expected(display):
    return json.loads((HERE / f'games/totk/assets/expected-v{display}.json').read_text())


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(8 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def identify_exefs(exefs):
    """display version whose expected ExeFS 'main' hash matches exefs/main, else None"""
    main = Path(exefs) / 'main'
    if not main.is_file():
        return None
    digest = sha256_file(main)
    for display in DIRS:
        try:
            if expected(display)['exefs_sha256']['main'] == digest:
                return display
        except (OSError, KeyError, ValueError):
            pass
    return None


def unsupported(extra=''):
    fail(f'Unsupported game version{extra}. Supported: {SUPPORTED}. Other updates (for example 1.4.2, 1.3.x or 1.1.0) have no patch table and cannot run.')


# ---- import ----
def cmd_import(a, detect_only=False):
    sys.argv[0] = str(HERE / 'tools/windows/import_game.py')
    try:
        import import_game as ig                      # bundled tools/windows/import_game.py
    except ModuleNotFoundError as e:
        fail(f'missing Python package {e.name} (the bundled Python is incomplete)')
    out(f'@@progress 0.02')
    argv = ['--base', str(a.base), '--keys', str(a.keys), '--output', str(a.output)]
    if a.update:
        argv += ['--update', str(a.update)]
    if not detect_only:
        argv += ['--romfs', 'full']
    try:
        if detect_only:
            ig_out = Path(a.output).expanduser().resolve()
            ig_out.mkdir(parents=True, exist_ok=True)
            keyset = ig.load_keys(Path(a.keys))
            src = Path(a.update or a.base)
            pkg = ig.Package(src, keyset)
            try:
                title, meta = pkg.meta_version()
            finally:
                pkg.close()
            try:
                ver = ig.step_exefs(ig_out, src, Path(a.keys), update=bool(a.update))
            except Exception as e:
                if 'unsupported module hashes' in str(e):
                    unsupported(f' (this {"update" if a.update else "game"} has content version {meta}: it is not one of the supported ones)')
                raise
            display = ig.VERSIONS[ver]['display_version']
            out(f'@@version {ver} {display}')
            return
        watch(Path(a.output), 0.12 if a.update is None else 0.12)
        ig.main(argv)
    except SystemExit:
        raise
    except Exception as e:                            # BuildError and friends
        msg = str(e)
        if 'unsupported module hashes' in msg or 'meta says version' in msg:
            unsupported()
        fail(msg.strip().splitlines()[0][:400] if msg.strip() else type(e).__name__)


def watch(output, start):
    """progress of the long RomFS write: size of extracted/*/romfs/romfs.bin.partial against the expected ~17 GB"""
    import threading

    def run():
        last = -1
        while True:
            time.sleep(1.5)
            size = max((p.stat().st_size for p in output.glob('extracted/*/romfs/romfs.bin.partial') if p.exists()), default=0)
            if size:
                f = round(start + (0.97 - start) * min(size / 17.1e9, 1), 3)
                if f != last:
                    last = f
                    out(f'@@progress {f}')
    threading.Thread(target=run, daemon=True).start()


# ---- adopt an extracted exefs + romfs pair ----
def cmd_adopt(a):
    exefs, romfs = Path(a.exefs).resolve(), Path(a.romfs).resolve()
    display = identify_exefs(exefs)
    if not display:
        if not (exefs / 'main').is_file():
            fail(f'{exefs} has no "main" file: choose the ExeFS folder (main, main.npdm, rtld, sdk, subsdk0).')
        unsupported(' (the ExeFS "main" module matches none of the supported versions)')
    vdir = DIRS[display]
    image = romfs / 'romfs.bin'
    if not image.is_file():
        fail(f'{romfs} has no romfs.bin. The runtime reads the single RomFS image written by this launcher\'s NSP/XCI import; a folder of loose RomFS files is not supported. Use the NSP/XCI import instead.')
    want = expected(display)['romfs']['size']
    if image.stat().st_size != want:
        fail(f'romfs.bin is {image.stat().st_size} bytes but game {display} needs {want}: it belongs to another game version or is incomplete.')
    out(f'Game version {display} (ExeFS module hash matches)')
    if a.probe:
        out(f'@@version {vdir} {display}')
        return
    if exefs.parent == romfs.parent and exefs.name == 'exefs' and romfs.name == 'romfs' and exefs.parent.name == vdir and exefs.parent.parent.name == 'extracted':
        data = exefs.parent.parent.parent
        out(f'Using the folders where they are: {data}')
    else:
        data = Path(a.output).resolve()
        dest = data / 'extracted' / vdir
        (dest / 'exefs').mkdir(parents=True, exist_ok=True)
        (dest / 'romfs').mkdir(parents=True, exist_ok=True)
        for f in exefs.iterdir():
            if f.is_file():
                shutil.copy2(f, dest / 'exefs' / f.name)
        out('@@progress 0.1')
        target = dest / 'romfs' / 'romfs.bin'
        if target.exists():
            target.unlink()
        try:
            os.link(image, target)
            out('romfs.bin linked (no extra disk space)')
        except OSError:
            out(f'copying romfs.bin ({want / 2**30:.1f} GiB)')
            total, done = want, 0
            with open(image, 'rb') as s, open(target, 'wb') as d:
                while True:
                    b = s.read(16 << 20)
                    if not b:
                        break
                    d.write(b)
                    done += len(b)
                    out(f'@@progress {0.1 + 0.89 * done / total:.3f}')
    out(f'@@version {vdir} {display}')
    out(f'@@data {data}')
    out(f'@@done Game files ready: {display}')


# ---- saves ----
def save_root(profile, create=True):
    profile = Path(profile)
    profile.mkdir(parents=True, exist_ok=True)
    uid = profile / 'account.uid'
    if not uid.is_file() or uid.stat().st_size != 16:
        if not create:
            return None
        uid.write_bytes(secrets.token_bytes(16))
    raw = uid.read_bytes()
    lo, hi = int.from_bytes(raw[:8], 'little'), int.from_bytes(raw[8:], 'little')
    root = profile / 'save' / f'{hi:016x}{lo:016x}'          # runtime/fs_host.c init()
    if create:
        root.mkdir(parents=True, exist_ok=True)
    return root


SLOT = re.compile(r'^slot_\d+$', re.I)


def looks_like_save(d):
    try:
        names = {p.name.lower(): p for p in d.iterdir()}
    except OSError:
        return False
    return any(SLOT.match(n) and p.is_dir() for n, p in names.items()) or \
        any(n in names for n in ('progress.sav', 'caption.sav', 'option.sav')) or ('album' in names and names['album'].is_dir())


def find_save(src, depth=7):
    """the shallowest folder under src (breadth first) that has the Switch save layout: slot_NN/, album/, *.sav"""
    level = [Path(src)]
    for _ in range(depth):
        nxt = []
        for d in level:
            if looks_like_save(d):
                return d
            try:
                nxt += sorted(p for p in d.iterdir() if p.is_dir())
            except OSError:
                pass
        level = nxt
    return None


def backup(root, profile):
    files = [p for p in root.rglob('*') if p.is_file()]
    if not files:
        return None
    dest = Path(profile) / 'save-backups'
    dest.mkdir(exist_ok=True)
    z = dest / time.strftime('save-%Y%m%d-%H%M%S.zip')
    k = 1
    while z.exists():
        k += 1
        z = dest / time.strftime(f'save-%Y%m%d-%H%M%S-{k}.zip')
    with zipfile.ZipFile(z, 'w', zipfile.ZIP_DEFLATED) as zf:
        for p in files:
            zf.write(p, p.relative_to(root).as_posix())
    return z


def cmd_save_import(a):
    src = Path(a.src)
    tmp = None
    if src.is_file():
        if not zipfile.is_zipfile(src):
            fail('Choose a save folder or a .zip of one.')
        tmp = Path(a.profile) / 'import-tmp'
        shutil.rmtree(tmp, ignore_errors=True)
        with zipfile.ZipFile(src) as zf:
            for n in zf.namelist():                       # no path escapes
                if n.startswith('/') or '..' in Path(n).parts:
                    fail(f'unsafe path in the zip: {n}')
            zf.extractall(tmp)
        src = tmp
    found = find_save(src) if src.is_dir() else None
    if not found:
        shutil.rmtree(tmp, ignore_errors=True) if tmp else None
        fail('No Tears of the Kingdom save found there (expected a folder with slot_00\\progress.sav, album\\, ...). Pick the folder that holds slot_00 or a zip of it.')
    root = save_root(a.profile)
    out(f'Save data found in: {found}')
    b = backup(root, a.profile)
    out(f'Existing save backed up to {b}' if b else 'No existing save to back up')
    for p in list(root.iterdir()):                         # replace what a save consists of; leave other runtime files alone
        if SLOT.match(p.name) or p.name.lower() == 'album' or p.suffix.lower() == '.sav':
            shutil.rmtree(p) if p.is_dir() else p.unlink()
    n = 0
    for p in found.iterdir():
        if p.name.lower() == 'storage' and False:
            continue
        tgt = root / p.name
        if p.is_dir():
            shutil.copytree(p, tgt, dirs_exist_ok=True)
            n += sum(1 for q in p.rglob('*') if q.is_file())
        else:
            shutil.copy2(p, tgt)
            n += 1
    shutil.rmtree(tmp, ignore_errors=True) if tmp else None
    slots = sorted(p.name for p in root.iterdir() if SLOT.match(p.name))
    out(f'@@done Imported {n} files ({", ".join(slots) or "no slots"}) into {root}')


def cmd_save_export(a):
    root = save_root(a.profile, create=False)
    files = [p for p in root.rglob('*') if p.is_file()] if root and root.is_dir() else []
    if not files:
        fail('There is no save to export yet.')
    dest = Path(a.dest)
    dest.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(dest, 'w', zipfile.ZIP_DEFLATED) as zf:
        for p in files:
            zf.write(p, p.relative_to(root).as_posix())
    out(f'@@done Exported {len(files)} files to {dest}')


def cmd_save_path(a):
    out(f'@@data {save_root(a.profile)}')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    for name in ('detect', 'import'):
        p = sub.add_parser(name)
        p.add_argument('--base', required=True, type=Path)
        p.add_argument('--update', type=Path)
        p.add_argument('--keys', required=True, type=Path)
        p.add_argument('--output', required=True, type=Path)
    p = sub.add_parser('adopt')
    p.add_argument('--exefs', required=True)
    p.add_argument('--romfs', required=True)
    p.add_argument('--output', required=True)
    p.add_argument('--probe', action='store_true', help='only identify the version')
    p = sub.add_parser('save-import')
    p.add_argument('--src', required=True)
    p.add_argument('--profile', required=True)
    p = sub.add_parser('save-export')
    p.add_argument('--dest', required=True)
    p.add_argument('--profile', required=True)
    p = sub.add_parser('save-path')
    p.add_argument('--profile', required=True)
    p = sub.add_parser('shaders', help='prepare shaders: translate the shared shader list from the game files (prepare_shaders.py)')
    p.add_argument('--pkg', required=True)
    a = ap.parse_args()
    try:
        if a.cmd in ('detect', 'import'):
            for f in (a.base, a.keys, a.update):
                if f and not Path(f).is_file():
                    fail(f'No such file: {f}')
            cmd_import(a, detect_only=a.cmd == 'detect')
            if a.cmd == 'import':
                out('@@progress 1')
                out('@@done Import finished')
        elif a.cmd == 'shaders':
            sys.path.insert(0, str(Path(__file__).resolve().parent))
            import prepare_shaders
            sys.argv = ['prepare_shaders.py', '--pkg', a.pkg]
            rc = prepare_shaders.main()
            if rc:
                sys.exit(rc)
        else:
            {'adopt': cmd_adopt, 'save-import': cmd_save_import, 'save-export': cmd_save_export, 'save-path': cmd_save_path}[a.cmd](a)
    except SystemExit:
        raise
    except Exception as e:
        fail(f'{type(e).__name__}: {e}')


if __name__ == '__main__':
    main()
