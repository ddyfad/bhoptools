"""Checks the Options-tab slots (linux/sigs.h and windows/sigs.h) and offsets against the installed Linux .so files and the Windows x64 DLLs,
each from code evidence rather than slot arithmetic. Run: python3 probe.py"""
import re
import sys

from common import LIN, WIN, ELF, PE, vcalls, strrefs

checks = fails = 0


def check(cond, what):
    global checks, fails
    checks += 1
    fails += not cond
    print(('  ok    ' if cond else '  FAIL  ') + what)


def ins(mod, fn, n=0x60):
    return [x for _, x in mod.dis(fn, n)]


def reads(mod, fn, n=0x100):
    """Displacements of [rdi/rcx + disp] reads in the first n bytes."""
    return [int(m.group(1), 16) for x in ins(mod, fn, n) for m in [re.search(r'\[r(?:di|cx) \+ (0x[0-9a-f]+)\]', x)] if m]


for plat, mod_ui, mod_vgui, mod_surf, rtti in (
    ('linux', ELF(LIN + 'GameUI.so'), ELF(LIN + 'vgui2.so'), ELF(LIN + 'vguimatsurface.so'),
     lambda n: {'Panel': 'N4vgui5PanelE', 'Label': 'N4vgui5LabelE', 'PageTab': 'N4vgui7PageTabE',
                'PropertySheet': 'N4vgui13PropertySheetE', 'TextImage': 'N4vgui9TextImageE',
                'COptionsDialog': '14COptionsDialog', 'COptionsSubMultiplayer': '22COptionsSubMultiplayer',
                'COptionsSubMouse': '16COptionsSubMouse', 'COptionsSubAudio': '16COptionsSubAudio',
                'COptionsSubVoice': '16COptionsSubVoice', 'CInputSystem': '12CInputSystem',
                'VPanelWrapper': '13VPanelWrapper', 'CMatSystemSurface': '17CMatSystemSurface'}[n]),
    ('windows', PE(WIN + 'GameUI.dll'), PE(WIN + 'vgui2.dll'), PE(WIN + 'vguimatsurface.dll'),
     lambda n: {'Panel': '.?AVPanel@vgui@@', 'Label': '.?AVLabel@vgui@@', 'PageTab': '.?AVPageTab@vgui@@',
                'PropertySheet': '.?AVPropertySheet@vgui@@', 'TextImage': '.?AVTextImage@vgui@@',
                'COptionsDialog': '.?AVCOptionsDialog@@', 'COptionsSubMultiplayer': '.?AVCOptionsSubMultiplayer@@',
                'COptionsSubMouse': '.?AVCOptionsSubMouse@@', 'COptionsSubAudio': '.?AVCOptionsSubAudio@@',
                'COptionsSubVoice': '.?AVCOptionsSubVoice@@', 'CInputSystem': '.?AVCInputSystem@@',
                'VPanelWrapper': '.?AVVPanelWrapper@@', 'CMatSystemSurface': '.?AVCMatSystemSurface@@'}[n]),
):
    win = plat == 'windows'
    d = 1 if win else 0  # the one destructor slot MSVC does not emit
    print(f'\n== {plat}')
    vt = {}
    for mod, names in ((mod_ui, ['Panel', 'Label', 'PageTab', 'PropertySheet', 'TextImage', 'COptionsDialog',
                                 'COptionsSubMultiplayer', 'COptionsSubMouse', 'COptionsSubAudio', 'COptionsSubVoice']),
                       (mod_vgui, ['CInputSystem', 'VPanelWrapper']), (mod_surf, ['CMatSystemSurface'])):
        for n in names:
            try:
                vt[n] = mod.vtable(rtti(n))
                print(f'  ok    {n:24} vtable {vt[n][0]:#x}, {vt[n][1]} slots')
            except Exception as ex:
                check(False, f'{n} vtable: {ex}')
    S = lambda n, i: (mod_ui if n in ('Panel', 'Label', 'PageTab', 'PropertySheet', 'TextImage', 'COptionsDialog',
                                      'COptionsSubMultiplayer') else mod_vgui if n in ('CInputSystem', 'VPanelWrapper')
                      else mod_surf).slot(vt[n][0], i)
    g = mod_ui

    # Panel: PaintTraverse (slot 3, before any destructor) calls PaintBackground, Paint, PaintBorder, PostChildPaint in that
    # order, each bracketed by the surface's PushMakeCurrent (8) / PopMakeCurrent (9).
    paint = 131 - d
    trav = vcalls(g, S('Panel', 3), 0x600)
    seq = [trav[i + 1] for i in range(len(trav) - 2) if trav[i] == 8 and trav[i + 2] == 9]
    check([paint - 1, paint] == [s for s in seq if s in (paint - 1, paint)][:2],
          f'Panel: PaintTraverse calls PaintBackground {paint - 1} then Paint {paint} (bracketed calls {seq})')
    check(S('COptionsSubMultiplayer', paint) == S('Panel', paint),
          f'COptionsSubMultiplayer: slot {paint} is Panel\'s own Paint, not overridden')
    # InvalidateLayout(layoutNow, reloadScheme): recurses into itself, PerformApplySchemeSettings (2), then PerformLayout.
    layout = 135 - d
    inv = 67 - d
    v = vcalls(g, S('Panel', inv), 0x300)
    check(inv in v and 2 in v and layout in v and v.index(layout) > v.index(2),
          f'Panel: slot {inv} is InvalidateLayout, calling itself, 2 and then PerformLayout {layout} ({v})')
    check(S('PropertySheet', layout) != S('Panel', layout), f'PropertySheet: slot {layout} is its own PerformLayout')
    check(bool(re.match(r'mov [re]ax, (qword|dword) ptr \[r(di|cx) \+ 0x', ins(g, S('Panel', 0))[0])),
          'Panel: slot 0 is GetVPanel, returning a member')

    # Label: SetText(const char*) swaps a null for "" and tests for '#'; GetText forwards to TextImage::GetText, and the
    # char TextImage::GetText scans bytes (strlen/toupper) where the wchar_t one scans 16-bit words.
    set_text, get_text = 214, 216
    t = ins(g, S('Label', set_text), 0x80)
    check(any(re.search(r'cmp byte ptr \[\w+\], 0x23', x) for x in t),
          f'Label: slot {set_text} is SetText(const char*): null -> "", then text[0] == \'#\'')
    other_set = 213 if win else 215
    t = ins(g, S('Label', other_set), 0x80)
    check(any('word ptr' in x for x in t), f'Label: slot {other_set} is SetText(const wchar_t*, bool), comparing words')
    for slot, want in ((get_text, 'byte'), (get_text - 1 if win else get_text + 1, 'word')):
        fwd = ins(g, S('Label', slot), 0x20)
        m = re.search(r'jmp qword ptr \[rax \+ (0x[0-9a-f]+)\]', ' '.join(fwd))
        ti = int(m.group(1), 16) // 8 if m else None
        body = ' '.join(ins(g, S('TextImage', ti), 0x100)) if ti else ''
        scan = 'byte' if re.search(r'cmp byte ptr \[r\w+ \+ r\w+\], 0x0|movsx \w+, byte ptr', body) else \
               'word' if re.search(r'cmp word ptr \[r\w+ \+ 2\*r\w+\], 0x0', body) or 'shr' in body else '?'
        check(scan == want, f'Label: slot {slot} forwards to TextImage slot {ti}, which scans {scan}s '
                            f'-> GetText({"char" if want == "byte" else "wchar_t"}*, int)')
    check(S('PageTab', set_text) == S('Label', set_text) and S('PageTab', get_text) == S('Label', get_text),
          'PageTab: SetText and GetText are Label\'s')

    get_active, set_active = 243 - d, 241 - d
    r = reads(g, S('PropertySheet', get_active), 0x10)
    t = ins(g, S('PropertySheet', get_active), 0x10)
    check(len(t) == 2 and t[1] == 'ret' and len(r) == 1, f'PropertySheet: slot {get_active} is GetActivePage, returning _activePage at {r[0]:#x}')
    active = r[0]
    pages, tabs = active - 0x40, active - 0x20
    r = reads(g, S('PropertySheet', set_active), 0x60)
    check(pages in r and pages + 0x10 in r,
          f'PropertySheet: slot {set_active} is SetActivePage, walking m_Pages at {pages:#x} (count {pages + 0x10:#x})')
    for slot, want in ((253 - d, get_text), (254 - d, set_text)):
        r = reads(g, S('PropertySheet', slot), 0x50)
        v = vcalls(g, S('PropertySheet', slot), 0x50)
        check(tabs in r and tabs + 0x10 in r and want in v,
              f'PropertySheet: slot {slot} reads m_PageTabs at {tabs:#x} (count {tabs + 0x10:#x}) and calls the tab\'s slot {want}')

    # Dropped pages exist.
    for n in ('COptionsDialog', 'COptionsSubMouse', 'COptionsSubAudio', 'COptionsSubVoice'):
        check(n in vt, f'{n} found by RTTI')

    # Panel input handlers: the default OnMouseWheeled/OnKey* forward to the parent with a KeyValues named after them.
    # Windows' OnKeyCodePressed runs its keyboard-navigation code first, so it is pinned below through InternalKeyCodePressed.
    for name, lin, keys in (('OnMouseWheeled', 111, ['delta', 'MouseWheeled']),
                            ('OnKeyCodeTyped', 125, ['code', 'KeyCodeTyped']),
                            ('OnKeyTyped', 126, ['unichar', 'KeyTyped'])):
        slot = lin - d
        f = S('Panel', slot)
        s = strrefs(g, f, 0x180)
        check(any(s[i:i + 2] == keys for i in range(len(s))), f'Panel: slot {slot} is {name} ({s[:2]})')
    if win:
        # The Internal* message handlers call the On* slots: CursorEntered/Exited 104/105, MousePressed 106, Wheeled 110.
        for internal, target, name in ((198, 104, 'OnCursorEntered'), (199, 105, 'OnCursorExited'),
                                       (200, 106, 'OnMousePressed'), (201, 107, 'OnMouseDoublePressed'),
                                       (204, 110, 'OnMouseWheeled'), (205, 123, 'OnKeyCodePressed'),
                                       (207, 125, 'OnKeyTyped')):
            f = S('Panel', internal)
            check(target in vcalls(g, f, max(g.func_size(f), 0x80)), f'Panel: Internal handler {internal} calls {name} at {target}')
    else:
        # Linux's Internal handlers are one slot later and call one slot later: MousePressed 201 -> 107.
        for internal, target, name in ((201, 107, 'OnMousePressed'), (202, 108, 'OnMouseDoublePressed'),
                                       (204, 109, 'OnMouseReleased')):
            f = S('Panel', internal)
            check(target in vcalls(g, f, 0x200), f'Panel: Internal handler {internal} calls {name} at {target}')
    for slot in ((103, 106, 108) if win else (104, 107, 109)):
        check(S('COptionsSubMultiplayer', slot) == S('Panel', slot), f'COptionsSubMultiplayer: slot {slot} is Panel\'s')

    # VGUI IInput: GetFocus and GetMouseOver read the default context's _keyFocus / _mouseOver (0x360 + 0xa18 / 0xa30).
    gf, gmo, gcp = 5 - d, 7 - d, 9 - d
    rin = lambda i: reads(mod_vgui, S('CInputSystem', i), 0x40)
    check(0xe20 in rin(gmo) and (0x360 + 0xa30 in rin(gmo) or 0xe28 in rin(gmo)), f'IInput: slot {gmo} is GetMouseOver ({[hex(x) for x in rin(gmo)]})')
    check(0xe20 in rin(gf) and (0x360 + 0xa18 in rin(gf) or 0xe28 in rin(gf)), f'IInput: slot {gf} is GetFocus')

    # GetCursorPos(int&, int&) keeps both references whole; SetCursorPos(int, int) before it keeps ints.
    t = ' '.join(ins(mod_vgui, S('CInputSystem', 9 - d), 0x40))
    t8 = ' '.join(ins(mod_vgui, S('CInputSystem', 8 - d), 0x40))
    check(('mov rdi, rdx' in t and 'mov rbx, r8' in t and 'mov esi, edx' in t8) if win
          else ('mov r13, rsi' in t and 'mov r14, rdx' in t and 'mov r12d, esi' in t8),
          f'IInput: slot {9 - d} is GetCursorPos, keeping both references (slot {8 - d} keeps ints)')

    # IPanel forwarders to VPanel (VPanel's own vtable is one slot shorter on Windows too).
    for name, lin, voff in (('GetPos', 4, 0x58), ('GetSize', 6, 0x68), ('GetAbsPos', 11, 0x90), ('SetVisible', 15, 0xb8),
                            ('IsVisible', 16, 0xc8), ('GetChildCount', 18, 0xe0), ('GetChild', 19, 0xe8),
                            ('GetParent', 21, 0xf0), ('GetName', 37, 0x118)):
        t = ' '.join(ins(mod_vgui, S('VPanelWrapper', lin - d), 0x30))
        check(f'jmp qword ptr [rax + {voff - 8 * d:#x}]' in t, f'IPanel: slot {lin - d} forwards to VPanel::{name}')

    # ISurface. The Color overloads unpack and call the int one; MSVC puts the Color overload first.
    sc_int, sc_col = (11, 10) if win else (10, 11)
    tc_int, tc_col = (19, 18) if win else (18, 19)
    for col, it, name in ((sc_col, sc_int, 'DrawSetColor'), (tc_col, tc_int, 'DrawSetTextColor')):
        t = ' '.join(ins(mod_surf, S('CMatSystemSurface', col), 0x80))
        if win:
            check(f'call qword ptr [r11 + {it * 8:#x}]' in t, f'ISurface: slot {col} is {name}(Color), calling {it} = {name}(int x4)')
        else:
            check(f'[rdx + {it * 8:#x}]' in t or 'shr r8d, 0x18' in t, f'ISurface: slot {col} is {name}(Color), unpacking, with {it} the int one')
    t = ' '.join(ins(mod_surf, S('CMatSystemSurface', 14), 0x80))
    check('[rax + 0x60]' in t, 'ISurface: slot 14 is DrawOutlinedRect, drawing through slot 12 DrawFilledRect')
    t = ins(mod_surf, S('CMatSystemSurface', 17), 0x10)
    check(len(t) == 2 and t[0].startswith('mov dword ptr') and t[1] == 'ret', 'ISurface: slot 17 is DrawSetTextFont, storing the font')
    t = ins(mod_surf, S('CMatSystemSurface', 20), 0x20)
    t2 = ins(mod_surf, S('CMatSystemSurface', 21), 0x20)
    check(t[-1] == 'ret' and len(t2) == 5, 'ISurface: slot 20 is DrawSetTextPos, slot 21 DrawGetTextPos reading it back')
    t = ' '.join(ins(mod_surf, S('CMatSystemSurface', 22), 0x80))
    check(('test rdx, rdx' if win else 'test rsi, rsi') in t, 'ISurface: slot 22 is DrawPrintText, null-checking the text')
    t = ins(mod_surf, S('CMatSystemSurface', 15), 0x60)
    check(any('cvtdq2ps' in x or 'cvtsi2ss' in x for x in t) or len(t) <= 3, 'ISurface: slot 15 is DrawLine (ints to floats; Linux: alpha test then body)')
    for slot, name in ((61, 'UnlockCursor'), (62, 'LockCursor')):
        t = ins(mod_surf, S('CMatSystemSurface', slot), 0x10)
        arg = 'xor ecx, ecx' if (win and slot == 61) else 'mov cl, 0x1' if win else 'xor edi, edi' if slot == 61 else 'mov edi, 0x1'
        check(len(t) == 2 and t[0] == arg and t[1].startswith('jmp'), f'ISurface: slot {slot} is {name}, passing {slot == 62} to one helper')
    t0 = ins(mod_surf, S('CMatSystemSurface', 66), 0x20)
    check(any(x.startswith('call') for x in t0) and t0[-1].startswith('jmp') and len(t0) <= 6,
          'ISurface: slot 66 is CreateFont, a jump into the font manager')
    # SetFontGlyphSet(font, const char* name, tall, ...) keeps the name pointer whole; GetTextSize(font, text, int& wide,
    # int& tall) keeps both references whole, where GetCharacterWidth(font, int ch) before it keeps an int.
    t = ' '.join(ins(mod_surf, S('CMatSystemSurface', 67), 0x80))
    check(('mov rdi, r8' if win else 'mov r13, rdx') in t and ('dword ptr [rbp + 0x28]' in t or win),
          'ISurface: slot 67 is SetFontGlyphSet, keeping the font name pointer')
    t = ' '.join(ins(mod_surf, S('CMatSystemSurface', 75), 0x80))
    t74 = ' '.join(ins(mod_surf, S('CMatSystemSurface', 74), 0x80))
    check(('mov rbx, r9' if win else 'mov rbx, r8') in t and ('mov ebx, r8d' in t74 if win else 'mov rbx, r8' not in t74),
          'ISurface: slot 75 is GetTextSize, passing the size references on (74 is GetCharacterWidth, an int)')

print(f'\n{checks} checks, {fails} failed')
sys.exit(1 if fails else 0)
