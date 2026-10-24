"""AArch64 -> C instruction lifter (tools/recomp).

lift(word, pc) returns an Ins: the C statements for one instruction plus what the function emitter needs to know
(control flow kind, branch target / condition, the general registers / flags it reads and writes).

Register model of the generated code (see games/totk/runtime/cpu_static/rc_rt.h):
  - x0..x30 and sp are C locals of the lifted function: x0..x30, xsp. Register 31 reads as zero (or sp where the
    encoding allows it).
  - NZCV are four 0/1 locals fN fZ fC fV. Flag setters also leave their operands / result in fa fb fr, so the emitter
    can turn a following conditional into a direct compare (fusion).
  - V registers live in the context: c->v[n] (rc_rt.h vector views).
Register bit numbering for the use/def masks: 0..30 x0..x30, 31 sp, 32 flags.

Floating point / AdvSIMD encodings are delegated to a64_fp.py.
"""

MASK64 = (1 << 64) - 1
SP = 31
FLAGS = 32
ALL_GPR = (1 << 32) - 1          # x0..x30 + sp
COND_NAMES = ['EQ', 'NE', 'CS', 'CC', 'MI', 'PL', 'VS', 'VC', 'HI', 'LS', 'GE', 'LT', 'GT', 'LE', 'AL', 'NV']


class Unsupported(Exception):
    """A valid encoding the lifter does not implement (the function is left to the JIT)."""


class Undefined(Exception):
    """Unallocated / reserved encoding (data in text, padding): traps if ever executed."""


def bits(w, lo, n):
    return (w >> lo) & ((1 << n) - 1)


def sext(v, n):
    return v - (1 << n) if v & (1 << (n - 1)) else v


def hx(v):
    v &= MASK64
    return f'0x{v:x}ull' if v > 9 else f'{v}ull'


def hx32(v):
    v &= 0xFFFFFFFF
    return f'0x{v:x}u' if v > 9 else f'{v}u'


def ones(n):
    return (1 << n) - 1


def ror(v, r, size):
    r %= size
    return ((v >> r) | (v << (size - r))) & ones(size) if r else v


def decode_bit_masks(N, imms, immr, size, immediate):
    """ARM ARM DecodeBitMasks: (wmask, tmask)."""
    length = ((N << 6) | (~imms & 0x3F)).bit_length() - 1
    if length < 1:
        raise Undefined('bitmask')
    levels = ones(length)
    if immediate and (imms & levels) == levels:
        raise Undefined('bitmask all ones')
    s, r = imms & levels, immr & levels
    esize = 1 << length
    d = (s - r) & levels
    welem, telem = ones(s + 1), ones(d + 1)
    wmask = tmask = 0
    we = ror(welem, r, esize)
    for i in range(0, size, esize):
        wmask |= we << i
        tmask |= telem << i
    return wmask, tmask


class Ins:
    """One lifted instruction.
    kind: 'op' straight-line code; 'b' branch to target; 'bc' conditional branch (cond = C expr or ('F', cc) flag
    condition); 'bl' call target; 'blr' call register (target = C expr); 'br' jump register; 'ret' return
    (target = C expr of the return address); 'svc' (imm); 'exc' exception (code = kind constant) ; 'udf' undefined.
    uses / defs: register masks (bit 32 = NZCV). fset: flag-setter info (kind, size) for fusion, or None."""
    __slots__ = ('kind', 'code', 'target', 'cond', 'uses', 'defs', 'fset', 'imm', 'vec', 'mem', 'note', 'treg')

    def __init__(self, kind, code='', target=None, cond=None, uses=0, defs=0, fset=None, imm=0, vec=False, mem=False,
                 note=None, treg=None):
        self.kind, self.code, self.target, self.cond = kind, code, target, cond
        self.uses, self.defs, self.fset, self.imm = uses, defs, fset, imm
        self.vec, self.mem, self.note, self.treg = vec, mem, note, treg


class E:
    """Register access recorder for one instruction."""
    __slots__ = ('u', 'd')

    def __init__(self):
        self.u = 0
        self.d = 0

    # reads
    def x(self, r):
        if r == 31:
            return '0ull'
        self.u |= 1 << r
        return f'x{r}'

    def xs(self, r):  # register 31 = sp
        if r == 31:
            self.u |= 1 << SP
            return 'xsp'
        self.u |= 1 << r
        return f'x{r}'

    def w(self, r):
        if r == 31:
            return '0u'
        self.u |= 1 << r
        return f'(uint32_t)x{r}'

    def ws(self, r):
        return f'(uint32_t){self.xs(r)}'

    def r(self, sf, r, sp=False):
        if sf:
            return self.xs(r) if sp else self.x(r)
        return self.ws(r) if sp else self.w(r)

    def flags(self):
        self.u |= 1 << FLAGS

    # writes: statements
    def set(self, sf, r, expr, sp=False):
        if r == 31 and not sp:
            return f'(void)({expr});'
        dst = 'xsp' if r == 31 else f'x{r}'
        self.d |= 1 << (SP if r == 31 else r)
        if sf:
            return f'{dst}={expr};'
        return f'{dst}=(uint32_t)({expr});'

    def setflags(self):
        self.d |= 1 << FLAGS


