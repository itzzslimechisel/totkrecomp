"""Jump table resolution for `br xN` (tools/recomp).

Finds the bounds check guarding the table (cmp wI, #imm ; b.hi/b.hs/b.ls/b.lo), then runs the straight-line window
before the br once per index value through a tiny concrete emulator of the address-computation subset (adrp/adr, add/sub,
movz/movk/movn, logical imm, ubfm/sbfm, mov, register-offset / immediate loads from the read-only image). The targets
are the br register's values. Anything the emulator cannot follow makes the resolution fail (the br then dispatches at
run time)."""
from a64 import MASK64, bits, decode_bit_masks, sext, Undefined


class Mem:
    def __init__(self, image, base):
        self.image, self.base = image, base

    def read(self, a, n, signed=False):
        off = a - self.base
        if off < 0 or off + n > len(self.image):
            return None
        v = int.from_bytes(self.image[off:off + n], 'little', signed=signed)
        return v & MASK64


def _ext(v, option, shift):
    if v is None:
        return None
    if option == 0:
        v &= 0xFF
    elif option == 1:
        v &= 0xFFFF
    elif option == 2:
        v &= 0xFFFFFFFF
    elif option == 4:
        v = sext(v & 0xFF, 8)
    elif option == 5:
        v = sext(v & 0xFFFF, 16)
    elif option == 6:
        v = sext(v & 0xFFFFFFFF, 32)
    return (v << shift) & MASK64


def step(regs, w, pc, mem):
    """Execute one instruction on regs (list of 32 values / None; index 31 = sp/zr unknown). Returns False if the
    instruction is not modelled (its destination becomes unknown)."""
    def R(r):
        return 0 if r == 31 else regs[r]

    def set_(r, v, sf=1):
        if r == 31:
            return
        if v is None:
            regs[r] = None
        else:
            regs[r] = v & (MASK64 if sf else 0xFFFFFFFF)

    rd, rn, rm = w & 31, (w >> 5) & 31, (w >> 16) & 31
    sf = w >> 31
    if (w & 0x1F000000) == 0x10000000:  # ADR / ADRP
        imm = sext((bits(w, 5, 19) << 2) | bits(w, 29, 2), 21)
        set_(rd, ((pc & ~0xFFF) + (imm << 12)) if sf else pc + imm)
        return True
    if (w & 0x1F800000) == 0x11000000:  # add/sub imm
        imm = bits(w, 10, 12) << (12 * bits(w, 22, 1))
        a = regs[rn] if rn != 31 else None
        if a is None:
            set_(rd, None)
            return True
        v = a - imm if bits(w, 30, 1) else a + imm
        if not bits(w, 29, 1) or rd != 31:
            set_(rd, v, sf)
        return True
    if (w & 0x1F200000) == 0x0B000000:  # add/sub shifted reg
        sh, amt = bits(w, 22, 2), bits(w, 10, 6)
        a, b = R(rn), R(rm)
        if a is None or b is None or sh == 3:
            set_(rd, None)
            return True
        size = 64 if sf else 32
        b &= (1 << size) - 1
        if sh == 0:
            b <<= amt
        elif sh == 1:
            b >>= amt
        else:
            b = sext(b, size) >> amt
        v = a - b if bits(w, 30, 1) else a + b
        set_(rd, v, sf)
        return True
    if (w & 0x1F200000) == 0x0B200000:  # add/sub extended reg
        a = regs[rn] if rn != 31 else None
        b = _ext(R(rm), bits(w, 13, 3), bits(w, 10, 3))
        if a is None or b is None:
            set_(rd, None)
            return True
        v = a - b if bits(w, 30, 1) else a + b
        set_(rd, v, sf)
        return True
    if (w & 0x1F800000) == 0x12800000:  # move wide
        opc, hw, imm = bits(w, 29, 2), bits(w, 21, 2), bits(w, 5, 16)
        if opc == 0:
            set_(rd, ~(imm << (16 * hw)), sf)
        elif opc == 2:
            set_(rd, imm << (16 * hw), sf)
        elif opc == 3:
            o = R(rd)
            set_(rd, None if o is None else (o & ~(0xFFFF << (16 * hw))) | (imm << (16 * hw)), sf)
        return True
    if (w & 0x1F800000) == 0x12000000:  # logical imm
        try:
            imm, _ = decode_bit_masks(bits(w, 22, 1), bits(w, 10, 6), bits(w, 16, 6), 64 if sf else 32, True)
        except Undefined:
            return False
        a = R(rn)
        opc = bits(w, 29, 2)
        if a is None:
            set_(rd, None)
        else:
            set_(rd, (a & imm) if opc in (0, 3) else (a | imm) if opc == 1 else (a ^ imm), sf)
        return True
    if (w & 0x1F000000) == 0x0A000000 and not bits(w, 21, 1) and bits(w, 10, 6) == 0 and bits(w, 22, 2) == 0:
        opc = bits(w, 29, 2)  # logical shifted reg without shift
        a, b = R(rn), R(rm)
        if a is None or b is None:
            set_(rd, None)
        else:
            set_(rd, (a & b) if opc in (0, 3) else (a | b) if opc == 1 else (a ^ b), sf)
        return True
    if (w & 0x1F800000) == 0x13000000:  # bitfield
        opc, immr, imms = bits(w, 29, 2), bits(w, 16, 6), bits(w, 10, 6)
        size = 64 if sf else 32
        a = R(rn)
        if a is None or opc == 1:
            set_(rd, None)
            return True
        a &= (1 << size) - 1
        if imms >= immr:
            width = imms - immr + 1
            v = (a >> immr) & ((1 << width) - 1)
            if opc == 0:
                v = sext(v, width)
        else:
            width = imms + 1
            v = a & ((1 << width) - 1)
            if opc == 0:
                v = sext(v, width)
            v <<= size - immr
        set_(rd, v, sf)
        return True
    if (w & 0x3B200C00) == 0x38200800:  # load register offset
        size, opc = bits(w, 30, 2), bits(w, 22, 2)
        a = regs[rn] if rn != 31 else None
        b = _ext(R(rm), bits(w, 13, 3), size if bits(w, 12, 1) else 0)
        if opc == 0 or a is None or b is None:
            if opc:
                set_(rd, None)
            return opc != 0
        return _load(regs, set_, mem, (a + b) & MASK64, size, opc, rd)
    if (w & 0x3B000000) == 0x39000000:  # load unsigned imm
        size, opc = bits(w, 30, 2), bits(w, 22, 2)
        if opc == 0:
            return True
        a = regs[rn] if rn != 31 else None
        if a is None:
            set_(rd, None)
            return True
        return _load(regs, set_, mem, (a + (bits(w, 10, 12) << size)) & MASK64, size, opc, rd)
    return False


