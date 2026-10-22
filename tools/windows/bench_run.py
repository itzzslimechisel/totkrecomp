#!/usr/bin/env python3
"""Scripted run of a Windows package for performance / smoke tests (diagnostics; not shipped in packages).

Starts <package>\\bin\\totk-win.exe the way "Tears of the Kingdom <ver>.bat" does, on a COPY of the package's user-data
(the player's save is never written), with per-frame timing (DM_NVN_TSV), optional auto-load of the newest save
(TOTK_AUTOLOAD=1), screenshots at given seconds and a clean close at the end (TOTK_INPUT_FILE). Afterwards prints
average FPS / 1% low per time window from the frame times, and the log's error lines.

  python tools/windows/bench_run.py --pkg "C:\\...\\Tears of the Kingdom 1.4.3" --seconds 240 --out out\\run1 \
      --autoload --shots 120,200 --window 60:120,120:240 [--exe build\\...\\totk-win.exe] [--env K=V ...]
      [--keys "150:38:200,160:13:3000"]   (seconds:Mac keycode:ms; W=13 forward, J=38 A, Return=36 Plus)
"""
import argparse, json, os, shutil, subprocess, sys, threading, time
from pathlib import Path


def read_settings(pkg: Path) -> dict:
    env = {}
    for line in (pkg / 'settings.bat').read_text(encoding='utf-8', errors='replace').splitlines():
        line = line.strip()
        if line.lower().startswith('set "') and '=' in line:
            k, v = line[5:].rstrip('"').split('=', 1)
            env[k] = v
    return env