def cond_flags(cc):
    """C condition over the flag locals."""
    return ['fZ', '!fZ', 'fC', '!fC', 'fN', '!fN', 'fV', '!fV', '(fC && !fZ)', '!(fC && !fZ)', '(fN == fV)',
            '(fN != fV)', '(!fZ && fN == fV)', '!(!fZ && fN == fV)', '1', '1'][cc]


def cond_fused(cc, fset):
    """Condition directly from the last flag setter's operands (fa, fb, fr), or None."""
    kind, size = fset
    T, S = ('uint64_t', 'int64_t') if size == 64 else ('uint32_t', 'int32_t')
    a, b, r = f'({T})fa', f'({T})fb', f'({T})fr'
    sa, sb, sr = f'({S})fa', f'({S})fb', f'({S})fr'
    if kind == 'sub':
        return {0: f'{a} == {b}', 1: f'{a} != {b}', 2: f'{a} >= {b}', 3: f'{a} < {b}', 4: f'{sr} < 0',
                5: f'{sr} >= 0', 8: f'{a} > {b}', 9: f'{a} <= {b}', 10: f'{sa} >= {sb}', 11: f'{sa} < {sb}',
                12: f'{sa} > {sb}', 13: f'{sa} <= {sb}'}.get(cc)
    if kind == 'logic':
        return {0: f'{r} == 0', 1: f'{r} != 0', 2: '0', 3: '1', 4: f'{sr} < 0', 5: f'{sr} >= 0', 6: '0', 7: '1',
                8: '0', 9: '1', 10: f'{sr} >= 0', 11: f'{sr} < 0', 12: f'{sr} > 0', 13: f'{sr} <= 0'}.get(cc)
    if kind == 'add':
        return {0: f'{r} == 0', 1: f'{r} != 0', 2: f'{r} < {a}', 3: f'{r} >= {a}', 4: f'{sr} < 0',
                5: f'{sr} >= 0'}.get(cc)
    return None


def flags_sub(sf):
    return f'FSUB{64 if sf else 32}();'


def flags_add(sf):
    return f'FADD{64 if sf else 32}();'


def flags_logic(sf):
    return f'FLOGIC{64 if sf else 32}();'


def shift_expr(sf, val, typ, amt):
    if amt == 0:
        return val
    if typ == 0:
        return f'({val} << {amt})'
    if typ == 1:
        return f'({val} >> {amt})'
    if typ == 2:
        return f'(uint64_t)((int64_t){val} >> {amt})' if sf else f'(uint32_t)((int32_t){val} >> {amt})'
    return f'ROR{64 if sf else 32}({val}, {amt})'


def extend_expr(e, r, option, shift):
    """Extended register operand (64-bit value)."""
    base = e.x(r)
    v = {0: f'(uint64_t)(uint8_t){base}', 1: f'(uint64_t)(uint16_t){base}', 2: f'(uint64_t)(uint32_t){base}', 3: base,
         4: f'(uint64_t)(int8_t){base}', 5: f'(uint64_t)(int16_t){base}', 6: f'(uint64_t)(int32_t){base}',
         7: base}[option]
    return f'({v} << {shift})' if shift else v


# ------------------------------------------------------------------------------------------------ top level

_fp = None


def lift(w, pc):
    """Lift one instruction word at guest address pc. Raises Unsupported / Undefined."""
    global _fp
    op0 = bits(w, 25, 4)
    if (op0 & 0b1110) == 0b1000:
        return dp_imm(w, pc)
    if (op0 & 0b1110) == 0b1010:
        return branch_sys(w, pc)
    if (op0 & 0b0101) == 0b0100:
        if bits(w, 26, 1):
            if _fp is None:
                import a64_fp as _fp_mod
                _fp = _fp_mod
            return _fp.ldst_vec(w, pc)
        return ldst(w, pc)
    if (op0 & 0b0111) == 0b0101:
        return dp_reg(w, pc)
    if (op0 & 0b0111) == 0b0111:
        if _fp is None:
            import a64_fp as _fp_mod
            _fp = _fp_mod
        return _fp.dp_vec(w, pc)
    raise Undefined(f'unallocated {w:08x}')


# ------------------------------------------------------------------------------------------------ DP immediate

