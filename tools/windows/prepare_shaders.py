#!/usr/bin/env python3
"""Prepare shaders (Windows / Vulkan): translate the shared shader list from the player's own game files into the
package's shader cache before play, so the game does not have to translate (or skip draws) while it runs.

The repository ships only identifiers (games/totk/assets/shader_list.tsv, docs/shader-list.md): a shader key (SHA-256 of
the NVN control section and code) and the bind state ("deps") a variant was made for. This tool finds those shaders in
the selected, verified RomFS (games/totk/scripts/prepare_shader_seed.py: Shader/ packages, BNSH files, agl packages and
the list's locator rows), translates each listed bind state plus the default state of every startup shader with the
SPIR-V translator (totk_xlate --service --target spirv), one translator process per core, and records every variant in
<cache>/<key>.variants.tsv exactly as the runtime does (third_party/d2r_gfx/gfx/nvn/nvn_vk_rt.c). It is resumable
(variants already made are skipped) and prints "@@progress <0..1>" lines for the launcher.

  python tools/windows/prepare_shaders.py --pkg "D:\\Games\\Tears of the Kingdom 1.4.3"
  python tools/windows/prepare_shaders.py --romfs <extracted/v143/romfs> --cache <dir> --translator <totk_xlate.exe> --caps <vk-caps.json>
"""
import argparse, json, os, pickle, struct, subprocess, sys, threading, time, types
from pathlib import Path

HERE = Path(__file__).resolve()
for root in [HERE.parents[2], HERE.parent]:   # repo checkout (tools/windows/) or a package's support/ folder
    if (root / 'games/totk/scripts/prepare_shader_seed.py').is_file():
        sys.path.insert(0, str(root / 'games/totk/scripts'))
        LIST = root / 'games/totk/assets/shader_list.tsv'
        break
else:
    sys.exit('prepare_shader_seed.py not found next to this script')


def zstd_shim():
    """prepare_shader_seed.py reads Zstandard through Python 3.14's compression.zstd; older Pythons use the zstandard package."""
    try:
        import compression.zstd   # noqa: F401
        return
    except ImportError:
        pass
    import zstandard
    mod = types.ModuleType('compression.zstd')

    class ZstdDict:
        def __init__(self, raw):
            self.d = zstandard.ZstdCompressionDict(raw)
            self.dict_id = self.d.dict_id()

    def get_frame_info(raw):
        p = zstandard.get_frame_parameters(raw)
        size = p.content_size if p.content_size not in (zstandard.CONTENTSIZE_UNKNOWN, zstandard.CONTENTSIZE_ERROR) else None
        return types.SimpleNamespace(decompressed_size=size, dictionary_id=p.dict_id)

    def decompress(raw, zstd_dict=None):
        size = get_frame_info(raw).decompressed_size
        dctx = zstandard.ZstdDecompressor(dict_data=zstd_dict.d) if zstd_dict else zstandard.ZstdDecompressor()
        return dctx.decompress(raw, max_output_size=size or (512 << 20))

    mod.ZstdDict, mod.get_frame_info, mod.decompress = ZstdDict, get_frame_info, decompress
    pkg = types.ModuleType('compression'); pkg.zstd = mod
    sys.modules['compression'] = pkg; sys.modules['compression.zstd'] = mod


zstd_shim()
import prepare_shader_seed as seed   # noqa: E402
from shader_list import ShaderList   # noqa: E402

DMW = 1   # the game sets depth mode "near = -w" (runtime log / nvn_vk_rt.c index lines): the variants are made for it


def canon(obj):
    return json.dumps(obj, sort_keys=True, separators=(',', ':'))


def say(msg):
    print(msg, flush=True)


def read_settings(pkg):
    env = {}
    for line in (pkg / 'settings.bat').read_text(encoding='utf-8', errors='replace').splitlines():
        line = line.strip()
        if line.lower().startswith('set "') and '=' in line:
            k, v = line[5:].rstrip('"').split('=', 1)
            env[k] = v.replace('%~dp0', str(pkg) + os.sep)
    return env


