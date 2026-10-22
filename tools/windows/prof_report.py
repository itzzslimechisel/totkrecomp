#!/usr/bin/env python3
"""Summarize a TOTK_PROF sample file (runtime/win/prof_win.c): per thread, the functions with the most samples.

  python tools/windows/prof_report.py prof.txt --exe build/totk-win-x64-vk-v143/totk-win.exe [--top 25] [--threads 8]
Addresses inside totk-win.exe are symbolized with llvm-symbolizer (LLVM-MinGW); other modules are reported by name,
code outside any module as [jit] (the dynarmic code cache)."""
import argparse, collections, re, shutil, subprocess, sys


def image_base(exe):
    tool = shutil.which('llvm-objdump')
    out = subprocess.run([tool, '-p', exe], capture_output=True, text=True).stdout
    m = re.search(r'ImageBase\s+([0-9a-fA-F]+)', out)
    return int(m.group(1), 16) if m else 0x140000000


_exports = {}
def export_name(mod, rva):
    """Nearest export at or below rva of a system DLL (no PDB needed): enough to tell waits from work in ntdll."""
    import bisect, os
    if mod not in _exports:
        path = os.path.join(os.environ.get('SystemRoot', r'C:\Windows'), 'System32', mod)
        ex = []
        if os.path.exists(path):
            out = subprocess.run([shutil.which('llvm-readobj'), '--coff-exports', path], capture_output=True, text=True).stdout
            for m in re.finditer(r'Name: (\S*)\s+RVA: 0x([0-9A-Fa-f]+)', out):
                if m.group(1):
                    ex.append((int(m.group(2), 16), m.group(1)))
        ex.sort(); _exports[mod] = ex
    ex = _exports[mod]
    i = bisect.bisect_right(ex, (rva, '\uffff')) - 1
    return ex[i][1] if i >= 0 else '?'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('prof'); ap.add_argument('--exe', required=True)
    ap.add_argument('--top', type=int, default=25); ap.add_argument('--threads', type=int, default=8)
    ap.add_argument('--chains', help='print the most frequent call chains through this function (substring)')
    a = ap.parse_args()
    exe_name = a.exe.replace('\\', '/').split('/')[-1].lower()
    names, samples, stacks, mods = {}, collections.defaultdict(list), collections.defaultdict(list), []
    for line in open(a.prof, encoding='utf-8', errors='replace'):
        p = line.rstrip('\n').split('\t')
        if p[0] == 'T':
            names[p[1]] = (int(p[2]), p[3])
        elif p[0] == 'S':
            samples[p[1]].append((int(p[2], 16), int(p[3]), p[4], int(p[5], 16)))
        elif p[0] == 'M':
            mods.append((int(p[2], 16), int(p[2], 16) + int(p[3], 16), p[1]))
        elif p[0] == 'K':
            stacks[p[1]].append((int(p[2]), [int(x, 16) for x in p[3].split(',')]))
    pref = image_base(a.exe)
    rbase = next((base for v in samples.values() for _, _, mod, base in v if mod.lower() == exe_name), 0)
    addrs = {rip - base + pref for v in samples.values() for rip, _, mod, base in v if mod.lower() == exe_name}
    def modname(x):
        for lo, hi, nm in mods:
            if lo <= x < hi:
                return nm, lo
        return None, 0
    def frame(x):
        nm, lo = modname(x)
        if nm is None or nm.lower() == exe_name:
            return sym.get(x - rbase + pref, '?')
        return f'[{nm}] {export_name(nm, x - lo)}' if nm.lower() in ('ntdll.dll', 'kernelbase.dll', 'ucrtbase.dll', 'win32u.dll', 'kernel32.dll') else f'[{nm}]'
    addrs |= {x - rbase + pref for v in stacks.values() for _, ch in v for x in ch if (modname(x)[0] or exe_name).lower() == exe_name}
    addrs = sorted(addrs)
    sym = {}
    if addrs:
        r = subprocess.run([shutil.which('llvm-symbolizer'), '--obj=' + a.exe, '--functions=linkage', '--no-inlines', '--output-style=GNU', '--demangle'],
                           input='\n'.join(hex(x) for x in addrs), capture_output=True, text=True)
        lines = r.stdout.splitlines()
        for i, x in enumerate(addrs):
            fn = lines[2 * i] if 2 * i < len(lines) else '?'
            loc = lines[2 * i + 1].split('/')[-1].split('\\')[-1] if 2 * i + 1 < len(lines) else ''
            sym[x] = f'{fn} ({loc.split(":")[0]})'
    if a.chains:
        agg = collections.Counter()
        for v in stacks.values():
            for n, ch in v:
                fr = [frame(x) for x in ch]
                if any(a.chains in f for f in fr):
                    agg[' <- '.join(f.split(' (')[0][:60] for f in fr[:12])] += n
        for k, n in agg.most_common(a.top):
            print(n, k)
        return
    total_all = sum(n for v in samples.values() for _, n, _, _ in v)
    idle = ('ntdll.dll', 'win32u.dll')   # threads blocked in the kernel sit on a system-call return there
    busy = lambda t: sum(n for _, n, mod, _ in samples[t] if mod.lower() not in idle)
    order = sorted(samples, key=lambda t: -busy(t))
    for tid in order[:a.threads]:
        tot = sum(n for _, n, _, _ in samples[tid])
        agg = collections.Counter()
        for rip, n, mod, base in samples[tid]:
            if mod.lower() == exe_name:
                agg[sym.get(rip - base + pref, '?')] += n
            elif mod == '[anon]':
                agg['[jit]'] += n
            elif mod.lower() in ('ntdll.dll', 'kernelbase.dll', 'ucrtbase.dll', 'win32u.dll', 'kernel32.dll'):
                agg[f'[{mod}] {export_name(mod, rip - base)}'] += n
            else:
                agg[f'[{mod}]'] += n
        nm = names.get(tid, (0, '?'))[1]
        print(f'== thread {tid} {nm}: busy {100.0 * busy(tid) / max(1, tot):.1f}% of {tot} samples')
        for k, n in agg.most_common(a.top):
            print(f'  {100.0 * n / tot:5.1f}%  {k[:150]}')
        inc = collections.Counter()
        for n, ch in stacks.get(tid, []):
            for fn in {frame(x) for x in ch}:
                inc[fn] += n
        if inc:
            print('  -- inclusive (call chains inside the executable):')
            for k, n in inc.most_common(a.top):
                print(f'  {100.0 * n / tot:5.1f}%  {k[:150]}')


if __name__ == '__main__':
    main()