def dp_imm(w, pc):
    e = E()
    sf, rd, rn = bits(w, 31, 1), bits(w, 0, 5), bits(w, 5, 5)
    op = bits(w, 23, 3)
    if op in (0, 1):  # ADR / ADRP
        imm = sext((bits(w, 5, 19) << 2) | bits(w, 29, 2), 21)
        val = ((pc & ~0xFFF) + (imm << 12)) if sf else pc + imm
        return Ins('op', e.set(1, rd, hx(val)), defs=e.d, note=('adr', rd, val & MASK64))
    if op == 2:  # add/sub immediate
        o, S, sh = bits(w, 30, 1), bits(w, 29, 1), bits(w, 22, 1)
        imm = bits(w, 10, 12) << (12 if sh else 0)
        a = e.r(sf, rn, sp=True)
        if S:
            e.setflags()
            code = (f'fa = {a}; fb = {hx(imm) if sf else hx32(imm)}; fr = {"(uint64_t)" if sf else "(uint32_t)"}'
                    f'(fa {"-" if o else "+"} fb); {flags_sub(sf) if o else flags_add(sf)} {e.set(sf, rd, "fr")}')
            return op_(e, code, fset=('sub' if o else 'add', 64 if sf else 32))
        val = f'{a} {"-" if o else "+"} {hx(imm) if sf else hx32(imm)}'
        return op_(e, e.set(sf, rd, val, sp=True))
    if op == 3:
        raise Unsupported('add/sub imm with tags')
    if op == 4:  # logical immediate
        opc, N = bits(w, 29, 2), bits(w, 22, 1)
        if not sf and N:
            raise Undefined('logical imm')
        size = 64 if sf else 32
        imm, _ = decode_bit_masks(N, bits(w, 10, 6), bits(w, 16, 6), size, True)
        k = hx(imm) if sf else hx32(imm)
        a = e.r(sf, rn)
        if opc == 3:
            e.setflags()
            code = f'fr = {a} & {k}; {flags_logic(sf)} {e.set(sf, rd, "fr")}'
            return op_(e, code, fset=('logic', size))
        expr = {0: f'{a} & {k}', 1: f'{a} | {k}', 2: f'{a} ^ {k}'}[opc]
        return op_(e, e.set(sf, rd, expr, sp=True))
    if op == 5:  # move wide
        opc, hw = bits(w, 29, 2), bits(w, 21, 2)
        if not sf and hw > 1:
            raise Undefined('movw hw')
        imm = bits(w, 5, 16) << (hw * 16)
        if opc == 0:
            v = ~imm & ones(64 if sf else 32)
            return op_(e, e.set(sf, rd, hx(v)))
        if opc == 2:
            return op_(e, e.set(sf, rd, hx(imm)))
        if opc == 3:
            mask = ~(0xFFFF << (hw * 16)) & MASK64
            return op_(e, e.set(sf, rd, f'({e.x(rd)} & {hx(mask)}) | {hx(imm)}'))
        raise Undefined('movw opc')
    if op == 6:  # bitfield
        opc, N = bits(w, 29, 2), bits(w, 22, 1)
        immr, imms = bits(w, 16, 6), bits(w, 10, 6)
        size = 64 if sf else 32
        if N != sf or (not sf and (immr >= 32 or imms >= 32)):
            raise Undefined('bitfield')
        wmask, tmask = decode_bit_masks(N, imms, immr, size, False)
        src = e.r(sf, rn)
        K = hx if sf else hx32
        rot = f'ROR{size}({src}, {immr})' if immr else src
        if opc == 2:  # UBFM (lsl, lsr, ubfx, uxtb...)
            if imms >= immr:  # ubfx / lsr: (src >> immr) & ones(imms-immr+1)
                width = imms - immr + 1
                ex = f'({src} >> {immr})' if immr else src
                if width < size:
                    ex = f'{ex} & {K(ones(width))}'
                return op_(e, e.set(sf, rd, ex))
            # lsl / ubfiz: (src & ones(imms+1)) << (size-immr)
            ex = f'(({src} & {K(ones(imms + 1))}) << {size - immr})'
            return op_(e, e.set(sf, rd, ex))
        if opc == 0:  # SBFM (asr, sbfx, sxtb, sbfiz)
            S = 'int64_t' if sf else 'int32_t'
            U = 'uint64_t' if sf else 'uint32_t'
            if imms >= immr:  # sbfx / asr: sign-extend bits [imms:immr]
                width = imms - immr + 1
                ex = f'({U})((({S})({src} << {size - 1 - imms})) >> {size - width})'
                return op_(e, e.set(sf, rd, ex))
            # sbfiz: sign-extend low imms+1 bits, shift left by size-immr
            width = imms + 1
            ex = f'(({U})((({S})({src} << {size - width})) >> {size - width})) << {size - immr}'
            return op_(e, e.set(sf, rd, ex))
        if opc == 1:  # BFM (bfi, bfxil)
            dst = e.r(sf, rd)
            bot = f'(({dst} & {K(~wmask & ones(size))}) | ({rot} & {K(wmask)}))'
            ex = f'({dst} & {K(~tmask & ones(size))}) | ({bot} & {K(tmask)})'
            return op_(e, e.set(sf, rd, ex))
        raise Undefined('bitfield opc')
    if op == 7:  # EXTR
        rm, lsb = bits(w, 16, 5), bits(w, 10, 6)
        if bits(w, 29, 2) or bits(w, 21, 1) or bits(w, 22, 1) != sf or (not sf and lsb >= 32):
            raise Undefined('extr')
        size = 64 if sf else 32
        if lsb == 0:
            return op_(e, e.set(sf, rd, e.r(sf, rm)))
        T = 'uint64_t' if sf else 'uint32_t'
        return op_(e, e.set(sf, rd, f'(({T}){e.r(sf, rm)} >> {lsb}) | (({T}){e.r(sf, rn)} << {size - lsb})'))
    raise Undefined('dp_imm')


def op_(e, code, **kw):
    return Ins('op', code, uses=e.u, defs=e.d, **kw)


