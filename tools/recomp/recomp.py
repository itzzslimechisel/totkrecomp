"""TOTK static recompiler driver: AArch64 NSOs -> C (tools/recomp).

  python tools/recomp/recomp.py <exefs dir> <out dir> [--jobs N] [--modules main,subsdk0,sdk] [--limit N]

Discovery: seeds (dynamic symbols, relocation targets in text, .eh_frame_hdr, DT_INIT/FINI) are closed over the
reachable code: call targets and code addresses materialized with ADR / ADRP+ADD become functions too. Text is then
partitioned at the function starts; a branch leaving its function makes its target a start as well (fixpoint), so
every lifted function is a contiguous range [entry, next start) whose internal control flow is C gotos and whose exits
are C calls / tail calls. Jump tables are resolved with jumptab.py.

Output (all game-derived, never committed): <out>/rc_<module>_NNN.c chunks, rc_tables.c (address -> function, range
lengths, content hashes for patch detection), manifest.json (coverage, exclusions)."""
import argparse
import hashlib
import json
import multiprocessing as mp
import os
import struct
import sys
import time
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import a64  # noqa: E402
import jumptab  # noqa: E402
from a64 import ALL_GPR, FLAGS, SP, MASK64, Undefined, Unsupported, bits, sext  # noqa: E402
from nso import module_layout  # noqa: E402

LIFT_MODULES = ('main', 'subsdk0', 'sdk')
FNV_OFF, FNV_PRIME = 0xcbf29ce484222325, 0x100000001b3


def fnv_words(words):
    h = FNV_OFF
    for w in words:
        h = ((h ^ w) * FNV_PRIME) & MASK64
    return h


# ---------------------------------------------------------------------------------------------- module model

class Module:
    def __init__(self, name, base, nso):
        self.name, self.base, self.nso = name, base, nso
        off, size = nso.text
        self.lo = base + off
        self.hi = self.lo + size
        self.words = nso.text_words()
        self.mem = jumptab.Mem(bytes(nso.image), base)

    def in_text(self, a):
        return self.lo <= a < self.hi and not (a & 3)

    def word(self, a):
        return self.words[(a - self.lo) >> 2]


def eh_frame_starts(m):
    """Function starts from .eh_frame_hdr (MOD0 points at it)."""
    img = m.nso.image
    mod0 = m.nso.u32(4)
    if img[mod0:mod0 + 4] != b'MOD0':
        return []
    hdr_s = mod0 + struct.unpack_from('<i', img, mod0 + 16)[0]
    hdr_e = mod0 + struct.unpack_from('<i', img, mod0 + 20)[0]
    if hdr_e <= hdr_s or img[hdr_s] != 1:
        return []
    ver, ptr_enc, cnt_enc, tbl_enc = img[hdr_s:hdr_s + 4]

    def rd(off, enc):
        fmt, app = enc & 0x0F, enc & 0x70
        if fmt == 0x0B:
            v, n = struct.unpack_from('<i', img, off)[0], 4
        elif fmt == 0x03:
            v, n = struct.unpack_from('<I', img, off)[0], 4
        elif fmt == 0x0C:
            v, n = struct.unpack_from('<q', img, off)[0], 8
        elif fmt in (0x04, 0x00):
            v, n = struct.unpack_from('<Q', img, off)[0], 8
        else:
            raise ValueError(enc)
        if app == 0x10:
            v += off
        elif app == 0x30:
            v += hdr_s
        return v, n

    off = hdr_s + 4
    _, n = rd(off, ptr_enc)
    off += n
    count, n = rd(off, cnt_enc)
    off += n
    out = []
    for _ in range(count):
        loc, n = rd(off, tbl_enc)
        off += n
        _, n = rd(off, tbl_enc)
        off += n
        out.append(m.base + loc)
    return out


