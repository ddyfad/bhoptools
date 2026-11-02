"""Minimal ELF64 .so reader: Itanium RTTI -> vtable through R_X86_64_RELATIVE relocs, plus disassembly."""
import re
import subprocess

from disasm import Disasm


class ELF:
    def __init__(self, path):
        self.path = path
        self.data = open(path, 'rb').read()
        self._dis = None
        self.rel = {}
        out = subprocess.run(['llvm-readelf', '-r', '--wide', path], capture_output=True, text=True).stdout
        for l in out.splitlines():
            m = re.match(r'([0-9a-f]{16})\s+[0-9a-f]+\s+R_X86_64_RELATIVE\s+([0-9a-f]+)', l)
            if m:
                self.rel[int(m.group(1), 16)] = int(m.group(2), 16)
        self.by_target = {}
        for o, t in self.rel.items():
            self.by_target.setdefault(t, []).append(o)
        # PT_LOAD segments, to map between file offsets and VAs.
        self.loads = []
        out = subprocess.run(['llvm-readelf', '-l', '--wide', path], capture_output=True, text=True).stdout
        for l in out.splitlines():
            p = l.split()
            if p and p[0] == 'LOAD':
                self.loads.append((int(p[1], 16), int(p[2], 16), int(p[4], 16)))  # off, vaddr, filesz

    def va_of_off(self, o):
        for off, va, sz in self.loads:
            if off <= o < off + sz:
                return va + o - off

    def vtable(self, itanium_name):
        o = self.data.find(b'\0' + itanium_name.encode() + b'\0') + 1
        name_va = self.va_of_off(o)
        res = []
        for ti_name in self.by_target.get(name_va, []):
            ti = ti_name - 8
            for ref in self.by_target.get(ti, []):
                if self.data_q(ref - 8) == 0 and (ref - 8) not in self.rel:
                    vt = ref + 8
                    n = 0
                    while vt + 8 * n in self.rel:
                        n += 1
                    res.append((vt, n))
        res.sort(key=lambda v: -v[1])
        return res[0]

    def data_q(self, va):
        for off, v, sz in self.loads:
            if v <= va < v + sz:
                return int.from_bytes(self.data[off + va - v: off + va - v + 8], 'little')

    def slot(self, vt, i):
        return self.rel.get(vt + 8 * i)

    def dis(self, va, n=0x80, stop_at_ret=True):
        if self._dis is None:
            self._dis = Disasm(self.path)
        return self._dis.dis(va, n, stop_at_ret)

    def text(self, va, n=0x80, stop_at_ret=True):
        return '\n'.join(f'  {a:x}: {i}' for a, i in self.dis(va, n, stop_at_ret))