# ------------------------------------------------------------------------------------------------ branches, system

def branch_sys(w, pc):
    e = E()
    if bits(w, 26, 5) == 0b00101:  # B / BL
        t = (pc + sext(bits(w, 0, 26), 26) * 4) & MASK64
        return Ins('bl' if bits(w, 31, 1) else 'b', target=t)
    if bits(w, 24, 8) == 0b01010100:  # B.cond
        if bits(w, 4, 1):
            raise Unsupported('bc.cond')
        t = (pc + sext(bits(w, 5, 19), 19) * 4) & MASK64
        cc = bits(w, 0, 4)
        if cc >= 14:
            return Ins('b', target=t)
        return Ins('bc', target=t, cond=('F', cc), uses=1 << FLAGS)
    if bits(w, 25, 6) == 0b011010:  # CBZ / CBNZ
        sf, nz, rt = bits(w, 31, 1), bits(w, 24, 1), bits(w, 0, 5)
        t = (pc + sext(bits(w, 5, 19), 19) * 4) & MASK64
        return Ins('bc', target=t, cond=f'{e.r(sf, rt)} {"!=" if nz else "=="} 0', uses=e.u)
    if bits(w, 25, 6) == 0b011011:  # TBZ / TBNZ
        b = (bits(w, 31, 1) << 5) | bits(w, 19, 5)
        nz, rt = bits(w, 24, 1), bits(w, 0, 5)
        t = (pc + sext(bits(w, 5, 14), 14) * 4) & MASK64
        return Ins('bc', target=t, cond=f'(({e.x(rt)} >> {b}) & 1) {"!=" if nz else "=="} 0', uses=e.u)
    if bits(w, 25, 7) == 0b1101011:  # branch (register)
        opc, op2, op3, rn, op4 = bits(w, 21, 4), bits(w, 16, 5), bits(w, 10, 6), bits(w, 5, 5), bits(w, 0, 5)
        if op2 != 31 or op3 or op4:
            raise Unsupported('branch reg (pointer auth)')
        t = e.x(rn)
        if opc == 0:
            return Ins('br', target=t, uses=e.u, treg=rn)
        if opc == 1:
            return Ins('blr', target=t, uses=e.u, defs=1 << 30, treg=rn)
        if opc == 2:
            return Ins('ret', target=t, uses=e.u, treg=rn)
        raise Unsupported('eret/drps')
    if bits(w, 24, 8) == 0b11010100:  # exception generation
        opc, op2, ll, imm = bits(w, 21, 3), bits(w, 2, 3), bits(w, 0, 2), bits(w, 5, 16)
        if op2:
            raise Undefined('exception op2')
        if opc == 0 and ll == 1:
            return Ins('svc', imm=imm)
        if opc == 1 and ll == 0:
            return Ins('exc', code='CPU_EXC_BREAKPOINT', imm=imm)
        raise Unsupported('hvc/smc/hlt/dcps')
    if bits(w, 22, 10) == 0b1101010100:
        return system(w, pc)
    raise Undefined('branch_sys')


MRS_REGS = {
    (3, 3, 13, 0, 2): 'c->tpidr',
    (3, 3, 13, 0, 3): 'c->tpidrro',
    (3, 3, 14, 0, 0): '19200000ull',      # CNTFRQ_EL0
    (3, 3, 14, 0, 1): 'rc_cntpct(c)',     # CNTPCT_EL0
    (3, 3, 14, 0, 2): 'rc_cntpct(c)',     # CNTVCT_EL0
    (3, 3, 4, 4, 0): 'c->fpcr',
    (3, 3, 4, 4, 1): 'c->fpsr',
    (3, 3, 0, 0, 7): '4ull',              # DCZID_EL0
    (3, 3, 0, 0, 1): '0x8444c004ull',     # CTR_EL0
    (3, 0, 0, 0, 5): '0ull',              # MPIDR_EL1 (not readable at EL0 on Horizon; never seen)
}