def seeds(m):
    n, b = m.nso, m.base
    s = set()
    for name, info, shndx, value, size in n.syms:
        if shndx and (info & 0xF) in (0, 2) and m.in_text(b + value):
            s.add(b + value)
    defined = {i: v for i, (name, info, shndx, v, size) in enumerate(n.syms) if shndx}
    for where, typ, sym, add in n.relocs:
        if typ == 1027:
            t = b + add
        elif typ in (257, 1025) and sym in defined:
            t = b + defined[sym] + add
        else:
            continue
        if m.in_text(t):
            s.add(t)
    for where in n.relr:
        t = b + n.u64(where)
        if m.in_text(t):
            s.add(t)
    for a in (n.init, n.fini):
        if a and m.in_text(b + a):
            s.add(b + a)
    for a in eh_frame_starts(m):
        if m.in_text(a):
            s.add(a)
    return s


# ---------------------------------------------------------------------------------------------- control flow classes

B_, BL_, BC_, BR_, BLR_, RET_, STOP_ = 1, 2, 3, 4, 5, 6, 7


def cf_class(w, pc):
    """(class, target) for control flow, (0, None) for straight-line code."""
    top = w & 0xFC000000
    if top == 0x14000000:
        return B_, (pc + sext(w & 0x3FFFFFF, 26) * 4) & MASK64
    if top == 0x94000000:
        return BL_, (pc + sext(w & 0x3FFFFFF, 26) * 4) & MASK64
    if (w & 0xFF000010) == 0x54000000:
        t = (pc + sext(bits(w, 5, 19), 19) * 4) & MASK64
        return (B_ if (w & 15) >= 14 else BC_), t
    if (w & 0x7E000000) == 0x34000000:
        return BC_, (pc + sext(bits(w, 5, 19), 19) * 4) & MASK64
    if (w & 0x7E000000) == 0x36000000:
        return BC_, (pc + sext(bits(w, 5, 14), 14) * 4) & MASK64
    if (w & 0xFE000000) == 0xD6000000:
        opc = (w >> 21) & 15
        if opc == 0:
            return BR_, None
        if opc == 1:
            return BLR_, None
        if opc == 2:
            return RET_, None
        return STOP_, None
    if (w & 0xFFE0001F) == 0xD4200000:  # brk
        return STOP_, None
    if (w >> 25) & 15 in (0, 1, 2, 3):  # unallocated / udf
        return STOP_, None
    return 0, None


def adr_target(w, pc):
    if (w & 0x9F000000) == 0x10000000:
        imm = sext((bits(w, 5, 19) << 2) | bits(w, 29, 2), 21)
        return (pc + imm) & MASK64
    return None


def scan(m, entry, end, jt_cache):
    """Reachable instructions of [entry, end) from entry. Returns (reach set, external targets, call targets,
    address-taken targets, jump tables {br pc: targets or None})."""
    reach = set()
    ext, calls, taken = set(), set(), set()
    tables = {}
    work = [entry]
    adrp = {}
    while work:
        a = work.pop()
        while entry <= a < end and a not in reach:
            reach.add(a)
            w = m.words[(a - m.lo) >> 2]
            k, t = cf_class(w, a)
            if k == 0:
                if (w & 0x9F000000) == 0x10000000:  # ADR
                    t = adr_target(w, a)
                    if m.in_text(t):
                        nxt = [m.words[(p - m.lo) >> 2] for p in range(a + 4, min(a + 36, m.hi), 4)]
                        if not any((x & 0xFFFFFC1F) == 0xD61F0000 for x in nxt):
                            taken.add(t)
                elif (w & 0x9F000000) == 0x90000000:  # ADRP
                    imm = sext((bits(w, 5, 19) << 2) | bits(w, 29, 2), 21)
                    adrp[w & 31] = ((a & ~0xFFF) + (imm << 12)) & MASK64
                elif (w & 0xFFC00000) == 0x91000000:  # ADD x, x, #imm
                    rn = (w >> 5) & 31
                    if rn in adrp:
                        t = adrp[rn] + ((w >> 10) & 0xFFF)
                        if m.in_text(t):
                            taken.add(t)
                a += 4
                continue
            if k == B_:
                if entry <= t < end:
                    work.append(t)
                elif m.in_text(t):
                    ext.add(t)
                break
            if k == BC_:
                if entry <= t < end:
                    work.append(t)
                elif m.in_text(t):
                    ext.add(t)
                a += 4
                continue
            if k == BL_:
                if m.in_text(t):
                    calls.add(t)
                a += 4
                continue
            if k == BLR_:
                a += 4
                continue
            if k == BR_:
                key = (a, entry, end)
                if key not in jt_cache:
                    jt_cache[key] = jumptab.resolve(m.word, a, entry, end, m.mem, m.in_text)
                tg = jt_cache[key]
                tables[a] = tg
                if tg:
                    for t in tg:
                        if entry <= t < end:
                            work.append(t)
                        else:
                            ext.add(t)
                break
            break  # RET / STOP
        else:
            if a == end and end < m.hi and (a - 4) in reach:
                ext.add(end)  # falls through into the next function
    return reach, ext, calls, taken, tables


