// The engine reads input through an unfiltered PeekMessage(NULL, 0, 0, PM_REMOVE) loop once a frame.
// An 8 kHz mouse puts a WM_INPUT and a cursor move in that queue every 125 us. Each retrieval is a
// kernel transition, and each cursor move also has win32k send WM_NCHITTEST and WM_SETCURSOR to the
// window proc on the way out, so the loop retrieves tens of thousands of messages a second and runs
// long past where it would otherwise be empty. Handling them cheaply doesn't help; not retrieving
// them does. So, while the fix is on and the game is in front: the raw queue is read in one
// GetRawInputBuffer batch at the start of each pump and WM_INPUT is never retrieved, the coalesced
// WM_MOUSEMOVE and WM_NCMOUSEMOVE are taken at most once per pump (they carry the latest position),
// and everything else comes through range-filtered peeks as before.
#include "pollfix.h"
#include "utils.h"
#include "settings.h"
#include "game_layout.h"
#include "sigs.h"
#include "Detours/src/detours.h"

#include <intrin.h>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace pollfix {
namespace {

typedef BOOL(WINAPI* PeekMessageFn)(LPMSG, HWND, UINT, UINT, UINT);
PeekMessageFn oPeekMessageA;
PeekMessageFn oPeekMessageW;

int (*g_drain)();
bool (*g_rawActive)();
LONGLONG g_freq;
DWORD g_gameThread;
HWND g_gameWindow;
MemRange g_callers[2];  // inputsystem.dll and engine.dll: only their pumps are filtered

// Distinct PeekMessage shapes seen on the game thread, to know what the pump really looks like.
struct Shape { HWND hwnd; UINT min, max, flags; const char* from; unsigned count; };

struct Stats {
	LONGLONG since;
	unsigned pumps, filtered, slowPumps, calls;
	LONGLONG inPeek, inPump, maxPump;
	unsigned msgInput, msgMouseMove, msgNcMouseMove, msgMouseOther, msgKey, msgOther;
	unsigned procInput, procMouseMove, procHitTest, procSetCursor, procOther;
	unsigned drains, rawReports;
	unsigned noLegacyPumps, reregisters, postedButtons;
	unsigned offNotFront, offNoRaw, offCursor, cursorHidden, cursorClipped;
	Shape shapes[8];
	int shapeCount;
};
Stats g_s;
std::vector<float> g_frametimes;  // ms between pump starts
const size_t kMaxFrametimes = 32768;

// A pump runs from the first call after one returned FALSE to the next FALSE.
bool g_inPump;
bool g_filtering;
LONGLONG g_pumpStart;
bool g_tookMouseMove, g_tookNcMouseMove, g_tookUnfiltered;

// No-legacy mode: the mouse registered with RIDEV_NOLEGACY while looking around, so Windows stops
// making a WM_MOUSEMOVE (with its WM_NCHITTEST and WM_SETCURSOR) for every report. Filtering the
// pump still leaves win32k doing that work per report; this is the only way to stop it.
bool g_noLegacyWanted;
bool g_noLegacyOn;
WPARAM g_buttonsDown;  // MK_* we've posted down and not up

LONGLONG Now()
{
	LARGE_INTEGER t;
	QueryPerformanceCounter(&t);
	return t.QuadPart;
}

bool FindMouseRegistration(RAWINPUTDEVICE& out)
{
	RAWINPUTDEVICE devs[16];
	UINT n = 16;
	n = GetRegisteredRawInputDevices(devs, &n, sizeof(RAWINPUTDEVICE));
	if (n == (UINT)-1) return false;
	for (UINT i = 0; i < n; ++i) {
		if (devs[i].usUsagePage == 1 && devs[i].usUsage == 2) {
			out = devs[i];
			return true;
		}
	}
	return false;
}

void PostButton(UINT msg, WPARAM extra = 0)
{
	POINT p;
	GetCursorPos(&p);
	ScreenToClient(g_gameWindow, &p);
	PostMessageW(g_gameWindow, msg, g_buttonsDown | extra, MAKELPARAM(p.x, p.y));
	g_s.postedButtons++;
}

void ReleaseButtons()
{
	const struct { WPARAM mk; UINT up; WPARAM x; } buttons[] = {
		{ MK_LBUTTON, WM_LBUTTONUP, 0 }, { MK_RBUTTON, WM_RBUTTONUP, 0 }, { MK_MBUTTON, WM_MBUTTONUP, 0 },
		{ MK_XBUTTON1, WM_XBUTTONUP, MAKEWPARAM(0, XBUTTON1) }, { MK_XBUTTON2, WM_XBUTTONUP, MAKEWPARAM(0, XBUTTON2) },
	};
	for (const auto& b : buttons) {
		if (!(g_buttonsDown & b.mk)) continue;
		g_buttonsDown &= ~b.mk;
		PostButton(b.up, b.x);
	}
}

// The game is looking around rather than showing a menu: the cursor is hidden, or clipped to a
// part of the game window. Each is counted, to learn which one CS:S actually gives.
bool InGame()
{
	CURSORINFO ci = { sizeof(ci) };
	const bool hidden = GetCursorInfo(&ci) && (!(ci.flags & CURSOR_SHOWING) || !ci.hCursor);
	RECT clip, win;
	const bool clipped = GetClipCursor(&clip) && GetWindowRect(g_gameWindow, &win)
		&& clip.left >= win.left && clip.top >= win.top && clip.right <= win.right && clip.bottom <= win.bottom
		&& (clip.right - clip.left < GetSystemMetrics(SM_CXVIRTUALSCREEN) || clip.bottom - clip.top < GetSystemMetrics(SM_CYVIRTUALSCREEN));
	if (hidden) g_s.cursorHidden++;
	if (clipped) g_s.cursorClipped++;
	return hidden || clipped;
}

// Once per pump: no-legacy only while the game is in front and looking around (menus need the
// cursor messages) and our raw path sees every report (it posts the buttons).
void UpdateNoLegacy()
{
	bool want = false;
	if (!g_noLegacyWanted || !g_gameWindow) {
	} else if (GetForegroundWindow() != g_gameWindow) {
		g_s.offNotFront++;
	} else if (!g_rawActive || !g_rawActive()) {
		g_s.offNoRaw++;
	} else if (!InGame()) {
		g_s.offCursor++;
	} else {
		want = true;
	}
	if (want == g_noLegacyOn) return;
	RAWINPUTDEVICE d;
	if (!FindMouseRegistration(d)) return;
	d.dwFlags = want ? (d.dwFlags | RIDEV_NOLEGACY) : (d.dwFlags & ~RIDEV_NOLEGACY);
	if (!RegisterRawInputDevices(&d, 1, sizeof(d))) {
		CoreLog("no-legacy: RegisterRawInputDevices failed (%lu), turning it off", GetLastError());
		g_noLegacyWanted = false;
		return;
	}
	g_noLegacyOn = want;
	g_s.reregisters++;
	if (!want) ReleaseButtons();
}

double Ms(LONGLONG ticks) { return ticks * 1000.0 / g_freq; }

const char* CallerName(const void* ret)
{
	const uintptr_t r = reinterpret_cast<uintptr_t>(ret);
	if (r >= g_callers[0].start && r < g_callers[0].end) return "inputsystem";
	if (r >= g_callers[1].start && r < g_callers[1].end) return "engine";
	return nullptr;
}

void NoteShape(HWND hwnd, UINT min, UINT max, UINT flags, const char* from)
{
	for (int i = 0; i < g_s.shapeCount; ++i) {
		Shape& s = g_s.shapes[i];
		if (s.hwnd == hwnd && s.min == min && s.max == max && s.flags == flags && s.from == from) { s.count++; return; }
	}
	if (g_s.shapeCount < 8) g_s.shapes[g_s.shapeCount++] = { hwnd, min, max, flags, from, 1 };
}

void Count(const MSG& m)
{
	const UINT u = m.message;
	if (u == WM_INPUT) g_s.msgInput++;
	else if (u == WM_MOUSEMOVE) g_s.msgMouseMove++;
	else if (u == WM_NCMOUSEMOVE) g_s.msgNcMouseMove++;
	else if ((u >= WM_MOUSEFIRST && u <= WM_MOUSELAST) || (u >= WM_NCLBUTTONDOWN && u <= WM_NCXBUTTONDBLCLK)) g_s.msgMouseOther++;
	else if (u >= WM_KEYFIRST && u <= WM_KEYLAST) g_s.msgKey++;
	else g_s.msgOther++;
}

BOOL Peek(PeekMessageFn fn, LPMSG msg, HWND hwnd, UINT min, UINT max, UINT flags)
{
	const LONGLONG t0 = Now();
	const BOOL r = fn(msg, hwnd, min, max, flags);
	g_s.inPeek += Now() - t0;
	g_s.calls++;
	if (r) Count(*msg);
	return r;
}

BOOL Pump(PeekMessageFn fn, LPMSG msg, HWND hwnd, UINT flags)
{
	if (!g_inPump) {
		g_inPump = true;
		const LONGLONG now = Now();
		if (g_pumpStart && g_frametimes.size() < kMaxFrametimes)
			g_frametimes.push_back((float)Ms(now - g_pumpStart));
		g_pumpStart = now;
		g_s.pumps++;
		g_tookMouseMove = g_tookNcMouseMove = g_tookUnfiltered = false;
		UpdateNoLegacy();
		if (g_noLegacyOn) g_s.noLegacyPumps++;
		g_filtering = g_drain && settings::Get().pollfix && GetForegroundWindow() == g_gameWindow;
		if (g_filtering) {
			g_s.filtered++;
			g_drain();
		}
	}
	BOOL r;
	if (!g_filtering) {
		r = Peek(fn, msg, hwnd, 0, 0, flags);
	} else {
		// Everything but WM_NCMOUSEMOVE (0xA0), WM_INPUT (0xFF) and WM_MOUSEMOVE (0x200).
		r = Peek(fn, msg, hwnd, 0, WM_NCMOUSEMOVE - 1, flags)
			|| Peek(fn, msg, hwnd, WM_NCMOUSEMOVE + 1, WM_INPUT - 1, flags)
			|| Peek(fn, msg, hwnd, WM_INPUT + 1, WM_MOUSEMOVE - 1, flags)
			|| Peek(fn, msg, hwnd, WM_MOUSEMOVE + 1, 0xFFFFFFFF, flags);
		if (!r && !g_tookMouseMove) {
			g_tookMouseMove = true;
			r = Peek(fn, msg, hwnd, WM_MOUSEMOVE, WM_MOUSEMOVE, flags);
		}
		if (!r && !g_tookNcMouseMove) {
			g_tookNcMouseMove = true;
			r = Peek(fn, msg, hwnd, WM_NCMOUSEMOVE, WM_NCMOUSEMOVE, flags);
		}
		// Raw input the batch read didn't take (WM_INPUT_DEVICE_CHANGE arrives through the raw queue,
		// and a ranged peek can't retrieve it): one plain peek per pump, so it reaches the engine.
		if (!r && !g_tookUnfiltered && (HIWORD(GetQueueStatus(QS_RAWINPUT)) & QS_RAWINPUT)) {
			g_tookUnfiltered = true;
			r = Peek(fn, msg, hwnd, 0, 0, flags);
		}
	}
	if (!r) {
		g_inPump = false;
		const LONGLONG d = Now() - g_pumpStart;
		g_s.inPump += d;
		if (d > g_s.maxPump) g_s.maxPump = d;
		if (d * 1000 > g_freq) g_s.slowPumps++;
	}
	return r;
}

BOOL Hook(PeekMessageFn fn, LPMSG msg, HWND hwnd, UINT min, UINT max, UINT flags, const void* ret)
{
	if (GetCurrentThreadId() != g_gameThread)
		return fn(msg, hwnd, min, max, flags);
	const char* from = CallerName(ret);
	NoteShape(hwnd, min, max, flags, from);
	const bool pump = from && min == 0 && max == 0 && (flags & ~PM_NOYIELD) == PM_REMOVE
		&& (hwnd == NULL || hwnd == g_gameWindow);
	if (!pump)
		return Peek(fn, msg, hwnd, min, max, flags);
	return Pump(fn, msg, hwnd, flags);
}

BOOL WINAPI Hooked_PeekMessageA(LPMSG msg, HWND hwnd, UINT min, UINT max, UINT flags)
{
	return Hook(oPeekMessageA, msg, hwnd, min, max, flags, _ReturnAddress());
}

BOOL WINAPI Hooked_PeekMessageW(LPMSG msg, HWND hwnd, UINT min, UINT max, UINT flags)
{
	return Hook(oPeekMessageW, msg, hwnd, min, max, flags, _ReturnAddress());
}

void Cmd_PumpStats(const CCommandX&)
{
	const LONGLONG now = Now();
	const double secs = g_s.since ? (now - g_s.since) / (double)g_freq : 0.0;
	if (secs <= 0.0 || !g_s.pumps) {
		CoreLog("pump stats: nothing yet");
		return;
	}
	double avg = 0.0, low1 = 0.0, worst = 0.0;
	if (!g_frametimes.empty()) {
		std::vector<float> sorted(g_frametimes);
		std::sort(sorted.begin(), sorted.end());
		for (float f : sorted) avg += f;
		avg /= sorted.size();
		low1 = sorted[sorted.size() * 99 / 100];
		worst = sorted.back();
	}
	CoreLog("pump stats over %.1f s, high polling rate fix %s (%u of %u pumps filtered)",
		secs, settings::Get().pollfix ? (g_drain ? "on" : "on but unavailable") : "off", g_s.filtered, g_s.pumps);
	CoreLog("  %u pumps (%.0f/s); frametime avg %.2f ms, 1%% low %.2f ms (%.0f fps), worst %.2f ms",
		g_s.pumps, g_s.pumps / secs, avg, low1, low1 > 0.0 ? 1000.0 / low1 : 0.0, worst);
	CoreLog("  PeekMessage: %u calls (%.1f per pump), %.1f ms inside (%.1f%% of the time); pump loops %.1f ms (%.1f%%), %u over 1 ms, longest %.2f ms",
		g_s.calls, g_s.calls / (double)g_s.pumps, Ms(g_s.inPeek), Ms(g_s.inPeek) / (secs * 10.0),
		Ms(g_s.inPump), Ms(g_s.inPump) / (secs * 10.0), g_s.slowPumps, Ms(g_s.maxPump));
	CoreLog("  retrieved: %u WM_INPUT, %u WM_MOUSEMOVE, %u WM_NCMOUSEMOVE, %u other mouse, %u keyboard, %u other",
		g_s.msgInput, g_s.msgMouseMove, g_s.msgNcMouseMove, g_s.msgMouseOther, g_s.msgKey, g_s.msgOther);
	CoreLog("  window proc: %u WM_INPUT, %u WM_MOUSEMOVE, %u WM_NCHITTEST, %u WM_SETCURSOR, %u other; raw: %u reports in %u batch reads",
		g_s.procInput, g_s.procMouseMove, g_s.procHitTest, g_s.procSetCursor, g_s.procOther, g_s.rawReports, g_s.drains);
	CoreLog("  no-legacy %s: on for %u of %u pumps, %u re-registrations, %u buttons/wheel posted",
		g_noLegacyWanted ? "wanted" : "off", g_s.noLegacyPumps, g_s.pumps, g_s.reregisters, g_s.postedButtons);
	if (g_noLegacyWanted)
		CoreLog("  no-legacy off because: %u not in front, %u raw input not active, %u in a menu; in game by: %u cursor hidden, %u cursor clipped",
			g_s.offNotFront, g_s.offNoRaw, g_s.offCursor, g_s.cursorHidden, g_s.cursorClipped);
	for (int i = 0; i < g_s.shapeCount; ++i) {
		const Shape& s = g_s.shapes[i];
		CoreLog("  PeekMessage(hwnd=%p, %u..%u, flags 0x%x) from %s: %u calls", s.hwnd, s.min, s.max, s.flags, s.from ? s.from : "elsewhere", s.count);
	}
	g_s = {};
	g_s.since = now;
	g_frametimes.clear();
}

void Cmd_PollFix(const CCommandX& c)
{
	if (c.argc > 1) {
		settings::Get().pollfix = atoi(c.argv[1]) != 0;
		settings::Changed();
	}
	CoreLog("bhoptools_pollfix %d%s", settings::Get().pollfix ? 1 : 0, g_drain ? "" : " (unavailable, see the missing signature above)");
}

void Cmd_NoLegacy(const CCommandX& c)
{
	if (c.argc > 1) g_noLegacyWanted = atoi(c.argv[1]) != 0;
	CoreLog("bhoptools_nolegacy %d (%s now)", g_noLegacyWanted ? 1 : 0, g_noLegacyOn ? "on" : "off");
}

ConCommandX g_cmds[3];

}  // namespace

