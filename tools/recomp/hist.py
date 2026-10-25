"""Instruction histogram of the lifted modules (capstone mnemonics + encoding classes)."""
import collections, sys, os, capstone
from nso import module_layout
exefs = sys.argv[1]
md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
md.skipdata = True
cnt = collections.Counter()
for name, (base, n) in module_layout(exefs).items():
    if name == 'rtld': continue
    off, size = n.text
    code = bytes(n.image[off:off+size])
    for (addr, sz, mn, ops) in md.disasm_lite(code, base + off):
        if mn in ('ldr','str','ldp','stp','ldur','stur','mov','fmov','ld1','st1','dup','ins','umov','ldrb','strb','ldrh','strh') and ops:
            # separate SIMD from GPR forms
            r = ops.split(',')[0].strip()
            if r[:1] in 'qdshbv': mn += '.v'
        cnt[mn] += 1
tot = sum(cnt.values())
print('total', tot)
for mn, c in cnt.most_common():
    print(f'{mn:14s} {c:9d} {100*c/tot:7.3f}%')