def package_args(pkg):
    st = read_settings(pkg)
    ver, vdir = (pkg / 'bin/game-version.txt').read_text().split()
    romfs = Path(st['TOTK_DATA']) / 'extracted' / vdir / 'romfs'
    cache = pkg / 'shader-cache'
    aot = pkg / 'bin/xlate/totk_xlate.exe'
    custom = st.get('TOTK_XLATE', '')
    if custom:
        translator = [custom, str(pkg / 'bin/translator/totk_xlate.dll')] if custom.lower().startswith('dotnet') else [custom]
    elif aot.is_file():
        translator = [str(aot)]
    else:
        translator = ['dotnet.exe', str(pkg / 'bin/translator/totk_xlate.dll')]
    caps = cache / 'vk-caps.json'
    if not caps.is_file():   # the GPU's translator capabilities, as the runtime reports them
        cache.mkdir(parents=True, exist_ok=True)
        subprocess.run([str(pkg / 'bin/totk-win.exe'), '--vk-caps', str(cache)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=120)
    return romfs, cache, translator, caps, ver


def load_index(cache):
    """key -> {canonical deps: variant} from <cache>/index.tsv (one file; lines: key, variant, depth_minus_w, deps), or
    from the per-key <cache>/<key>.variants.tsv files (lines: variant, depth_minus_w, deps) when an older cache has no
    index.tsv. Reading 20 000 small files took a minute or more even with nothing left to translate."""
    idx = {}
    combined = cache / 'index.tsv'
    if combined.is_file():
        for line in combined.read_text(encoding='utf-8', errors='replace').splitlines():
            parts = line.split('\t')
            if len(parts) != 4 or len(parts[0]) != 64 or parts[2] != str(DMW):
                continue
            try:
                idx.setdefault(parts[0], {})[canon(json.loads(parts[3]))] = parts[1]
            except ValueError:
                pass
        return idx
    for p in cache.glob('*.variants.tsv'):
        key = p.name[:64]
        lines = p.read_text(encoding='utf-8', errors='replace').splitlines()
        keep = []
        for line in lines:
            parts = line.split('\t')
            if len(parts) != 3 or not (cache / (parts[0] + '_v.spv')).is_file() or not (cache / (parts[0] + '_v.json')).is_file():
                continue   # a variant whose files are gone: dropped from the index (and made again)
            keep.append(line)
            if parts[1] == str(DMW):
                try:
                    idx.setdefault(key, {})[canon(json.loads(parts[2]))] = parts[0]
                except ValueError:
                    pass
        if len(keep) != len(lines):
            with open(p, 'w', encoding='utf-8', newline='\n') as f:
                f.write(''.join(l + '\n' for l in keep))
    return idx


def write_combined_index(cache, index):
    """<cache>/index.tsv: every variant in one file (key, variant, depth_minus_w, deps), which the runtime reads at start
    instead of opening each .variants.tsv (nvn_vk_rt.c nvn_vk_shader_index_load)."""
    tmp = cache / 'index.tsv.tmp'
    with open(tmp, 'w', encoding='utf-8', newline='\n') as f:
        for key in sorted(index):
            for deps, variant in index[key].items():
                f.write(f'{key}\t{variant}\t{DMW}\t{deps}\n')
    os.replace(tmp, cache / 'index.tsv')


def inventory(romfs, cache, wanted_keys, locators):
    """(stage, control, code) of every shader the list needs; kept in <cache>/prepare-inventory.bin (local game data, never
    shipped) so that a resumed run does not scan the RomFS again."""
    inv_path = cache / 'prepare-inventory.bin'
    sha = json.loads((romfs / 'index.json').read_text(encoding='utf-8'))['romfs_sha256']
    if inv_path.is_file():
        try:
            saved = pickle.loads(inv_path.read_bytes())
            if saved.get('romfs_sha256') == sha and saved.get('list') == len(wanted_keys):
                say(f'shader inventory: {len(saved["shaders"])} shaders (saved)')
                return saved['shaders'], saved['startup']
        except Exception:
            pass
    say('finding the game\'s shaders in your game files (a few minutes) ...')
    _, shaders, startup, _ = seed.inventory(romfs, None, 'totk')
    extra = seed.located({w for w in locators if w[0] not in shaders}, seed.source_reader(romfs, None, 'totk'), 'totk')
    shaders.update(extra)
    keep = {k: v[:3] for k, v in shaders.items() if k in wanted_keys or k in startup}
    inv_path.write_bytes(pickle.dumps(dict(romfs_sha256=sha, list=len(wanted_keys), shaders=keep, startup=sorted(startup))))
    say(f'found {len(shaders)} shaders ({len(extra)} through locator rows)')
    return keep, sorted(startup)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--pkg', type=Path, help='a Windows package folder (reads its settings.bat, translator and shader-cache)')
    ap.add_argument('--romfs', type=Path); ap.add_argument('--cache', type=Path); ap.add_argument('--caps', type=Path)
    ap.add_argument('--translator', nargs='+', help='translator command (totk_xlate.exe, or dotnet.exe totk_xlate.dll)')
    ap.add_argument('--list', type=Path, default=None, help='shader list (default: games/totk/assets/shader_list.tsv)')
    ap.add_argument('--workers', type=int, default=0, help='translator processes (default: cores - 1, at most 8)')
    ap.add_argument('--limit', type=int, default=0, help='translate at most this many variants (tests)')
    a = ap.parse_args()
    if a.pkg:
        romfs, cache, translator, caps, ver = package_args(a.pkg.resolve())
    else:
        if not (a.romfs and a.cache and a.translator and a.caps):
            ap.error('give --pkg, or --romfs, --cache, --translator and --caps')
        romfs, cache, translator, caps, ver = a.romfs, a.cache, a.translator, a.caps, '?'
    if not (romfs / 'romfs.bin').is_file():
        say(f'@@error game data not found: {romfs}\\romfs.bin'); return 1
    if not caps.is_file():
        say(f'@@error no GPU capabilities ({caps}): start the game once, or check the Vulkan driver'); return 1
    caps_obj = json.loads(caps.read_text(encoding='utf-8'))
    cache.mkdir(parents=True, exist_ok=True)
    shared = ShaderList.load(a.list or LIST)
    listed = sorted(shared.variants)   # (key, deps json)
    wanted = {k for k, _ in listed}
    t0 = time.monotonic()
    say('@@progress 0.01')
    shaders, startup = inventory(romfs, cache, wanted, shared.locators)
    index = load_index(cache)
    done_path = cache / 'prepare-done.txt'
    done = set(done_path.read_text().split()) if done_path.is_file() else set()
    jobs = []
    for key in startup:
        if key in shaders:
            jobs.append((key, dict(seed.ANSWERS)))
    for key, deps in listed:
        if key in shaders:
            d = json.loads(deps)
            if canon(d) in index.get(key, {}):
                continue   # this bind state is there already
            jobs.append((key, {**seed.ANSWERS, **d}))
    jobs = list({f'{k}-{seed.suffix(ans)}': (k, ans, f'{k}-{seed.suffix(ans)}') for k, ans in jobs}.values())   # one job per variant name
    made = {v for have in index.values() for v in have.values()}
    jobs = [j for j in jobs if j[2] not in made and j[2] not in done]
    if a.limit:
        jobs = jobs[:a.limit]
    total = len(jobs)
    say(f'game {ver}: {len(shaders)} listed or startup shaders found in your files; {total} variants to translate')
    marker = cache / 'prepared.json'   # the launcher shows the shaders as prepared when this names the current list
    list_sha = __import__('hashlib').sha256(Path(a.list or LIST).read_bytes()).hexdigest()
    if not total:
        write_combined_index(cache, index)
        marker.write_text(json.dumps(dict(list_sha256=list_sha, version=ver)))
        say('@@progress 1'); say('@@done Shaders are prepared.'); return 0
    by_key = {}
    for j in jobs:
        by_key.setdefault(j[0], []).append(j)
    pending = iter(sorted(by_key.items()))
    lock = threading.Lock()
    counts = [0, 0]
    errors = []
    done_f = open(done_path, 'a', encoding='utf-8')

    def worker():
        proc = subprocess.Popen(translator + ['--service', '--target', 'spirv', '--out', str(cache), '--chain', 'vk1'],
                                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True,
                                creationflags=getattr(subprocess, 'BELOW_NORMAL_PRIORITY_CLASS', 0) | getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        try:
            while True:
                with lock:
                    item = next(pending, None)
                if item is None:
                    return
                key, mine = item
                stage, control, code = shaders[key]
                for _, answers, variant in mine:
                    c1, length = struct.unpack_from('<II', control, 0x700)
                    code_path = cache / (variant + '.code')
                    code_path.write_bytes(code[0 if stage == 'cs' else 0x30:length])
                    req = dict(variant=variant, stage=stage, code=str(code_path).replace('\\', '/'), c1=code[c1:length].hex(),
                               depth_minus_w=bool(DMW), caps=caps_obj, **answers)
                    if stage == 'cs':
                        req['local'] = list(struct.unpack_from('<III', control, 0x748))
                    proc.stdin.write(json.dumps(req) + '\n'); proc.stdin.flush()
                    line = proc.stdout.readline()
                    code_path.unlink(missing_ok=True)
                    if not line:
                        raise RuntimeError('the shader translator stopped')
                    ok = json.loads(line).get('ok')
                    with lock:
                        if ok:
                            try:
                                refl = json.loads((cache / (variant + '_v.json')).read_text(encoding='utf-8', errors='replace'))
                            except (OSError, ValueError) as e:
                                refl = None; errors.append(f'{variant}: reflection {e}')
                            if refl is None:
                                counts[1] += 1; done_f.write(variant + '\n'); continue
                            deps = canon(refl['deps'])
                            have = index.setdefault(key, {})
                            if deps in have and have[deps] != variant:   # same deps as a variant made before: keep one
                                for ext in ('_v.json', '_v.spv', '_v.res.json'):
                                    (cache / (variant + ext)).unlink(missing_ok=True)
                            elif deps not in have:
                                have[deps] = variant
                                with open(cache / f'{key}.variants.tsv', 'a', encoding='utf-8', newline='\n') as f:
                                    f.write(f'{variant}\t{DMW}\t{deps}\n')
                            counts[0] += 1
                        else:
                            counts[1] += 1
                            errors.append(f'{variant}: {json.loads(line).get("error")}')
                        if not ok or variant not in index.get(key, {}).values():   # failed / duplicate deps: not tried again
                            done_f.write(variant + '\n'); done_f.flush()
                        n = counts[0] + counts[1]
                        if n % 50 == 0 or n == total:
                            el = time.monotonic() - t0
                            say(f'@@progress {0.02 + 0.98 * n / total:.4f}')
                            say(f'prepared {n}/{total} shader variants ({counts[1]} failed), {el:.0f} s, about {el / n * (total - n) / 60:.0f} min left')
        finally:
            try:
                proc.stdin.close(); proc.wait(timeout=30)
            except Exception:
                proc.kill()

    n_workers = a.workers or max(1, min(8, (os.cpu_count() or 4) - 1))
    say(f'translating with {n_workers} translator processes')
    threads = [threading.Thread(target=worker, daemon=True) for _ in range(n_workers)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    done_f.close()
    for e in errors[:5]:
        say('  failed: ' + e[:300])
    write_combined_index(cache, index)
    marker.write_text(json.dumps(dict(list_sha256=list_sha, version=ver, translated=counts[0], failed=counts[1])))
    say('@@progress 1')
    say(f'@@done Shaders are prepared: {counts[0]} translated, {counts[1]} could not be translated, {time.monotonic() - t0:.0f} s.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