void Init(int (*drain)(), bool (*rawActive)())
{
	g_drain = drain;
	g_rawActive = rawActive;
	LARGE_INTEGER f;
	QueryPerformanceFrequency(&f);
	g_freq = f.QuadPart;
	g_frametimes.reserve(kMaxFrametimes);
	const char* modules[2] = { "inputsystem.dll", "engine.dll" };
	for (int i = 0; i < 2; ++i) {
		MODULEINFO mi{};
		HMODULE h = GetModuleHandleA(modules[i]);
		if (h && GetModuleInformation(GetCurrentProcess(), h, &mi, sizeof(mi)))
			g_callers[i] = { (uintptr_t)mi.lpBaseOfDll, (uintptr_t)mi.lpBaseOfDll + mi.SizeOfImage };
	}
	HMODULE user32 = GetModuleHandleA("user32.dll");
	oPeekMessageA = user32 ? (PeekMessageFn)GetProcAddress(user32, "PeekMessageA") : nullptr;
	oPeekMessageW = user32 ? (PeekMessageFn)GetProcAddress(user32, "PeekMessageW") : nullptr;
	if (!oPeekMessageA || !oPeekMessageW) {
		CoreLog("high polling rate fix: PeekMessage not found, unavailable");
		g_drain = nullptr;
	} else if (!g_drain) {
		CoreLog("high polling rate fix: unavailable (the raw input accumulators weren't found), only the pump counters run");
	} else {
		CoreLog("high polling rate fix: %s (Options > bhoptools, or bhoptools_pollfix 0/1)", settings::Get().pollfix ? "on" : "off");
	}
	g_s.since = Now();
}

