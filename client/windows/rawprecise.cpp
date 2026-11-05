// rawinput2 spreads the movement it has evenly over the frame's time, since all it knows is when the
// game last read the raw input queue. Here a thread of ours owns the mouse's raw input and stamps
// every report with the time it arrived, so a tick ending at time t takes exactly the reports up
// to t. Windows allows one target window per device class in a process, so while this is on the
// mouse is registered to our window (RIDEV_INPUTSINK, since a message-only window is never in
// front) and the game's own registrations of it are redirected there.
#include "rawprecise.h"
#include "pollfix.h"
#include "settings.h"
#include "mouse_split.h"
#include "utils.h"
#include "game_layout.h"
#include "sigs.h"
#include "Detours/src/detours.h"

#include <atomic>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace rawprecise {
namespace {

struct Report { LONGLONG t; int dx, dy; };

// Shared with the input thread, under g_lock.
SRWLOCK g_lock = SRWLOCK_INIT;
const size_t kCap = 65536;  // 8 s at 8 kHz; past that a report is added to the newest
Report g_ring[kCap];
size_t g_head, g_count;
std::vector<RAWMOUSE> g_buttons;
int g_reports;           // since the last FlushButtons
int g_pendX, g_pendY;    // not yet given to the old method's shadow

LONGLONG g_freq;
double (*g_plat)();
HWND g_window;           // ours, on the input thread
DWORD g_threadId;
HANDLE g_thread;
std::atomic<HWND> g_gameWindow;
UINT g_shutdownMsg;

// Game thread.
bool g_on;
bool g_shutdown;
bool g_failed;
HWND g_gameTarget;       // what the game registered the mouse to, given back when this turns off
bool g_gameSink;

// What rawinput2's even spread would have given, fed the same reports the way it reads them:
// all at once, stamped with the time of the read. For bhoptools_rawcompare.
MouseSplit g_shadow;
struct Tick { int px, py, ox, oy; };
std::vector<Tick> g_ticks;
bool g_recording;
double g_recordUntil;

typedef BOOL(WINAPI* RegisterRawInputDevicesFn)(PCRAWINPUTDEVICE, UINT, UINT);
RegisterRawInputDevicesFn oRegisterRawInputDevices = RegisterRawInputDevices;

LONGLONG Now()
{
	LARGE_INTEGER t;
	QueryPerformanceCounter(&t);
	return t.QuadPart;
}

void Add(const RAWMOUSE& m, LONGLONG t)
{
	AcquireSRWLockExclusive(&g_lock);
	g_reports++;
	if (m.lLastX || m.lLastY) {
		if (g_count < kCap) {
			g_ring[(g_head + g_count) % kCap] = { t, m.lLastX, m.lLastY };
			g_count++;
		} else {
			Report& r = g_ring[(g_head + kCap - 1) % kCap];
			r.t = t;
			r.dx += m.lLastX;
			r.dy += m.lLastY;
		}
		g_pendX += m.lLastX;
		g_pendY += m.lLastY;
	}
	if (m.usButtonFlags && g_buttons.size() < 4096)
		g_buttons.push_back(m);
	ReleaseSRWLockExclusive(&g_lock);
}

LRESULT CALLBACK InputProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	if (msg == WM_INPUT) {
		const LONGLONG t = Now();
		// RIDEV_INPUTSINK also delivers it while the game is in the background; the game's own
		// registration wouldn't have.
		const HWND game = g_gameWindow.load();
		RAWINPUT ri;
		UINT size = sizeof(ri);
		if (game && GetForegroundWindow() == game
				&& GetRawInputData((HRAWINPUT)lParam, RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER)) != (UINT)-1
				&& ri.header.dwType == RIM_TYPEMOUSE)
			Add(ri.data.mouse, t);
	}
	return DefWindowProcW(hwnd, msg, wParam, lParam);
}

DWORD WINAPI InputThread(LPVOID ready)
{
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
	WNDCLASSW wc = {};
	wc.lpfnWndProc = InputProc;
	wc.hInstance = GetModuleHandleW(NULL);
	wc.lpszClassName = L"bhoptools_rawprecise";
	RegisterClassW(&wc);
	g_window = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, wc.hInstance, NULL);
	if (!g_window)
		CoreLog("rawinput2 precise: couldn't make the input thread's window (%lu), unavailable", GetLastError());
	SetEvent(ready);
	if (!g_window)
		return 0;
	MSG msg;
	while (GetMessageW(&msg, NULL, 0, 0) > 0)
		DispatchMessageW(&msg);
	DestroyWindow(g_window);
	return 0;
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