def system(w, pc):
    e = E()
    L = bits(w, 21, 1)
    op0, op1, crn, crm, op2, rt = bits(w, 19, 2), bits(w, 16, 3), bits(w, 12, 4), bits(w, 8, 4), bits(w, 5, 3), bits(w, 0, 5)
    if op0 == 0 and not L:
        if crn == 2 and rt == 31:  # hints: NOP YIELD WFE WFI SEV SEVL CSDB BTI; PAC hints are NOPs on ARMv8.0
            if op1 == 3 and crm == 0 and op2 in (1, 2, 3):  # YIELD/WFE/WFI: spin-wait hint
                return Ins('op', 'RC_PAUSE();')
            return Ins('op', '/* hint */')
        if crn == 3 and op1 == 3 and rt == 31:  # barriers
            if op2 == 2:
                return Ins('op', 'c->excl_addr = ~0ull;')
            if op2 == 4 or op2 == 5:  # DSB / DMB
                if crm in (0b1101, 0b1001, 0b0101, 0b0001, 0b1110, 0b1010, 0b0110, 0b0010):  # ...LD / ...ST
                    return Ins('op', 'RC_BARRIER();')
                return Ins('op', 'RC_FENCE();')
            if op2 == 6:  # ISB
                return Ins('op', 'RC_BARRIER();')
            return Ins('op', 'RC_BARRIER();')
        if crn == 4:  # MSR (immediate): pstate fields
            raise Unsupported('msr imm')
        raise Unsupported(f'system {w:08x}')
    if op0 == 1:  # SYS / SYSL
        if L:
            raise Unsupported('sysl')
        if op1 == 3 and crn == 7 and crm == 4 and op2 == 1:  # DC ZVA
            return Ins('op', f'memset((void *)(uintptr_t)({e.x(rt)} & ~63ull), 0, 64);', uses=e.u, mem=True)
        return Ins('op', '/* cache maintenance */', uses=e.u)
    key = (op0, op1, crn, crm, op2)
    if L:  # MRS
        if key == (3, 3, 4, 2, 0):  # NZCV
            e.flags()
            return op_(e, e.set(1, rt, '(uint64_t)fN << 31 | (uint64_t)fZ << 30 | (uint64_t)fC << 29 | (uint64_t)fV << 28'))
        src = MRS_REGS.get(key)
        if src is None:
            raise Unsupported(f'mrs {key}')
        return op_(e, e.set(1, rt, src))
    if key == (3, 3, 4, 2, 0):  # MSR NZCV
        e.setflags()
        v = e.x(rt)
        return op_(e, f'fN = ({v} >> 31) & 1; fZ = ({v} >> 30) & 1; fC = ({v} >> 29) & 1; fV = ({v} >> 28) & 1;')
    dst = {(3, 3, 13, 0, 2): 'c->tpidr = {v};',
           (3, 3, 4, 4, 0): 'rc_set_fpcr(c, {v});',
           (3, 3, 4, 4, 1): 'c->fpsr = {v};'}.get(key)
    if dst is None:
        raise Unsupported(f'msr {key}')
    return op_(e, dst.format(v=e.x(rt)))


# ------------------------------------------------------------------------------------------------ loads / stores (GPR)

LD = {1: 'LD8', 2: 'LD16', 4: 'LD32', 8: 'LD64'}
ST = {1: 'ST8', 2: 'ST16', 4: 'ST32', 8: 'ST64'}


def ldst(w, pc):
    e = E()
    if bits(w, 24, 6) == 0b001000:
        return ldst_excl(w)
    if bits(w, 27, 3) == 0b011 and bits(w, 24, 2) == 0:  # load literal
        opc, rt = bits(w, 30, 2), bits(w, 0, 5)
        addr = (pc + sext(bits(w, 5, 19), 19) * 4) & MASK64
        if opc == 0:
            return op_(e, e.set(0, rt, f'LD32({hx(addr)})'), mem=True)
        if opc == 1:
            return op_(e, e.set(1, rt, f'LD64({hx(addr)})'), mem=True)
        if opc == 2:
            return op_(e, e.set(1, rt, f'(int64_t)(int32_t)LD32({hx(addr)})'), mem=True)
        return Ins('op', '/* prfm */')
    if bits(w, 27, 3) == 0b101:
        return ldst_pair(w)
    if bits(w, 27, 3) == 0b111:
        if bits(w, 24, 2) == 0 and bits(w, 21, 1) and bits(w, 10, 2) == 0:
            raise Unsupported('LSE atomic')
        return ldst_reg(w)
    if bits(w, 24, 6) in (0b011001, 0b011101) or bits(w, 23, 7) in (0b0110010, 0b0110011):
        raise Unsupported('ldapr/stlur/memcpy/tags')
    raise Unsupported(f'ldst {w:08x}')


def ldst_excl(w):
    e = E()
    size, o2, L, o1, rs, o0, rt2, rn, rt = (bits(w, 30, 2), bits(w, 23, 1), bits(w, 22, 1), bits(w, 21, 1),
                                            bits(w, 16, 5), bits(w, 15, 1), bits(w, 10, 5), bits(w, 5, 5), bits(w, 0, 5))
    nb = 8 << size
    addr = e.xs(rn)
    if o1:  # pairs (ldxp/stxp) and CAS
        if o2:
            raise Unsupported('cas')
        if size < 2:
            raise Unsupported('casp')
        if L:
            # LDXP / LDAXP: 2 x size, read atomically as one 2*size access
            if size == 3:
                code = (f'{{ unsigned __int128 v_ = rc_ldxp128(c, {addr}); {e.set(1, rt, "(uint64_t)v_")} '
                        f'{e.set(1, rt2, "(uint64_t)(v_ >> 64)")} }}')
            else:
                code = (f'{{ uint64_t v_ = rc_ldx64(c, {addr}); {e.set(1, rt, "(uint32_t)v_")} '
                        f'{e.set(1, rt2, "v_ >> 32")} }}')
            return op_(e, code, mem=True)
        if size == 3:
            code = (f'{{ uint32_t st_ = rc_stxp128(c, {addr}, {e.x(rt)}, {e.x(rt2)}); {e.set(0, rs, "st_")} }}')
        else:
            code = (f'{{ uint32_t st_ = rc_stx64(c, {addr}, (uint64_t){e.w(rt)} | (uint64_t){e.w(rt2)} << 32); '
                    f'{e.set(0, rs, "st_")} }}')
        return op_(e, code, mem=True)
    if o2 == 0:  # exclusive
        if L:
            return op_(e, e.set(1, rt, f'rc_ldx{nb}(c, {addr})'), mem=True)
        return op_(e, f'{{ uint32_t st_ = rc_stx{nb}(c, {addr}, {e.x(rt)}); {e.set(0, rs, "st_")} }}', mem=True)
    # ordered (LDAR / STLR, LDLAR / STLLR)
    if L:
        return op_(e, e.set(1, rt, f'rc_lda{nb}({addr})'), mem=True)
    return op_(e, f'rc_stl{nb}({addr}, {e.x(rt)});', mem=True)