def discover(m, log):
    t0 = time.time()
    starts = seeds(m)
    log(f'{m.name}: {len(starts)} seeds')
    scanned = {}
    jt_cache = {}
    rounds = 0
    while True:
        rounds += 1
        order = sorted(starts)
        new = set()
        for i, a in enumerate(order):
            end = order[i + 1] if i + 1 < len(order) else m.hi
            key = (a, end)
            r = scanned.get(key)
            if r is None:
                r = scanned[key] = scan(m, a, end, jt_cache)
            new |= (r[1] | r[2] | r[3]) - starts
        new = {a for a in new if m.in_text(a)}
        log(f'{m.name}: round {rounds}: {len(order)} functions, +{len(new)} ({time.time() - t0:.0f}s)')
        if not new:
            break
        starts |= new
    funcs = []
    for i, a in enumerate(order):
        end = order[i + 1] if i + 1 < len(order) else m.hi
        reach, ext, calls, taken, tables = scanned[(a, end)]
        funcs.append((a, end, tables))
    return funcs


# ---------------------------------------------------------------------------------------------- emission

CALLER_SAVED = ((1 << 19) - 1) | 1 << 30          # x0..x18, x30
CALL_CLOBBER = CALLER_SAVED | 1 << FLAGS
CALL_USES = ((1 << 9) - 1) | 1 << 29 | 1 << SP     # x0..x8 arguments, frame pointer, stack
CALL_STORE = CALLER_SAVED | 1 << 29 | 1 << SP     # stored before a call when dirty


def dirty_out_of(ins, d):
    """Dirty set after executing ins with dirty set d on entry."""
    if isinstance(ins, tuple):
        return d
    k = ins.kind
    if k in ('bl', 'blr'):
        return (d | 1 << 30) & ~CALL_STORE & ~(1 << FLAGS)
    if k == 'svc':
        return 0
    return d | ins.defs


def lift_at(m, a, cache):
    ins = cache.get(a)
    if ins is None:
        try:
            ins = a64.lift(m.word(a), a)
        except Unsupported as e:
            ins = ('unsupported', str(e))
        except Undefined as e:
            ins = ('undefined', str(e))
        cache[a] = ins
    return ins


def fname(a):
    return f'f_{a:x}'


def regs_of(mask):
    return [r for r in range(33) if mask >> r & 1]