// While on, the game (a new video mode, m_rawinput) and no-legacy registering the mouse keep it on
// our window. What the game asked for is kept for turning off.
BOOL WINAPI Hooked_RegisterRawInputDevices(PCRAWINPUTDEVICE devices, UINT count, UINT size)
{
	if (!g_on || !devices || size != sizeof(RAWINPUTDEVICE) || count == 0 || count > 16)
		return oRegisterRawInputDevices(devices, count, size);
	RAWINPUTDEVICE copy[16];
	memcpy(copy, devices, count * size);
	for (UINT i = 0; i < count; ++i) {
		RAWINPUTDEVICE& d = copy[i];
		if (d.usUsagePage != 1 || d.usUsage != 2 || (d.dwFlags & RIDEV_REMOVE) || d.hwndTarget == g_window)
			continue;
		g_gameTarget = d.hwndTarget;
		g_gameSink = (d.dwFlags & RIDEV_INPUTSINK) != 0;
		d.hwndTarget = g_window;
		d.dwFlags |= RIDEV_INPUTSINK;
	}
	return oRegisterRawInputDevices(copy, count, size);
}

void SwitchOff()
{
	FlushButtons();
	RAWINPUTDEVICE d;
	if (FindMouseRegistration(d) && d.hwndTarget == g_window) {
		d.hwndTarget = g_gameTarget;
		if (!g_gameSink) d.dwFlags &= ~RIDEV_INPUTSINK;
		if (!oRegisterRawInputDevices(&d, 1, sizeof(d)))
			CoreLog("rawinput2 precise: couldn't give the mouse back to the game (%lu)", GetLastError());
	}
	g_on = false;
	g_recording = false;
	CoreLog("rawinput2 precise: off");
}

void FinishCompare()
{
	g_recording = false;
	size_t moved = 0;
	double sumDiff = 0.0, sumMove = 0.0;
	int maxDiff = 0;
	for (const Tick& t : g_ticks) {
		if (!t.px && !t.ox) continue;
		moved++;
		const int d = abs(t.px - t.ox);
		sumDiff += d;
		sumMove += abs(t.px);
		if (d > maxDiff) maxDiff = d;
	}
	CoreLog("rawcompare: %u ticks, the mouse moved sideways on %u", (unsigned)g_ticks.size(), (unsigned)moved);
	if (moved)
		CoreLog("  per tick, the even spread was off from precise by %.1f counts on average (%.1f%% of the tick's movement), %d at most",
			sumDiff / moved, sumMove > 0.0 ? sumDiff * 100.0 / sumMove : 0.0, maxDiff);
	const std::string dir = settings::Dir();
	const std::string path = dir + "\\rawcompare.csv";
	FILE* f = dir.empty() ? nullptr : fopen(path.c_str(), "w");
	if (f) {
		fprintf(f, "tick,precise_x,spread_x,precise_y,spread_y\n");
		for (size_t i = 0; i < g_ticks.size(); ++i)
			fprintf(f, "%u,%d,%d,%d,%d\n", (unsigned)i, g_ticks[i].px, g_ticks[i].ox, g_ticks[i].py, g_ticks[i].oy);
		fclose(f);
		CoreLog("  every tick (mouse counts) is in %s", path.c_str());
	}
	g_ticks.clear();
}

void Cmd_RawPrecise(const CCommandX& c)
{
	if (c.argc > 1) {
		settings::Get().rawprecise = atoi(c.argv[1]) != 0;
		settings::Changed();
	}
	CoreLog("bhoptools_rawprecise %d (%s now)%s", settings::Get().rawprecise ? 1 : 0, g_on ? "on" : "off",
		g_window && !g_failed ? "" : ", unavailable, see above");
}

void Cmd_RawCompare(const CCommandX& c)
{
	if (!g_on) {
		CoreLog("rawcompare: needs rawinput2 precise on (bhoptools_rawprecise 1) and m_rawinput 1");
		return;
	}
	if (!settings::Get().rawinput)
		CoreLog("rawcompare: rawinput2 is off, so ticks aren't split and the two will match");
	const double secs = c.argc > 1 ? atof(c.argv[1]) : 5.0;
	g_ticks.clear();
	g_ticks.reserve(4096);
	g_recording = true;
	g_recordUntil = g_plat() + (secs > 0.0 && secs <= 60.0 ? secs : 5.0);
	CoreLog("rawcompare: recording for %.0f s, strafe now", g_recordUntil - g_plat());
}

ConCommandX g_cmds[2];

}  // namespace

void Init(double (*platFloatTime)())
{
	g_plat = platFloatTime;
	LARGE_INTEGER f;
	QueryPerformanceFrequency(&f);
	g_freq = f.QuadPart;
	g_buttons.reserve(4096);
	g_shutdownMsg = RegisterWindowMessageW(L"bhoptools_rawprecise_shutdown");
	HANDLE ready = CreateEventW(NULL, TRUE, FALSE, NULL);
	g_thread = ready ? CreateThread(nullptr, 0, InputThread, ready, 0, &g_threadId) : nullptr;
	if (g_thread)
		WaitForSingleObject(ready, 5000);
	if (ready)
		CloseHandle(ready);
	if (g_window)
		CoreLog("rawinput2 precise: %s (Options > bhoptools, or bhoptools_rawprecise 0/1)", settings::Get().rawprecise ? "on" : "off");
}