def ldst_pair(w):
    e = E()
    opc, mode, L = bits(w, 30, 2), bits(w, 23, 2), bits(w, 22, 1)
    imm7, rt2, rn, rt = sext(bits(w, 15, 7), 7), bits(w, 10, 5), bits(w, 5, 5), bits(w, 0, 5)
    if opc == 3 or (opc == 1 and not L):
        raise Unsupported('pair opc (stgp)')
    scale = 3 if opc == 2 else 2
    off = imm7 << scale
    n = 1 << scale
    base = e.xs(rn)
    post, wb = mode == 1, mode in (1, 3)
    if L:
        if opc == 2:
            ld = 'LD64({})'
        elif opc == 1:
            ld = '(int32_t)LD32({})'
        else:
            ld = 'LD32({})'
        sfl = 1 if opc else 0
    if not wb:
        if L:
            a1, a2 = _off(base, off), _off(base, off + n)
            if rn in (rt, rt2) and rn != 31:
                code = (f'{{ uint64_t a_ = {ld.format(a1)}, b_ = {ld.format(a2)}; {e.set(sfl, rt, "a_")} '
                        f'{e.set(sfl, rt2, "b_")} }}')
            else:
                code = f'{e.set(sfl, rt, ld.format(a1))} {e.set(sfl, rt2, ld.format(a2))}'
            return op_(e, code, mem=True)
        if opc == 2:
            return op_(e, f'ST64({_off(base, off)}, {e.x(rt)}); ST64({_off(base, off + n)}, {e.x(rt2)});', mem=True)
        return op_(e, f'ST32({_off(base, off)}, {e.w(rt)}); ST32({_off(base, off + n)}, {e.w(rt2)});', mem=True)
    ea = base if post else _off(base, off)
    lines = [f'uint64_t ea_ = {ea};']
    if L:
        lines.append(f'uint64_t a_ = {ld.format("ea_")}, b_ = {ld.format(f"ea_ + {n}")};')
    elif opc == 2:
        lines.append(f'ST64(ea_, {e.x(rt)}); ST64(ea_ + 8, {e.x(rt2)});')
    else:
        lines.append(f'ST32(ea_, {e.w(rt)}); ST32(ea_ + 4, {e.w(rt2)});')
    lines.append(e.set(1, rn, 'ea_' if not post else _off('ea_', off), sp=True))
    if L:
        lines.append(e.set(sfl, rt, 'a_'))
        lines.append(e.set(sfl, rt2, 'b_'))
    return op_(e, '{ ' + ' '.join(lines) + ' }', mem=True)


def _off(base, imm):
    if imm == 0:
        return base
    return f'{base} + {imm}' if imm > 0 else f'{base} - {-imm}'


def ldst_reg(w):
    e = E()
    size, opc, rn, rt = bits(w, 30, 2), bits(w, 22, 2), bits(w, 5, 5), bits(w, 0, 5)
    base = e.xs(rn)
    wb = None   # writeback: (new base expr over ea_), address expr is then ea_
    if bits(w, 24, 2) == 1:  # unsigned offset
        ea = _off(base, bits(w, 10, 12) << size)
    elif bits(w, 24, 2) == 0:
        kind = bits(w, 10, 2)
        if bits(w, 21, 1):
            if kind != 2:
                raise Unsupported('ldst reg pac')
            rm, option, S = bits(w, 16, 5), bits(w, 13, 3), bits(w, 12, 1)
            if option not in (2, 3, 6, 7):
                raise Undefined('ldst reg option')
            ea = f'{base} + {extend_expr(e, rm, option, size if S else 0)}'
        else:
            imm9 = sext(bits(w, 12, 9), 9)
            if kind in (0, 2):  # unscaled / unprivileged
                ea = _off(base, imm9)
            elif kind == 1:  # post-index
                ea, wb = base, _off(base, imm9)
            else:  # pre-index
                ea, wb = _off(base, imm9), 'ea_'
    else:
        raise Undefined('ldst reg')
    nb = 1 << size
    if opc == 0:
        acc = f'{ST[nb]}(@, {e.x(rt) if size == 3 else e.w(rt)});'
        load = None
    elif opc == 1:
        acc, load = None, (1, f'{LD[nb]}(@)')
    elif size == 3:
        if opc == 2:
            return Ins('op', '/* prfm */', uses=e.u)
        raise Undefined('ldst size3 opc3')
    elif size == 2 and opc == 3:
        raise Undefined('ldst size2 opc3')
    else:  # sign-extending: opc 2 -> 64-bit, 3 -> 32-bit
        acc, load = None, (1 if opc == 2 else 0, f'(int{8 * nb}_t){LD[nb]}(@)')
    if wb is None:
        if load:
            return op_(e, e.set(load[0], rt, load[1].replace('@', ea)), mem=True)
        return op_(e, acc.replace('@', ea), mem=True)
    parts = [f'uint64_t ea_ = {ea};']
    if load:
        parts.append(f'uint64_t v_ = {load[1].replace("@", "ea_")};')
    else:
        parts.append(acc.replace('@', 'ea_'))
    parts.append(e.set(1, rn, wb, sp=True))
    if load:
        parts.append(e.set(load[0], rt, 'v_'))
    return op_(e, '{ ' + ' '.join(parts) + ' }', mem=True)