def _load(regs, set_, mem, a, size, opc, rd):
    n = 1 << size
    if opc == 1:
        set_(rd, mem.read(a, n))
    elif opc in (2, 3):
        v = mem.read(a, n, signed=True)
        set_(rd, v, 1 if opc == 2 else 0)
    else:
        set_(rd, None)
    return True


COPY_MASKS = ((0x7F800000, 0x12000000), (0x7FE0FFE0, 0x2A0003E0), (0x7F800000, 0x53000000), (0x7F800000, 0x13000000))


def resolve(words_at, br_pc, entry, end, mem, in_text):
    """Targets of the jump table at br_pc, or None. words_at(a) -> instruction word."""
    w = words_at(br_pc)
    rn = (w >> 5) & 31
    # bounds check: nearest cmp #imm followed closely by an unsigned conditional branch
    guard = None
    a = br_pc - 4
    lo = max(entry, br_pc - 4 * 48)
    while a >= lo:
        c = words_at(a)
        if (c & 0xFF000010) == 0x54000000 and (c & 15) in (2, 3, 8, 9):
            p = a - 4
            for _ in range(3):
                if p < entry:
                    break
                cw = words_at(p)
                if (cw & 0x7F80001F) == 0x7100001F and not bits(cw, 22, 1):  # cmp wI/xI, #imm (no shift)
                    guard = (p, a, cw, c & 15)
                    break
                p -= 4
            if guard:
                break
        a -= 4
    if guard is None:
        return None
    cmp_pc, bc_pc, cw, cc = guard
    limit = bits(cw, 10, 12)
    count = limit + 1 if cc in (8, 9) else limit   # hi/ls: index <= limit; hs/lo: index < limit
    if count <= 0 or count > 4096:
        return None
    idx = (cw >> 5) & 31
    start = max(entry, cmp_pc - 4 * 64)
    # registers holding copies of the index before the cmp
    copies = {idx}
    changed = True
    while changed:
        changed = False
        for p in range(start, cmp_pc, 4):
            x = words_at(p)
            src = None
            for m, v in COPY_MASKS:
                if (x & m) == v:
                    src = (x >> 16) & 31 if v == 0x2A0003E0 else (x >> 5) & 31
                    break
            d = x & 31
            if src is None or src == 31 or d == 31:
                continue
            for p1, p2 in ((d, src), (src, d)):
                if p1 in copies and p2 not in copies:
                    copies.add(p2)
                    changed = True
    targets = set()
    for i in range(count):
        regs = [None] * 32
        for p in range(start, br_pc, 4):
            if p == cmp_pc:
                for r in copies:
                    regs[r] = i
                continue
            x = words_at(p)
            if (x & 0xFC000000) in (0x94000000, 0x14000000) or (x & 0xFFFFFC1F) in (0xD63F0000, 0xD61F0000):
                if p > cmp_pc:
                    return None   # a call / jump between the check and the br: not a simple table
                regs = [None] * 32
                continue
            if not step(regs, x, p, mem):
                d = x & 31
                if d != 31 and not ((x & 0xFF000010) == 0x54000000 or (x & 0x7E000000) in (0x34000000, 0x36000000)
                                    or (x & 0x7F80001F) == 0x7100001F or (x & 0x3B000000) == 0x39000000):
                    regs[d] = None
        t = regs[rn]
        if t is None or t & 3 or not in_text(t):
            return None
        targets.add(t)
    return sorted(targets)