class Emitter:
    def __init__(self, m, starts, index):
        self.m, self.starts, self.index = m, starts, index
        self.cache = {}

    def call_expr(self, t, refs):
        if t in self.starts:
            refs.add(t)
            return f'{fname(t)}(c, 0)'
        return f'rc_call(c, {a64.hx(t)})'

    def function(self, entry, end, tables, idx):
        """Returns (C lines, refs, status, info)."""
        m = self.m
        # reachable instructions + lifting
        reach = []
        seen = set()
        work = [entry]
        succ = {}
        insmap = {}
        labels = set()
        unsupported = None
        while work:
            a = work.pop()
            while entry <= a < end and a not in seen:
                seen.add(a)
                ins = lift_at(m, a, self.cache)
                insmap[a] = ins
                if isinstance(ins, tuple):
                    if ins[0] == 'unsupported' and unsupported is None:
                        unsupported = (a, ins[1])
                    succ[a] = ()
                    break
                k = ins.kind
                if k == 'b':
                    t = ins.target
                    if entry <= t < end:
                        succ[a] = (t,)
                        labels.add(t)
                        work.append(t)
                    else:
                        succ[a] = ()
                    break
                if k == 'bc':
                    t = ins.target
                    if entry <= t < end:
                        succ[a] = (a + 4, t)
                        labels.add(t)
                        work.append(t)
                    else:
                        succ[a] = (a + 4,)
                    a += 4
                    continue
                if k == 'br':
                    tg = tables.get(a)
                    s = []
                    if tg:
                        for t in tg:
                            if entry <= t < end:
                                s.append(t)
                                labels.add(t)
                                work.append(t)
                    succ[a] = tuple(s)
                    break
                if k in ('ret', 'exc'):
                    succ[a] = ()
                    break
                succ[a] = (a + 4,)
                a += 4
        if unsupported:
            return self.stub(entry, f'unsupported at {unsupported[0]:#x}: {unsupported[1]}'), set(), 'excluded', unsupported[1]
        order = sorted(seen)
        # liveness (bit 0..30 x, 31 sp, 32 flags). Calls: AAPCS64 callee-saved x19..x29 and sp survive them;
        # arguments are x0..x8 (+ sp, x29 for frame records).
        ALL = ALL_GPR | (1 << FLAGS)
        use, dfn, used, written = {}, {}, 0, 0
        for a in order:
            ins = insmap[a]
            if isinstance(ins, tuple):
                use[a], dfn[a] = ALL, 0
                continue
            k = ins.kind
            used |= ins.uses | ins.defs
            written |= ins.defs
            if k in ('bl', 'blr'):
                use[a] = ins.uses       # (arguments are read from the synced RcCpu, not from locals)
                dfn[a] = CALL_CLOBBER
                used |= 1 << 30
                written |= 1 << 30
            elif k == 'svc':
                use[a] = 0
                dfn[a] = ALL_GPR
            else:
                use[a] = ins.uses
                dfn[a] = ins.defs
        live_in = {a: 0 for a in order}
        live_out = {}
        exits = {}
        preds = {a: [] for a in order}
        for a in order:
            for s_ in succ[a]:
                if s_ in preds:
                    preds[s_].append(a)
            ins = insmap[a]
            ex = False
            if isinstance(ins, tuple):
                ex = True
            else:
                k = ins.kind
                if k in ('ret', 'exc', 'br') or (k == 'b' and not (entry <= ins.target < end)) or \
                        (k == 'bc' and not (entry <= ins.target < end)):
                    ex = True
                if a + 4 >= end and k not in ('b', 'br', 'ret', 'exc'):
                    ex = True  # falls out of the range
            exits[a] = 0  # exits store the dirty registers only: no local needs to be valid there
        changed = True
        while changed:
            changed = False
            for a in reversed(order):
                lo = exits[a]
                for s_ in succ[a]:
                    if s_ in live_in:
                        lo |= live_in[s_]
                li = use[a] | (lo & ~dfn[a])
                live_out[a] = lo
                if li != live_in[a]:
                    live_in[a] = li
                    changed = True
        # dirty registers (local differs from the RcCpu copy) on entry to each instruction: forward union
        dirty_in = {a: 0 for a in order}
        changed = True
        while changed:
            changed = False
            for a in order:
                d = 0
                for p_ in preds[a]:
                    d |= dirty_out_of(insmap[p_], dirty_in[p_])
                if d != dirty_in[a]:
                    dirty_in[a] = d
                    changed = True
        used_gpr = used & ALL_GPR
        flags_used = bool((used | written) >> FLAGS & 1)

        def sync_out(dmask, mask=ALL):
            m_ = dmask & mask
            s = [f'c->x[{r}]=x{r};' if r != SP else 'c->sp=xsp;' for r in regs_of(m_ & ALL_GPR)]
            if m_ >> FLAGS & 1:
                s.append('RC_PACK();')
            return ''.join(s)

        def sync_in(mask):
            s = [f'x{r}=c->x[{r}];' if r != SP else 'xsp=c->sp;' for r in regs_of(mask & used_gpr)]
            if mask >> FLAGS & 1 and flags_used:
                s.append('RC_UNPACK();')
            return ''.join(s)

        refs = set()
        out = [f'RC_FN uint64_t {fname(entry)}(RcCpu *restrict c, uint64_t t_) {{',
               f' RC_GUARD({idx}, {a64.hx(entry)});']
        decl = [f'x{r}=c->x[{r}]' if r != SP else 'xsp=c->sp' for r in regs_of(used_gpr)]
        if decl:
            out.append(' uint64_t ' + ','.join(decl) + ';')
        if flags_used:
            out.append(' uint64_t fa=0,fb=0,fr=0; unsigned fN=0,fZ=0,fC=0,fV=0;')
            if live_in[entry] >> FLAGS & 1:
                out.append(' RC_UNPACK();')
        out.append(' (void)t_;')
        last_fset = None
        insns = 0

        def cond_of(cc):
            if last_fset is not None:
                f = a64.cond_fused(cc, last_fset)
                if f is not None:
                    return f
            return a64.cond_flags(cc)

        def tail(t, d):
            if t in self.starts:
                refs.add(t)
                return f'{sync_out(d)}RC_TAIL({fname(t)});'
            return f'{sync_out(d)}RC_TAILCALL({a64.hx(t)});'

        for i, a in enumerate(order):
            if a in labels or a == entry and entry in labels:
                out.append(f'L_{a:x}:;')
                last_fset = None
            elif i and order[i - 1] != a - 4:
                last_fset = None
            ins = insmap[a]
            insns += 1
            d = dirty_in[a]
            if isinstance(ins, tuple):  # undefined: trap if executed
                out.append(f' {sync_out(d)}return rc_exception(c, {a64.hx(a)}, CPU_EXC_UNDEFINED);')
                continue
            k = ins.kind
            if k == 'op':
                code = ins.code
                if '{COND' in code:
                    cc = ins.note[1]
                    code = code.replace(f'{{COND{cc}}}', cond_of(cc))
                out.append(' ' + code)
                if ins.defs >> FLAGS & 1:
                    last_fset = ins.fset
            elif k == 'bc':
                cond = ins.cond
                if isinstance(cond, tuple):
                    cond = cond_of(cond[1])
                t = ins.target
                if entry <= t < end:
                    out.append(f' if ({cond}) goto L_{t:x};')
                else:
                    out.append(f' if ({cond}) {{ {tail(t, d)} }}')
            elif k == 'b':
                t = ins.target
                if entry <= t < end:
                    out.append(f' goto L_{t:x};')
                else:
                    out.append(f' {tail(t, d)}')
            elif k == 'bl':
                ra = a64.hx(a + 4)
                out.append(f' x30={ra}; {sync_out(d | 1 << 30, CALL_STORE)}{{ uint64_t r_={self.call_expr(ins.target, refs)}; '
                           f'if (__builtin_expect(r_!={ra},0)) return r_; }} {sync_in(live_out[a] & CALL_CLOBBER)}')
                last_fset = None
            elif k == 'blr':
                ra = a64.hx(a + 4)
                out.append(f' {{ uint64_t t1_={ins.target}; x30={ra}; {sync_out(d | 1 << 30, CALL_STORE)}uint64_t r_=rc_call(c, t1_); '
                           f'if (__builtin_expect(r_!={ra},0)) return r_; }} {sync_in(live_out[a] & CALL_CLOBBER)}')
                last_fset = None
            elif k == 'br':
                tg = tables.get(a)
                cases = ''
                if tg:
                    cases = 'switch (t1_) { ' + ' '.join(f'case {a64.hx(t)}: goto L_{t:x};' for t in tg
                                                       if entry <= t < end) + ' default: break; } '
                out.append(f' {{ uint64_t t1_={ins.target}; {cases}{sync_out(d)}RC_TAILCALL(t1_); }}')
            elif k == 'ret':
                out.append(f' {{ uint64_t t1_={ins.target}; {sync_out(d)}return t1_; }}')
            elif k == 'svc':
                out.append(f' {sync_out(d)}rc_svc(c, {ins.imm}, {a64.hx(a)}); {sync_in(ALL_GPR & live_out[a])}')
                last_fset = None
            elif k == 'exc':
                out.append(f' {sync_out(d)}return rc_exception(c, {a64.hx(a)}, {ins.code});')
            else:
                raise RuntimeError(k)
            if a + 4 >= end and k not in ('b', 'br', 'ret', 'exc'):
                dx = dirty_out_of(ins, d)
                if end < m.hi:
                    out.append(f' {tail(end, dx)}')
                else:
                    out.append(f' {sync_out(dx)}return rc_exception(c, {a64.hx(end)}, CPU_EXC_UNDEFINED);')
        out.append('}')
        return out, refs, 'lifted', insns

    def stub(self, entry, why):
        return [f'/* {why} */',
                f'RC_FN uint64_t {fname(entry)}(RcCpu *restrict c, uint64_t t_) {{ (void)t_; '
                f'RC_TAILCALL_REDIRECT({a64.hx(entry)}); }}']