void Attach()
{
	if (oPeekMessageA) DetourAttach(&(PVOID&)oPeekMessageA, Hooked_PeekMessageA);
	if (oPeekMessageW) DetourAttach(&(PVOID&)oPeekMessageW, Hooked_PeekMessageW);
}

void Detach()
{
	if (oPeekMessageA) DetourDetach(&(PVOID&)oPeekMessageA, Hooked_PeekMessageA);
	if (oPeekMessageW) DetourDetach(&(PVOID&)oPeekMessageW, Hooked_PeekMessageW);
}

void OnWindowMessage(HWND hwnd, UINT msg)
{
	if (!g_gameWindow) {
		g_gameWindow = hwnd;
		g_gameThread = GetCurrentThreadId();
	}
	if (msg == WM_INPUT) g_s.procInput++;
	else if (msg == WM_MOUSEMOVE) g_s.procMouseMove++;
	else if (msg == WM_NCHITTEST) g_s.procHitTest++;
	else if (msg == WM_SETCURSOR) g_s.procSetCursor++;
	else g_s.procOther++;
}

void OnRawMouse(const RAWMOUSE& m)
{
	if (!g_noLegacyOn || !m.usButtonFlags) return;
	const USHORT f = m.usButtonFlags;
	const struct { USHORT down, up; WPARAM mk; UINT msgDown, msgUp; WPARAM x; } buttons[] = {
		{ RI_MOUSE_LEFT_BUTTON_DOWN, RI_MOUSE_LEFT_BUTTON_UP, MK_LBUTTON, WM_LBUTTONDOWN, WM_LBUTTONUP, 0 },
		{ RI_MOUSE_RIGHT_BUTTON_DOWN, RI_MOUSE_RIGHT_BUTTON_UP, MK_RBUTTON, WM_RBUTTONDOWN, WM_RBUTTONUP, 0 },
		{ RI_MOUSE_MIDDLE_BUTTON_DOWN, RI_MOUSE_MIDDLE_BUTTON_UP, MK_MBUTTON, WM_MBUTTONDOWN, WM_MBUTTONUP, 0 },
		{ RI_MOUSE_BUTTON_4_DOWN, RI_MOUSE_BUTTON_4_UP, MK_XBUTTON1, WM_XBUTTONDOWN, WM_XBUTTONUP, MAKEWPARAM(0, XBUTTON1) },
		{ RI_MOUSE_BUTTON_5_DOWN, RI_MOUSE_BUTTON_5_UP, MK_XBUTTON2, WM_XBUTTONDOWN, WM_XBUTTONUP, MAKEWPARAM(0, XBUTTON2) },
	};
	for (const auto& b : buttons) {
		if (f & b.down) { g_buttonsDown |= b.mk; PostButton(b.msgDown, b.x); }
		if (f & b.up) { g_buttonsDown &= ~b.mk; PostButton(b.msgUp, b.x); }
	}
	if (f & RI_MOUSE_WHEEL) {
		// Wheel messages carry screen coordinates, unlike the buttons.
		POINT p;
		GetCursorPos(&p);
		PostMessageW(g_gameWindow, WM_MOUSEWHEEL, MAKEWPARAM(g_buttonsDown, (SHORT)m.usButtonData), MAKELPARAM(p.x, p.y));
		g_s.postedButtons++;
	}
}