def stats(frames_ms):
    if not frames_ms:
        return None
    s = sorted(frames_ms)
    n = len(s)
    total = sum(s)
    worst = s[-max(1, n // 100):]           # the slowest 1 % of frames
    return {'frames': n, 'avg_fps': round(1000.0 * n / total, 2), 'low1_fps': round(1000.0 / (sum(worst) / len(worst)), 2),
            'p50_ms': round(s[n // 2], 2), 'p99_ms': round(s[min(n - 1, n * 99 // 100)], 2), 'max_ms': round(s[-1], 2),
            'over50ms': sum(1 for x in s if x > 50)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--pkg', required=True)
    ap.add_argument('--exe')
    ap.add_argument('--seconds', type=float, default=180)
    ap.add_argument('--out', required=True)
    ap.add_argument('--autoload', action='store_true')
    ap.add_argument('--fresh-profile', action='store_true', help='start without any save (new game)')
    ap.add_argument('--keep-profile', help='use this profile folder as is (save tests across runs)')
    ap.add_argument('--shots', default='')
    ap.add_argument('--keys', default='', help='sec:keycode:ms,... pressed through the input file')
    ap.add_argument('--window', default='', help='a:b,... seconds after the first frame to report separately')
    ap.add_argument('--cmds', default='', help='sec:command;... raw TOTK_INPUT_FILE commands (e.g. "50:sdlkey 58;52:oshot out.png")')
    ap.add_argument('--env', action='append', default=[])
    ap.add_argument('--shader-cache', help='shader cache folder (default: the package one)')
    a = ap.parse_args()

    pkg = Path(a.pkg).resolve(); out = Path(a.out).resolve(); out.mkdir(parents=True, exist_ok=True)
    st = read_settings(pkg)
    ver, vdir = (pkg / 'bin' / 'game-version.txt').read_text().split()
    data = Path(st['TOTK_DATA']) / 'extracted' / vdir
    exe = Path(a.exe).resolve() if a.exe else pkg / 'bin' / 'totk-win.exe'
    if a.keep_profile:
        profile = Path(a.keep_profile).resolve(); profile.mkdir(parents=True, exist_ok=True)
    else:
        profile = out / 'profile'
        if profile.exists():
            shutil.rmtree(profile)
        if a.fresh_profile or not (pkg / 'user-data').exists():
            profile.mkdir()
        else:
            shutil.copytree(pkg / 'user-data', profile, ignore=shutil.ignore_patterns('save-backups'))
    cmds = out / 'input.txt'; cmds.write_text('')
    tsv = out / 'frames.tsv'; log = out / 'totk.log'
    xlate, aot = st.get('TOTK_XLATE', ''), pkg / 'bin' / 'xlate' / 'totk_xlate.exe'
    xlate_cmd = f'"{aot}"' if not xlate and aot.is_file() else f'"{xlate or "dotnet.exe"}" "{pkg / "bin" / "translator" / "totk_xlate.dll"}"'
    env = os.environ.copy()
    env.update({k: v for k, v in st.items() if k.startswith('TOTK_')})
    env.update({'TOTK_ROMFS': str(data / 'romfs'), 'TOTK_ROMFS_INDEX': str(pkg / 'bin' / 'runtime-index.bin'),
                'TOTK_PROFILE_DIR': str(profile), 'TOTK_SHADER_CACHE': a.shader_cache or str(pkg / 'shader-cache'),
                'TOTK_XLATE_CMD': xlate_cmd,
                'DM_NVN_TSV': str(tsv), 'DM_NVN_TSV_CALLS': '0', 'TOTK_INPUT_FILE': str(cmds)})
    if st.get('TOTK_AUDIO_OFF') == '0':
        env.pop('TOTK_AUDIO_OFF', None)
    if a.autoload:
        env['TOTK_AUTOLOAD'] = '1'
    env.setdefault('TOTK_ASSUME_FOCUS', '1')   # scripted runs do not take the keyboard from whatever the user is doing
    for kv in a.env:
        k, v = kv.split('=', 1); env[k] = v

    events = []
    for s in filter(None, a.shots.split(',')):
        events.append((float(s), f'shot {out / ("shot_%03d.png" % int(float(s)))}'))
    for k in filter(None, a.keys.split(',')):
        t, code, ms = k.split(':')
        events.append((float(t), f'key {code} {ms}' if int(ms) <= 5000 else f'down {code}'))
        if int(ms) > 5000:
            events.append((float(t) + int(ms) / 1000, f'up {code}'))
    for c in filter(None, a.cmds.split(';')):
        t, cmd = c.split(':', 1)
        events.append((float(t), cmd.strip()))
    events.sort()

    t0 = time.time()
    with open(log, 'wb') as lf:
        p = subprocess.Popen([str(exe), str(data / 'exefs'), 'boot'], cwd=str(pkg), env=env, stderr=lf, stdout=subprocess.DEVNULL)

        def feeder():
            for t, c in events:
                while time.time() - t0 < t:
                    if p.poll() is not None:
                        return
                    time.sleep(0.2)
                with open(cmds, 'a', newline='\n') as f:
                    f.write(c + '\n')
            while time.time() - t0 < a.seconds:
                if p.poll() is not None:
                    return
                time.sleep(0.5)
            with open(cmds, 'a', newline='\n') as f:
                f.write('close\n')
        th = threading.Thread(target=feeder, daemon=True); th.start()
        try:
            rc = p.wait(timeout=a.seconds + 120)
        except subprocess.TimeoutExpired:
            p.kill(); rc = 'killed (no exit after close)'
    wall = time.time() - t0

    frames, other = [], {}
    if tsv.exists():
        for line in tsv.read_text(encoding='utf-8', errors='replace').splitlines():
            parts = line.split('\t')
            if len(parts) == 3 and parts[1] in ('@frame_ms', '@sub_wall_ms', '@nvn_ms', '@draws'):
                try:
                    v = float(parts[2])
                except ValueError:
                    continue
                if parts[1] == '@frame_ms':
                    frames.append(v)
                else:
                    other.setdefault(parts[1], []).append(v)
    res = {'pkg': str(pkg), 'exe': str(exe), 'rc': rc, 'wall_s': round(wall, 1), 'all': stats(frames)}
    for k, v in other.items():   # medians over the last half of the run (submission replay, NVN submit + present, draws per frame)
        v = sorted(v[len(v) // 2:])
        if v:
            res['median_' + k[1:]] = round(v[len(v) // 2], 2)
    # time windows measured from the first frame (cumulative frame times)
    cum, times = 0.0, []
    for f in frames:
        cum += f; times.append(cum / 1000.0)
    for w in filter(None, a.window.split(',')):
        lo, hi = map(float, w.split(':'))
        res[f'{lo:g}-{hi:g}s'] = stats([f for f, t in zip(frames, times) if lo <= t < hi])
    text = log.read_text(encoding='utf-8', errors='replace') if log.exists() else ''
    bad = [l for l in text.splitlines() if any(k in l for k in ('ã€çº¢ã€‘', 'fatal', 'FATAL', 'guest exception', 'data abort', 'failed:', 'runtime_fail', 'Assertion'))]
    res['error_lines'] = bad[:30]
    res['underruns'] = [l for l in text.splitlines() if 'underruns' in l][-1:] if text else []
    (out / 'result.json').write_text(json.dumps(res, indent=1, ensure_ascii=False), encoding='utf-8')
    print(json.dumps(res, indent=1, ensure_ascii=False))


if __name__ == '__main__':
    main()