# ---------------------------------------------------------------------------------------------- workers

_W = {}


def _worker_init(exefs, name, starts, index):
    lay = module_layout(exefs)
    base, nso = lay[name]
    m = Module(name, base, nso)
    _W['m'] = m
    _W['em'] = Emitter(m, set(starts), index)


def _worker_chunk(job):
    chunk_id, funcs, outdir, name = job
    em = _W['em']
    m = _W['m']
    lines, refs, meta = [], set(), []
    defined = set()
    for entry, end, tables, idx in funcs:
        out, r, status, info = em.function(entry, end, tables, idx)
        if status == 'lifted':
            out_lines, nins = out, info
        else:
            out_lines, nins = out, 0
        lines += out_lines
        refs |= r
        defined.add(entry)
        h = fnv_words(m.words[(entry - m.lo) >> 2:(end - m.lo) >> 2])
        meta.append((entry, end - entry, h, status, nins, info if status != 'lifted' else ''))
        em.cache.clear()
    hdr = ['#include "rc_rt.h"']
    hdr += [f'RC_FN uint64_t {fname(r)}(RcCpu *restrict c, uint64_t t_);' for r in sorted(refs)]
    text = '\n'.join(hdr + lines) + '\n'
    path = os.path.join(outdir, f'rc_{name}_{chunk_id:04d}.c')
    old = None
    if os.path.exists(path):
        with open(path) as f:
            old = f.read()
    if old != text:
        with open(path, 'w') as f:
            f.write(text)
    return chunk_id, meta, len(text)