void Attach()
{
	DetourAttach(&(PVOID&)oRegisterRawInputDevices, Hooked_RegisterRawInputDevices);
}

void Detach()
{
	DetourDetach(&(PVOID&)oRegisterRawInputDevices, Hooked_RegisterRawInputDevices);
}

void Shutdown()
{
	const HWND game = g_gameWindow.load();
	if (game && g_shutdownMsg)
		SendMessageTimeoutW(game, g_shutdownMsg, 0, 0, SMTO_NORMAL, 10000, nullptr);
	if (g_thread) {
		PostThreadMessageW(g_threadId, WM_QUIT, 0, 0);
		WaitForSingleObject(g_thread, 2000);
		CloseHandle(g_thread);
		g_thread = nullptr;
	}
}

bool OnWindowMessage(HWND hwnd, UINT msg)
{
	if (!g_gameWindow.load())
		g_gameWindow = hwnd;
	if (!g_shutdownMsg || msg != g_shutdownMsg)
		return false;
	g_shutdown = true;
	if (g_on) SwitchOff();
	return true;
}

void Update(bool rawActive, int (*drain)())
{
	const bool want = settings::Get().rawprecise && rawActive && g_window && g_gameWindow.load() && !g_shutdown && !g_failed;
	if (want == g_on) return;
	if (!want) {
		SwitchOff();
		return;
	}
	// What's queued for the game's window goes to the engine's accumulators first; GetRawMouseAccumulators
	// still hands those out.
	drain();
	RAWINPUTDEVICE d;
	if (!FindMouseRegistration(d)) return;
	const HWND target = d.hwndTarget;
	const bool sink = (d.dwFlags & RIDEV_INPUTSINK) != 0;
	d.hwndTarget = g_window;
	d.dwFlags |= RIDEV_INPUTSINK;
	if (!oRegisterRawInputDevices(&d, 1, sizeof(d))) {
		CoreLog("rawinput2 precise: couldn't register the mouse to its thread (%lu), unavailable", GetLastError());
		g_failed = true;
		return;
	}
	g_gameTarget = target;
	g_gameSink = sink;
	AcquireSRWLockExclusive(&g_lock);
	g_head = g_count = 0;
	g_pendX = g_pendY = 0;
	g_buttons.clear();
	ReleaseSRWLockExclusive(&g_lock);
	g_shadow = MouseSplit();
	g_on = true;
	CoreLog("rawinput2 precise: on, mouse reports are stamped on their own thread");
}

bool On() { return g_on; }

int FlushButtons()
{
	static std::vector<RAWMOUSE> buttons;
	AcquireSRWLockExclusive(&g_lock);
	buttons.swap(g_buttons);
	const int reports = g_reports;
	g_reports = 0;
	ReleaseSRWLockExclusive(&g_lock);
	for (const RAWMOUSE& m : buttons)
		pollfix::OnRawMouse(m);
	buttons.clear();
	return reports;
}

void Take(int& dx, int& dy, double split)
{
	const LONGLONG now = Now();
	const double platNow = g_plat();
	const LONGLONG limit = split == 0.0 ? LLONG_MAX : now - (LONGLONG)((platNow - split) * g_freq);
	int pendX, pendY;
	dx = dy = 0;
	AcquireSRWLockExclusive(&g_lock);
	while (g_count && g_ring[g_head].t <= limit) {
		dx += g_ring[g_head].dx;
		dy += g_ring[g_head].dy;
		g_head = (g_head + 1) % kCap;
		g_count--;
	}
	pendX = g_pendX;
	pendY = g_pendY;
	g_pendX = g_pendY = 0;
	ReleaseSRWLockExclusive(&g_lock);

	if (pendX || pendY)
		MouseSplitAdd(g_shadow, pendX, pendY, platNow);
	int ox, oy;
	MouseSplitTake(g_shadow, ox, oy, split);
	if (g_recording && split != 0.0) {
		g_ticks.push_back({ dx, dy, ox, oy });
		if (platNow >= g_recordUntil)
			FinishCompare();
	}
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
		CoreLog("rawinput2 precise: couldn't register bhoptools_rawprecise and bhoptools_rawcompare");
		return;
	}
	const struct { const char* name; const char* help; void (*fn)(const CCommandX&); } defs[2] = {
		{ "bhoptools_rawprecise", "rawinput2 precise: mouse reports timestamped on their own thread, each tick takes its own (0/1, off by default).", Cmd_RawPrecise },
		{ "bhoptools_rawcompare", "Records for N seconds (default 5) what each tick got with precise against rawinput2's even spread, prints the difference and writes rawcompare.csv.", Cmd_RawCompare },
	};
	for (int i = 0; i < 2; ++i) {
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
