import os
import re
from elf import ELF
from pe import PE

LIN = os.path.expanduser('~/.local/share/Steam/steamapps/common/Counter-Strike Source/bin/linux64/')
WIN = os.path.expanduser('~/.cache/bhoptools-bins/steam/client-windows/bin/x64/')


def vcalls(mod, fn, n=0x400):
    """Slot indices of every call/jmp through a vtable ([reg + disp]) in the first n bytes, in order."""
    out = []
    for _, ins in mod.dis(fn, n, stop_at_ret=False):
        m = re.match(r'(call|jmp) qword ptr \[r\w+( \+ (0x[0-9a-f]+))?\]$', ins)
        if m:
            out.append(int(m.group(3) or '0', 16) // 8)
    return out


def cstr_at(mod, va):
    if isinstance(mod, PE):
        b = mod.read(va, 64)
    else:
        b = None
        for off, v, sz in mod.loads:
            if v <= va < v + sz:
                b = mod.data[off + va - v: off + va - v + 64]
    if not b or b'\0' not in b:
        return None
    s = b.split(b'\0')[0]
    return s.decode() if len(s) >= 3 and all(32 <= c < 127 for c in s) else None


def strrefs(mod, fn, n=0x200):
    out = []
    for _, ins in mod.dis(fn, n, stop_at_ret=False):
        m = re.match(r'lea \w+, \[rip [+-] 0x[0-9a-f]+\] # (0x[0-9a-f]+)', ins)
        if m:
            s = cstr_at(mod, int(m.group(1), 16))
            if s:
                out.append(s)
    return out