# ---------------------------------------------------------------------------------------------- main

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('exefs')
    ap.add_argument('out')
    ap.add_argument('--jobs', type=int, default=os.cpu_count())
    ap.add_argument('--modules', default=','.join(LIFT_MODULES))
    ap.add_argument('--limit', type=int, default=0, help='first N functions per module only (testing)')
    ap.add_argument('--chunk-insns', type=int, default=40000)
    args = ap.parse_args()
    t0 = time.time()

    def log(s):
        print(f'[recomp {time.time() - t0:6.0f}s] {s}', file=sys.stderr, flush=True)

    os.makedirs(args.out, exist_ok=True)
    lay = module_layout(args.exefs)
    mods = [n for n in args.modules.split(',') if n]
    all_funcs = {}
    for name in mods:
        base, nso = lay[name]
        m = Module(name, base, nso)
        funcs = discover(m, log)
        if args.limit:
            funcs = funcs[:args.limit]
        all_funcs[name] = (m, funcs)
    # global function index (sorted by address across modules)
    entries = sorted(a for name in mods for a, _, _ in all_funcs[name][1])
    index = {a: i for i, a in enumerate(entries)}
    metas = {}
    total_c = 0
    for name in mods:
        m, funcs = all_funcs[name]
        starts = [a for a, _, _ in funcs]
        jobs, cur, n = [], [], 0
        for a, end, tables in funcs:
            cur.append((a, end, tables, index[a]))
            n += (end - a) >> 2
            if n >= args.chunk_insns:
                jobs.append(cur)
                cur, n = [], 0
        if cur:
            jobs.append(cur)
        log(f'{name}: emitting {len(funcs)} functions in {len(jobs)} chunks')
        with mp.Pool(args.jobs, initializer=_worker_init, initargs=(args.exefs, name, starts, None)) as pool:
            done = 0
            for cid, meta, size in pool.imap_unordered(_worker_chunk, [(i, j, args.out, name) for i, j in enumerate(jobs)]):
                for e in meta:
                    metas[e[0]] = (name,) + e
                total_c += size
                done += 1
                if done % 50 == 0 or done == len(jobs):
                    log(f'{name}: {done}/{len(jobs)} chunks, {total_c / 1e6:.0f} MB C')
        for fn in os.listdir(args.out):  # stale chunks of an earlier run
            if fn.startswith(f'rc_{name}_') and fn.endswith('.c') and int(fn[len(name) + 4:-2]) >= len(jobs):
                os.remove(os.path.join(args.out, fn))
    # tables
    order = entries
    with open(os.path.join(args.out, 'rc_tables.c'), 'w') as f:
        f.write('#include "rc_rt.h"\n')
        for a in order:
            f.write(f'RC_FN uint64_t {fname(a)}(RcCpu *restrict c, uint64_t t_);\n')
        f.write(f'const uint32_t rc_fn_count = {len(order)};\n')
        f.write('const uint64_t rc_fn_addr[] = {\n' + ',\n'.join(a64.hx(a) for a in order) + '};\n')
        f.write('const uint32_t rc_fn_len[] = {\n' + ',\n'.join(str(metas[a][2]) for a in order) + '};\n')
        f.write('const uint64_t rc_fn_hash[] = {\n' + ',\n'.join(a64.hx(metas[a][3]) for a in order) + '};\n')
        f.write('const uint8_t rc_fn_lifted[] = {\n' + ','.join('1' if metas[a][4] == 'lifted' else '0' for a in order) + '};\n')
        f.write('RcFn const rc_fn_ptr[] = {\n' + ',\n'.join(fname(a) for a in order) + '};\n')
        f.write(f'uint8_t rc_bad[{len(order)}];\n')
        f.write('const RcModuleInfo rc_modules[] = {\n')
        for name in mods:
            m = all_funcs[name][0]
            f.write(f' {{"{name}", {a64.hx(m.base)}, {a64.hx(m.lo)}, {a64.hx(m.hi)}, "{m.nso.sha256}"}},\n')
        f.write('};\n')
        f.write(f'const uint32_t rc_module_count = {len(mods)};\n')
    # manifest
    st = Counter()
    reasons = Counter()
    lifted_bytes = total_bytes = 0
    lifted_insns = 0
    for name in mods:
        m = all_funcs[name][0]
        total_bytes += m.hi - m.lo
    for a, (name, entry, ln, h, status, nins, why) in metas.items():
        st[status] += 1
        if status == 'lifted':
            lifted_bytes += ln
            lifted_insns += nins
        else:
            reasons[why] += 1
    man = {'functions': len(order), 'status': dict(st), 'text_bytes': total_bytes, 'lifted_range_bytes': lifted_bytes,
           'lifted_reachable_insns': lifted_insns, 'c_bytes': total_c,
           'exclusion_reasons': dict(reasons.most_common(60)),
           'modules': {name: {'base': all_funcs[name][0].base, 'sha256': all_funcs[name][0].nso.sha256} for name in mods}}
    with open(os.path.join(args.out, 'manifest.json'), 'w') as f:
        json.dump(man, f, indent=1)
    log(f'done: {len(order)} functions {dict(st)}, lifted ranges {100 * lifted_bytes / total_bytes:.1f}% of text, '
        f'{lifted_insns} reachable insns lifted, {total_c / 1e6:.0f} MB C')


if __name__ == '__main__':
    main()