void CountRaw(int reports)
{
	if (reports <= 0) return;
	g_s.drains++;
	g_s.rawReports += reports;
}

void RegisterCommands()
{
	static bool done;
	if (done) return;
	done = true;
	void* cvar = CreateInterfaceIn(VSTDLIB_MODULE, "VEngineCvar004");
	void* echo = cvar ? VFunc<void* (*)(void*, const char*)>(cvar, VT_Cvar_FindBase)(cvar, "echo") : nullptr;
	const char* echoName = echo ? static_cast<ConCommandX*>(echo)->name : nullptr;
	if (!echo || !VFunc<bool (*)(void*)>(echo, VT_CCB_IsCommand)(echo) || !echoName || strcmp(echoName, "echo")) {
		CoreLog("pollfix: couldn't register bhoptools_pumpstats and bhoptools_pollfix");
		return;
	}
	const struct { const char* name; const char* help; void (*fn)(const CCommandX&); } defs[3] = {
		{ "bhoptools_pumpstats", "Print and reset what the message pump cost since the last print.", Cmd_PumpStats },
		{ "bhoptools_pollfix", "High polling rate fix: filter the message pump the way Godot does (0/1, off by default).", Cmd_PollFix },
		{ "bhoptools_nolegacy", "Stop Windows making cursor messages for every mouse report while looking around (0/1, off by default).", Cmd_NoLegacy },
	};
	for (int i = 0; i < 3; ++i) {
		ConCommandX& c = g_cmds[i];
		c.vtable = *reinterpret_cast<void**>(echo);
		c.name = defs[i].name;
		c.help = defs[i].help;
		c.callback = reinterpret_cast<void*>(defs[i].fn);
		c.bits = 0x02;  // new-style callback taking const CCommand&
		VFunc<void (*)(void*, void*)>(cvar, VT_Cvar_Register)(cvar, &c);
	}
}

}
