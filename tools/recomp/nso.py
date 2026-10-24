"""NSO0 reader: segments, dynamic table, symbols, relocations (tools/recomp)."""
import hashlib
import struct

import lz4.block

R_AARCH64_ABS64 = 257
R_AARCH64_GLOB_DAT = 1025
R_AARCH64_JUMP_SLOT = 1026
R_AARCH64_RELATIVE = 1027


class Nso:
    def __init__(self, path, name=None):
        data = open(path, 'rb').read()
        if data[:4] != b'NSO0':
            raise ValueError(f'{path}: not an NSO')
        self.name = name
        self.sha256 = hashlib.sha256(data).hexdigest()
        flags = struct.unpack_from('<I', data, 0xC)[0]
        seg = [struct.unpack_from('<4I', data, 0x10 + 16 * i) for i in range(3)]
        comp = struct.unpack_from('<3I', data, 0x60)
        self.build_id = data[0x40:0x60].hex()
        self.text = (seg[0][1], seg[0][2])   # (mem offset, size)
        self.ro = (seg[1][1], seg[1][2])
        self.data = (seg[2][1], seg[2][2])
        self.bss = seg[2][3]
        end = seg[2][1] + seg[2][2] + seg[2][3]
        self.image_end = (end + 0xFFF) & ~0xFFF
        img = bytearray(self.image_end)
        for i in range(3):
            off, mem, size = seg[i][0], seg[i][1], seg[i][2]
            if flags & (1 << i):
                raw = lz4.block.decompress(data[off:off + comp[i]], uncompressed_size=size)
            else:
                raw = data[off:off + size]
            img[mem:mem + size] = raw
        self.image = img
        self._parse_dynamic()

    def u32(self, off):
        return struct.unpack_from('<I', self.image, off)[0]

    def u64(self, off):
        return struct.unpack_from('<Q', self.image, off)[0]

    def _parse_dynamic(self):
        img = self.image
        mod0 = self.u32(4)
        self.syms, self.relocs, self.relr = [], [], []
        self.init = self.fini = 0
        self.init_array = (0, 0)
        if img[mod0:mod0 + 4] != b'MOD0':
            return
        dyn = mod0 + struct.unpack_from('<i', img, mod0 + 4)[0]
        tags = {}
        p = dyn
        while True:
            tag, val = struct.unpack_from('<qQ', img, p)
            p += 16
            if tag == 0:
                break
            tags[tag] = val
        self.tags = tags
        self.init = tags.get(12, 0)
        self.fini = tags.get(13, 0)
        self.init_array = (tags.get(25, 0), tags.get(27, 0))
        symtab, strtab = tags.get(6), tags.get(5)
        nsym = 0
        if 4 in tags:
            nsym = self.u32(tags[4] + 4)
        elif 0x6ffffef5 in tags and symtab:
            # count from relocations' max index and GNU hash: fall back to scanning until strtab
            nsym = (strtab - symtab) // 24 if strtab and strtab > symtab else 0
        for i in range(nsym):
            e = symtab + 24 * i
            name_off, info, other, shndx, value, size = struct.unpack_from('<IBBHQQ', img, e)
            end = img.index(0, strtab + name_off)
            name = img[strtab + name_off:end].decode('latin-1')
            self.syms.append((name, info, shndx, value, size))
        for start_tag, size_tag in ((7, 8), (23, 2)):
            if start_tag in tags:
                for off in range(tags[start_tag], tags[start_tag] + tags[size_tag], 24):
                    where, info, addend = struct.unpack_from('<QQq', img, off)
                    self.relocs.append((where, info & 0xFFFFFFFF, info >> 32, addend))
        if 36 in tags:
            cursor = 0
            for off in range(tags[36], tags[36] + tags[35], 8):
                e = self.u64(off)
                if not e & 1:
                    self.relr.append(e)
                    cursor = e + 8
                else:
                    for bit in range(1, 64):
                        if e >> bit & 1:
                            self.relr.append(cursor + (bit - 1) * 8)
                    cursor += 63 * 8

    def text_words(self):
        off, size = self.text
        return struct.unpack_from(f'<{size // 4}I', self.image, off)


MODULES_AT = 0x800000000
MAP_ORDER = ['rtld', 'main', 'subsdk0', 'sdk']


def module_layout(exefs):
    """Base addresses as loader_win.c maps them: rtld, main, subsdk0, sdk at 0x8_0000_0000, 2 MiB aligned.
    main gets 16 KiB alignment + one extra 16 KiB page (frame-step page)."""
    import os
    at = MODULES_AT
    out = {}
    for name in MAP_ORDER:
        n = Nso(os.path.join(exefs, name), name)
        size = n.image_end
        if name == 'main':
            size = ((size + 0x3FFF) & ~0x3FFF) + 0x4000
        out[name] = (at, n)
        at = (at + size + 0x1FFFFF) & ~0x1FFFFF
    return out