# ------------------------------------------------------------------------------------------------ DP register

def dp_reg(w, pc):
    e = E()
    sf, rd, rn, rm = bits(w, 31, 1), bits(w, 0, 5), bits(w, 5, 5), bits(w, 16, 5)
    size = 64 if sf else 32
    T = 'uint64_t' if sf else 'uint32_t'
    K = hx if sf else hx32
    if not bits(w, 28, 1):
        if bits(w, 24, 4) == 0b1010:  # logical (shifted register)
            opc, sh, N, amt = bits(w, 29, 2), bits(w, 22, 2), bits(w, 21, 1), bits(w, 10, 6)
            if not sf and amt >= 32:
                raise Undefined('logical shift')
            b = shift_expr(sf, e.r(sf, rm), sh, amt)
            if N:
                b = f'({T})~{b}'
            if opc == 1 and rn == 31 and not N and amt == 0:  # MOV
                return op_(e, e.set(sf, rd, b))
            a = e.r(sf, rn)
            if opc == 3:
                e.setflags()
                code = f'fr = ({T})({a} & {b}); {flags_logic(sf)} {e.set(sf, rd, "fr")}'
                return op_(e, code, fset=('logic', size))
            expr = {0: f'{a} & {b}', 1: f'{a} | {b}', 2: f'{a} ^ {b}'}[opc]
            return op_(e, e.set(sf, rd, expr))
        if bits(w, 24, 4) == 0b1011:  # add/sub (shifted or extended register)
            o, S = bits(w, 30, 1), bits(w, 29, 1)
            if bits(w, 21, 1) == 0:
                sh, amt = bits(w, 22, 2), bits(w, 10, 6)
                if sh == 3 or (not sf and amt >= 32):
                    raise Undefined('add shift')
                a = e.r(sf, rn)
                b = shift_expr(sf, e.r(sf, rm), sh, amt)
                sp_dst = False
            else:
                if bits(w, 22, 2):
                    raise Undefined('add ext opt')
                option, amt = bits(w, 13, 3), bits(w, 10, 3)
                if amt > 4:
                    raise Undefined('add ext shift')
                a = e.r(sf, rn, sp=True)
                b = extend_expr(e, rm, option, amt)
                if not sf:
                    b = f'(uint32_t)({b})'
                sp_dst = not S
            if S:
                e.setflags()
                code = (f'fa = {a}; fb = ({T})({b}); fr = ({T})(fa {"-" if o else "+"} fb); '
                        f'{flags_sub(sf) if o else flags_add(sf)} {e.set(sf, rd, "fr")}')
                return op_(e, code, fset=('sub' if o else 'add', size))
            return op_(e, e.set(sf, rd, f'{a} {"-" if o else "+"} {b}', sp=sp_dst))
        raise Undefined('dp_reg')
    op2 = bits(w, 21, 4)
    if bits(w, 24, 4) == 0b1011:  # 3-source
        op54, op31, o0, ra = bits(w, 29, 2), bits(w, 21, 3), bits(w, 15, 1), bits(w, 10, 5)
        if op54:
            raise Undefined('dp3')
        sgn = '-' if o0 else '+'
        if op31 == 0:
            return op_(e, e.set(sf, rd, f'{e.r(sf, ra)} {sgn} {e.r(sf, rn)} * {e.r(sf, rm)}'))
        if not sf:
            raise Undefined('dp3 32')
        if op31 == 1:
            return op_(e, e.set(1, rd, f'{e.x(ra)} {sgn} (uint64_t)((int64_t)(int32_t){e.w(rn)} * (int64_t)(int32_t){e.w(rm)})'))
        if op31 == 5:
            return op_(e, e.set(1, rd, f'{e.x(ra)} {sgn} (uint64_t){e.w(rn)} * (uint64_t){e.w(rm)}'))
        if op31 == 2 and not o0:
            return op_(e, e.set(1, rd, f'(uint64_t)(((__int128)(int64_t){e.x(rn)} * (__int128)(int64_t){e.x(rm)}) >> 64)'))
        if op31 == 6 and not o0:
            return op_(e, e.set(1, rd, f'(uint64_t)(((unsigned __int128){e.x(rn)} * {e.x(rm)}) >> 64)'))
        raise Undefined('dp3 op')
    if bits(w, 24, 4) == 0b1010:
        if op2 == 0:  # ADC / SBC
            o, S = bits(w, 30, 1), bits(w, 29, 1)
            if bits(w, 10, 6):
                raise Unsupported('rmif/setf')
            e.flags()
            a, b = e.r(sf, rn), e.r(sf, rm)
            if o:
                b = f'({T})~{b}'
            if S:
                e.setflags()
                code = (f'{{ {T} a_ = {a}, b_ = {b}; unsigned c_ = fC; {T} r_ = a_ + b_ + c_; '
                        f'fC = c_ ? r_ <= a_ : r_ < a_; fV = ((a_ ^ r_) & (b_ ^ r_)) >> {size - 1}; '
                        f'fN = r_ >> {size - 1}; fZ = r_ == 0; fr = r_; {e.set(sf, rd, "r_")} }}')
                return op_(e, code)
            return op_(e, e.set(sf, rd, f'{a} + {b} + ({T})fC'))
        if op2 == 2:  # CCMN / CCMP
            o, imm_form, cc, nzcv = bits(w, 30, 1), bits(w, 11, 1), bits(w, 12, 4), bits(w, 0, 4)
            if bits(w, 29, 1) == 0 or bits(w, 10, 1) or bits(w, 4, 1):
                raise Undefined('ccmp')
            e.flags()
            e.setflags()
            b = K(rm) if imm_form else e.r(sf, rm)
            a = e.r(sf, rn)
            setf = flags_sub(sf) if o else flags_add(sf)
            code = (f'if ({{COND{cc}}}) {{ fa = {a}; fb = {b}; fr = ({T})(fa {"-" if o else "+"} fb); {setf} }} '
                    f'else {{ fN = {nzcv >> 3 & 1}; fZ = {nzcv >> 2 & 1}; fC = {nzcv >> 1 & 1}; fV = {nzcv & 1}; }}')
            return op_(e, code, note=('ccmp', cc))
        if op2 == 4:  # conditional select
            o, o2, cc = bits(w, 30, 1), bits(w, 10, 2), bits(w, 12, 4)
            if bits(w, 29, 1) or o2 > 1:
                raise Undefined('csel')
            e.flags()
            b = e.r(sf, rm)
            alt = {(0, 0): b, (0, 1): f'{b} + 1', (1, 0): f'~{b}', (1, 1): f'-{b}'}[(o, o2)]
            a = e.r(sf, rn)
            code = e.set(sf, rd, f'({{COND{cc}}}) ? ({T}){a} : ({T})({alt})')
            return op_(e, code, note=('csel', cc))
        if op2 == 6:  # data processing 1 / 2 source
            opcode = bits(w, 10, 6)
            if bits(w, 30, 1):  # 1-source
                if bits(w, 16, 5) or bits(w, 29, 1):
                    raise Unsupported('dp1 (pac)')
                a = e.r(sf, rn)
                expr = {0: f'rc_rbit{size}({a})', 1: f'rc_rev16_{size}({a})',
                        2: f'__builtin_bswap32({a})' if not sf else f'rc_rev32_64({a})',
                        3: f'__builtin_bswap64({a})' if sf else None,
                        4: f'rc_clz{size}({a})', 5: f'rc_cls{size}({a})'}.get(opcode)
                if expr is None:
                    raise Undefined('dp1')
                return op_(e, e.set(sf, rd, expr))
            if bits(w, 29, 1):
                raise Undefined('dp2 S')
            if 16 <= opcode <= 23:  # CRC32 / CRC32C
                C, sz = bits(w, 12, 1), bits(w, 10, 2)
                if (sz == 3) != bool(sf):
                    raise Undefined('crc size')
                val = e.x(rm) if sz == 3 else e.w(rm)
                return op_(e, e.set(0, rd, f'rc_crc32{"c" if C else ""}_{8 << sz}({e.w(rn)}, {val})'))
            a, b = e.r(sf, rn), e.r(sf, rm)
            expr = {2: f'rc_udiv{size}({a}, {b})', 3: f'rc_sdiv{size}({a}, {b})',
                    8: f'({T})({a} << ({b} & {size - 1}))', 9: f'({a} >> ({b} & {size - 1}))',
                    10: f'({T})(({"int64_t" if sf else "int32_t"}){a} >> ({b} & {size - 1}))',
                    11: f'ROR{size}({a}, {b} & {size - 1})'}.get(opcode)
            if expr is None:
                raise Unsupported('dp2')
            return op_(e, e.set(sf, rd, expr))
    raise Unsupported(f'dp_reg {w:08x}')


# ------------------------------------------------------------------------------------------------ helpers for gen

def branch_target(w, pc):
    """Static target of a direct branch / call word, or None (used by discovery without full lifting)."""
    if (w & 0x7C000000) == 0x14000000:  # B / BL
        return (pc + sext(w & 0x3FFFFFF, 26) * 4) & MASK64
    if (w & 0xFF000010) == 0x54000000 or (w & 0x7E000000) == 0x34000000:  # B.cond, CBZ/CBNZ
        return (pc + sext(bits(w, 5, 19), 19) * 4) & MASK64
    if (w & 0x7E000000) == 0x36000000:  # TBZ/TBNZ
        return (pc + sext(bits(w, 5, 14), 14) * 4) & MASK64
    return None
