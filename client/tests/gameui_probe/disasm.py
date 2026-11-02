"""Whole-module disassembly through llvm-objdump, cached next to this file, indexed by address."""
import bisect
import hashlib
import os
import re
import subprocess

HERE = os.path.dirname(os.path.abspath(__file__))


class Disasm:
    def __init__(self, path):
        key = hashlib.md5((path + str(os.path.getmtime(path))).encode()).hexdigest()[:12]
        cache = os.path.join(HERE, 'cache', os.path.basename(path) + '.' + key + '.txt')
        if not os.path.exists(cache):
            os.makedirs(os.path.dirname(cache), exist_ok=True)
            out = subprocess.run(['llvm-objdump', '-d', '--no-show-raw-insn', '-M', 'intel', path],
                                 capture_output=True, text=True).stdout
            with open(cache, 'w') as f:
                for l in out.splitlines():
                    m = re.match(r'\s*([0-9a-f]+):\s+(.*)', l)
                    if m:
                        ins = re.sub(r'\s*<[^>]*>', '', re.sub(r'\s+', ' ', m.group(2))).strip()
                        f.write(f'{m.group(1)} {ins}\n')
        self.path = path
        self.addrs, self.ins = [], []
        for l in open(cache):
            a, _, i = l.rstrip('\n').partition(' ')
            self.addrs.append(int(a, 16))
            self.ins.append(i)

    def dis(self, va, n=0x80, stop_at_ret=True):
        i = bisect.bisect_left(self.addrs, va)
        if i >= len(self.addrs) or self.addrs[i] != va:
            return self._exact(va, n, stop_at_ret)
        out = []
        while i < len(self.addrs) and self.addrs[i] < va + n:
            out.append((self.addrs[i], self.ins[i]))
            s = self.ins[i]
            if stop_at_ret and (s == 'ret' or s.startswith('jmp ') or s.startswith('int3')):
                break
            i += 1
        return out

    def text(self, va, n=0x80, stop_at_ret=True):
        return '\n'.join(f'  {a:x}: {i}' for a, i in self.dis(va, n, stop_at_ret))

    def _exact(self, va, n, stop_at_ret):
        """The linear sweep lost sync here (padding or data in .text), so disassemble from va itself."""
        out = subprocess.run(['llvm-objdump', '-d', '--no-show-raw-insn', '-M', 'intel',
                              f'--start-address={va:#x}', f'--stop-address={va + n:#x}', self.path],
                             capture_output=True, text=True).stdout
        res = []
        for l in out.splitlines():
            m = re.match(r'\s*([0-9a-f]+):\s+(.*)', l)
            if not m:
                continue
            ins = re.sub(r'\s*<[^>]*>', '', re.sub(r'\s+', ' ', m.group(2))).strip()
            res.append((int(m.group(1), 16), ins))
            if stop_at_ret and (ins == 'ret' or ins.startswith('jmp ') or ins.startswith('int3')):
                break
        return res
