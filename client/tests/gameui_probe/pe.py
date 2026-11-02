"""Minimal PE64 reader: MSVC RTTI -> vtable lookup, plus disassembly through llvm-objdump."""
import re
import struct
import subprocess

from disasm import Disasm


class PE:
    def __init__(self, path):
        self.path = path
        self.data = open(path, 'rb').read()
        self._dis = None
        d = self.data
        pe = struct.unpack_from('<I', d, 0x3c)[0]
        assert d[pe:pe + 4] == b'PE\0\0'
        nsec = struct.unpack_from('<H', d, pe + 6)[0]
        optsize = struct.unpack_from('<H', d, pe + 20)[0]
        opt = pe + 24
        assert struct.unpack_from('<H', d, opt)[0] == 0x20b, 'not PE32+'
        self.base = struct.unpack_from('<Q', d, opt + 24)[0]
        self.sections = []
        for i in range(nsec):
            s = opt + optsize + i * 40
            name = d[s:s + 8].rstrip(b'\0').decode()
            vsize, rva, rawsize, rawptr = struct.unpack_from('<IIII', d, s + 8)
            chars = struct.unpack_from('<I', d, s + 36)[0]
            self.sections.append((name, rva, max(vsize, rawsize), rawptr, rawsize, chars))
        self._col_cache = None

    def sec(self, name):
        return next(s for s in self.sections if s[0] == name)

    def off(self, rva):
        for name, srva, size, raw, rawsize, _ in self.sections:
            if srva <= rva < srva + rawsize:
                return raw + rva - srva
        return None

    def read(self, va, n):
        o = self.off(va - self.base)
        return None if o is None else self.data[o:o + n]

    def q(self, va):
        b = self.read(va, 8)
        return None if b is None else struct.unpack('<Q', b)[0]

    def is_code(self, va):
        rva = va - self.base
        return any(s[5] & 0x20000000 and s[1] <= rva < s[1] + s[2] for s in self.sections)

    def cols(self):
        """Every CompleteObjectLocator in .rdata: rva -> (offset, td_rva)."""
        if self._col_cache is None:
            _, rva, size, raw, rawsize, _ = self.sec('.rdata')
            d = self.data
            out = {}
            for o in range(raw, raw + rawsize - 24, 4):
                sig, offset, cd, td, chd, selfrva = struct.unpack_from('<IIIIII', d, o)
                if sig == 1 and selfrva == rva + o - raw:
                    out[selfrva] = (offset, td)
            self._col_cache = out
        return self._col_cache

    def vtables(self, mangled):
        """All vtables for a class, as [(this-offset, va, slot count)]."""
        name = mangled.encode() + b'\0'
        res = []
        for m in re.finditer(re.escape(name), self.data):
            td_off = m.start() - 16
            td_rva = next(s[1] + td_off - s[3] for s in self.sections if s[3] <= td_off < s[3] + s[4])
            for col_rva, (offset, td) in self.cols().items():
                if td != td_rva:
                    continue
                needle = struct.pack('<Q', self.base + col_rva)
                _, rva, size, raw, rawsize, _ = self.sec('.rdata')
                seg = self.data[raw:raw + rawsize]
                for p in range(0, len(seg), 8):
                    if seg[p:p + 8] == needle:
                        vt = self.base + rva + p + 8
                        n = 0
                        while True:
                            f = self.q(vt + 8 * n)
                            if f is None or not self.is_code(f):
                                break
                            n += 1
                        res.append((offset, vt, n))
        return sorted(res)

    def vtable(self, mangled):
        vts = [v for v in self.vtables(mangled) if v[0] == 0]
        assert len(vts) == 1, (mangled, vts)
        return vts[0][1], vts[0][2]

    def slot(self, vt, i):
        return self.q(vt + 8 * i)

    def strref(self, s):
        """VA of a NUL-terminated string in the image."""
        o = self.data.find(s.encode() + b'\0')
        if o < 0:
            return None
        return next(self.base + sec[1] + o - sec[3] for sec in self.sections if sec[3] <= o < sec[3] + sec[4])

    def dis(self, va, n=0x80, stop_at_ret=True):
        if self._dis is None:
            self._dis = Disasm(self.path)
        return self._dis.dis(va, n, stop_at_ret)

    def text(self, va, n=0x80, stop_at_ret=True):
        return '\n'.join(f'  {a:x}: {i}' for a, i in self.dis(va, n, stop_at_ret))

    def imports(self):
        """IAT slot VA -> 'dll!name'."""
        d = self.data
        pe = struct.unpack_from('<I', d, 0x3c)[0]
        opt = pe + 24
        imp_rva = struct.unpack_from('<I', d, opt + 112 + 8)[0]
        res = {}
        o = self.off(imp_rva)
        while True:
            ilt, _, _, name, iat = struct.unpack_from('<IIIII', d, o)
            if not name:
                break
            dll = d[self.off(name):].split(b'\0')[0].decode()
            i = 0
            while True:
                e = struct.unpack_from('<Q', d, self.off(ilt or iat) + 8 * i)[0]
                if not e:
                    break
                fn = f'#{e & 0xffff}' if e >> 63 else d[self.off(e & 0x7fffffff) + 2:].split(b'\0')[0].decode()
                res[self.base + iat + 8 * i] = f'{dll}!{fn}'
                i += 1
            o += 20
        return res

    def annotate(self, lines):
        """Replace '# 0x...' IAT comments with the import name."""
        imp = self.imports()
        out = []
        for a, ins in lines:
            m = re.search(r'# (0x[0-9a-f]+)', ins)
            if m and int(m.group(1), 16) in imp:
                ins = ins + ' <' + imp[int(m.group(1), 16)] + '>'
            out.append((a, ins))
        return out

    def func_size(self, va):
        """Extent of the function starting at va, from .pdata (0x400 if not listed)."""
        if not hasattr(self, '_pdata'):
            _, rva, size, raw, rawsize, _ = self.sec('.pdata')
            self._pdata = {}
            for o in range(raw, raw + rawsize, 12):
                b, e, _u = struct.unpack_from('<III', self.data, o)
                if b:
                    self._pdata[self.base + b] = e - b
        return self._pdata.get(va, 0x400)
