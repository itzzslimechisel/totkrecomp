"""AArch64 FP / AdvSIMD lifter (placeholder until implemented)."""
from a64 import Unsupported


def ldst_vec(w, pc):
    raise Unsupported('simd ldst')


def dp_vec(w, pc):
    raise Unsupported('simd dp')
