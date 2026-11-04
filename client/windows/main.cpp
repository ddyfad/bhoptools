#include <Windows.h>
#include <winhttp.h>
#include <atomic>
#include <fstream>
#include <functional>
#include <string>
#include <vector>
#include <conio.h>
#include <stdio.h>
#include "utils.h"
#include "version.h"
#include "update_json.h"
#include "sigs.h"
#include "download_progress.h"
#include "fastdl.h"
#include "Detours/src/detours.h"
#include "teleport_predict.h"
#include "checkpoint_predict.h"
#include "speedo.h"
#include "jumpstats.h"
#include "settings.h"
#include "options_tab.h"
#include "spectate.h"
#include "game_layout.h"

typedef void(__thiscall* CHostState_OnClientConnectedFn)(void*);
CHostState_OnClientConnectedFn oCHostState_OnClientConnected;

typedef LRESULT(__thiscall* WindowProcFn)(void*, HWND, UINT, WPARAM, LPARAM);
WindowProcFn oWindowProc;

IInputSystem* g_InputSystem = nullptr;

typedef bool(__thiscall* GetRawMouseAccumulatorsFn)(void*, int&, int&);
typedef void(__thiscall* GetAccumulatedMouseDeltasAndResetAccumulatorsFn)(void*, float*, float*);
typedef void(__thiscall* ControllerMoveFn)(void*, float, void*);
typedef void(__thiscall* In_SetSampleTimeFn)(void*, float);

GetRawMouseAccumulatorsFn oGetRawMouseAccumulators;
GetAccumulatedMouseDeltasAndResetAccumulatorsFn oGetAccumulatedMouseDeltasAndResetAccumulators;
ControllerMoveFn oControllerMove;
In_SetSampleTimeFn oIn_SetSampleTime;

typedef void* (__fastcall* CHud_FindElementFn)(void* pThis, const char* name);
CHud_FindElementFn CHud_FindElement;

typedef void(__cdecl* CBaseHudChat_ChatPrintfFn)(void* pThis, int client, int filter, const char* fmt, ...);
CBaseHudChat_ChatPrintfFn CBaseHudChat_ChatPrintf;

typedef double(__cdecl* Plat_FloatTimeFn)();
Plat_FloatTimeFn Plat_FloatTime;

int* m_rawinput_cvar;

float mouseMoveFrameTime;

double m_mouseSplitTime;
double m_mouseSampleTime;
float m_flMouseSampleTime;

// F5-F8 run from the game's WindowProc: the HUD and patched code aren't safe to touch from the injector thread.
std::function<void(int)> g_hotkey;
std::atomic<bool> g_hotkeyReady;

int* m_mouseRawAccumX;
int* m_mouseRawAccumY;
bool* m_bRawInputSupported;
bool g_rawInput2Ready = false;

// NEXTRAWINPUTBLOCK uses QWORD, which no SDK header defines.
typedef unsigned __int64 QWORD;

void AddRawMouse(const RAWINPUT* ri)
{
	if (ri->header.dwType == RIM_TYPEMOUSE)
	{
		*m_mouseRawAccumX += ri->data.mouse.lLastX;
		*m_mouseRawAccumY += ri->data.mouse.lLastY;
	}
}

// One WM_INPUT per HID report means 8000 trips a second through the window procs at 8kHz.
// GetRawInputBuffer reads everything queued and removes those WM_INPUT messages as well.
bool DrainRawInput()
{
	alignas(8) static BYTE buf[64 * sizeof(RAWINPUT)];
	bool any = false;
	for (;;)
	{
		UINT size = sizeof(buf);
		UINT n = GetRawInputBuffer((RAWINPUT*)buf, &size, sizeof(RAWINPUTHEADER));
		if (n == 0 || n == (UINT)-1)
			break;
		RAWINPUT* ri = (RAWINPUT*)buf;
		for (UINT i = 0; i < n; i++, ri = NEXTRAWINPUTBLOCK(ri))
			AddRawMouse(ri);
		any = true;
	}
	return any;
}

bool GetRawMouseAccumulators(int& accumX, int& accumY, double frame_split)
{
	if (frame_split != 0.0 && DrainRawInput())
		m_mouseSampleTime = Plat_FloatTime();

	double mouseSplitTime = m_mouseSplitTime;
	if (mouseSplitTime == 0.0)
	{
		mouseSplitTime = m_mouseSampleTime - 0.01;
		m_mouseSplitTime = mouseSplitTime;
	}

	double mouseSampleTime = m_mouseSampleTime;

	if (abs(mouseSplitTime - mouseSampleTime) >= 0.000001)
	{
		if (frame_split == 0.0 || frame_split >= mouseSampleTime)
		{
			accumX = *m_mouseRawAccumX;
			accumY = *m_mouseRawAccumY;
			*m_mouseRawAccumX = *m_mouseRawAccumY = 0;

			m_mouseSplitTime = m_mouseSampleTime;

			return *m_bRawInputSupported;
		}
		else if (frame_split >= mouseSplitTime)
		{
			float splitSegment = (frame_split - mouseSplitTime) / (mouseSampleTime - mouseSplitTime);

			accumX = splitSegment * (*m_mouseRawAccumX);
			accumY = splitSegment * (*m_mouseRawAccumY);

			*m_mouseRawAccumX -= accumX;
			*m_mouseRawAccumY -= accumY;

			m_mouseSplitTime = frame_split;

			return *m_bRawInputSupported;
		}
	}

	accumX = accumY = 0;

	return *m_bRawInputSupported;
}

__declspec(noinline)
void GetAccumulatedMouseDeltasAndResetAccumulators(CInput* thisptr, float* mx, float* my, float frametime)
{
	float* m_flAccumulatedMouseXMovement = (float*)((uintptr_t)thisptr + 0xc);
	float* m_flAccumulatedMouseYMovement = (float*)((uintptr_t)thisptr + 0x10);

	int m_rawinput = *m_rawinput_cvar;

	if (m_flMouseSampleTime > 0.0)
	{
		int rawMouseX, rawMouseY;
		if(m_rawinput != 0)
		{
			if (settings::Get().rawinput && frametime > 0.0)
			{
				m_flMouseSampleTime -= MIN(m_flMouseSampleTime, frametime);
				GetRawMouseAccumulators(rawMouseX, rawMouseY, Plat_FloatTime() - m_flMouseSampleTime);
			}
			else
			{
				GetRawMouseAccumulators(rawMouseX, rawMouseY, 0.0);
				m_flMouseSampleTime = 0.0;
			}
		}
		else
		{
			rawMouseX = *m_flAccumulatedMouseXMovement;
			rawMouseY = *m_flAccumulatedMouseYMovement;
		}

		*m_flAccumulatedMouseXMovement = 0.0;
		*m_flAccumulatedMouseYMovement = 0.0;

		*mx = (float)rawMouseX;
		*my = (float)rawMouseY;
		cp::AddInput(*mx, *my);
	}
	else
	{
		*mx = 0.0;
		*my = 0.0;
	}
}

bool __fastcall Hooked_GetRawMouseAccumulators(void* thisptr, int& accumX, int& accumY)
{
	return GetRawMouseAccumulators(accumX, accumY, 0.0);
}

LRESULT __fastcall Hooked_WindowProc(void* thisptr, HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	// Does what CInputSystem::WindowProc would (add lLastX/Y to its accumulators), then takes the
	// rest of the queue in one go. DefWindowProc does the WM_INPUT cleanup the docs ask for.
	if (uMsg == WM_INPUT && g_rawInput2Ready && *m_bRawInputSupported)
	{
		RAWINPUT ri;
		UINT size = sizeof(ri);
		if (GetRawInputData((HRAWINPUT)lParam, RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER)) != (UINT)-1)
			AddRawMouse(&ri);
		DrainRawInput();
		m_mouseSampleTime = Plat_FloatTime();
		return DefWindowProcW(hwnd, uMsg, wParam, lParam);
	}
	tp::RegisterCommands();
	speedo::RegisterCommands();
	// GameUI's vtables are patched on the game thread; a failed look is a few ms of scanning, so once a second.
	static bool optionsDone = false;
	static ULONGLONG optionsNext = 0;
	static bool spectateDone = false;
	static bool paintDone = false;
	if ((!optionsDone || !spectateDone || !paintDone) && g_hotkeyReady && GetTickCount64() >= optionsNext) {
		if (!optionsDone) optionsDone = options::Install();
		if (!spectateDone) spectateDone = spectate::Install();
		if (!paintDone) paintDone = js::InstallPaint();
		optionsNext = GetTickCount64() + 1000;
	}
	switch (uMsg)
	{
	case WM_SYSKEYDOWN:
	case WM_KEYDOWN:
		{
			// Bit 30 is the previous key state, so this skips autorepeat.
			if ((lParam & 0x40000000) == 0) {
				if (wParam >= VK_F5 && wParam <= VK_F8) {
					if (g_hotkeyReady)
						g_hotkey((int)(wParam - VK_F5 + 1));
				}
			}
			break;
		}
	}

	return oWindowProc(thisptr, hwnd, uMsg, wParam, lParam);
}

void __fastcall Hooked_GetAccumulatedMouseDeltasAndResetAccumulators(CInput* thisptr, float* mx, float* my)
{
	GetAccumulatedMouseDeltasAndResetAccumulators(thisptr, mx, my, mouseMoveFrameTime);

	mouseMoveFrameTime = 0.0;
}

void __fastcall Hooked_ControllerMove(void* thisptr, float ft, void* cmd)
{
	mouseMoveFrameTime = ft;

	oControllerMove(thisptr, mouseMoveFrameTime, cmd);
}

void __fastcall Hooked_IN_SetSampleTime(void* thisptr, float frametime)
{
	m_flMouseSampleTime = frametime;

	oIn_SetSampleTime(thisptr, frametime);
	js::OnFrame();
}

void __fastcall Hooked_CHostState_OnClientConnected(void* thisptr)
{
	oCHostState_OnClientConnected(thisptr);
	FlashWindow(FindWindowA("Valve001", NULL), TRUE);
}

typedef void(__fastcall* CEngineVGui_UpdateCustomProgressBarFn)(void*, float, const wchar_t*);
typedef void(__fastcall* CDownloadManager_QueueInternalFn)(void*, const char*, const char*, const char*, bool, bool);

CEngineVGui_UpdateCustomProgressBarFn oCEngineVGui_UpdateCustomProgressBar;
CDownloadManager_QueueInternalFn oCDownloadManager_QueueInternal;

// Through the original body, whose GameUI null check guards it; it never reads `this`.
void dlprogress::Draw(const wchar_t* desc, float progress)
{
	oCEngineVGui_UpdateCustomProgressBar(nullptr, progress, desc);
}

void __fastcall Hooked_CEngineVGui_UpdateCustomProgressBar(void* thisptr, float progress, const wchar_t* desc)
{
	wchar_t buf[256];
	desc = dlprogress::Describe(desc, progress, buf);
	oCEngineVGui_UpdateCustomProgressBar(thisptr, progress, desc);
}

// The injected image keeps the launcher's resolved imports, which only hold for DLLs the game
// has mapped at the same address, so WinHTTP is resolved here.
bool HttpsDownload(const wchar_t* host, const wchar_t* path, const wchar_t* file)
{
	HMODULE h = LoadLibraryA("winhttp.dll");
	if (!h)
		return false;
	auto open = (decltype(&WinHttpOpen))GetProcAddress(h, "WinHttpOpen");
	auto setTimeouts = (decltype(&WinHttpSetTimeouts))GetProcAddress(h, "WinHttpSetTimeouts");
	auto connect = (decltype(&WinHttpConnect))GetProcAddress(h, "WinHttpConnect");
	auto openRequest = (decltype(&WinHttpOpenRequest))GetProcAddress(h, "WinHttpOpenRequest");
	auto sendRequest = (decltype(&WinHttpSendRequest))GetProcAddress(h, "WinHttpSendRequest");
	auto receiveResponse = (decltype(&WinHttpReceiveResponse))GetProcAddress(h, "WinHttpReceiveResponse");
	auto queryHeaders = (decltype(&WinHttpQueryHeaders))GetProcAddress(h, "WinHttpQueryHeaders");
	auto readData = (decltype(&WinHttpReadData))GetProcAddress(h, "WinHttpReadData");
	auto closeHandle = (decltype(&WinHttpCloseHandle))GetProcAddress(h, "WinHttpCloseHandle");
	if (!open || !setTimeouts || !connect || !openRequest || !sendRequest || !receiveResponse
			|| !queryHeaders || !readData || !closeHandle)
		return false;

	wchar_t tmp[MAX_PATH + 8];
	swprintf(tmp, sizeof(tmp) / sizeof(tmp[0]), L"%ls.part", file);
	bool ok = false;
	HINTERNET session = open(L"bhoptools", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
		WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
	if (session)
		setTimeouts(session, 15000, 15000, 30000, 30000);
	HINTERNET conn = session ? connect(session, host, INTERNET_DEFAULT_HTTPS_PORT, 0) : nullptr;
	HINTERNET req = conn ? openRequest(conn, L"GET", path, nullptr, WINHTTP_NO_REFERER,
		WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
	DWORD status = 0, statusSize = sizeof(status);
	if (req && sendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
			&& receiveResponse(req, nullptr)
			&& queryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
				WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX)
			&& status == 200)
	{
		if (FILE* f = _wfopen(tmp, L"wb"))
		{
			static char buf[64 * 1024];
			ok = true;
			for (;;)
			{
				DWORD n = 0;
				if (!readData(req, buf, sizeof(buf), &n)) { ok = false; break; }
				if (!n) break;
				if (fwrite(buf, 1, n, f) != n) { ok = false; break; }
			}
			ok = fclose(f) == 0 && ok;
		}
	}
	if (req) closeHandle(req);
	if (conn) closeHandle(conn);
	if (session) closeHandle(session);
	if (ok)
		ok = MoveFileExW(tmp, file, MOVEFILE_REPLACE_EXISTING) != 0;
	if (!ok)
		DeleteFileW(tmp);
	return ok;
}

FILE* fastdl::OpenLumpChecksums()
{
	wchar_t path[MAX_PATH];
	DWORD len = GetTempPathW(MAX_PATH - 20, path);
	if (!len || len >= MAX_PATH - 20)
		return nullptr;
	wcscat(path, L"lump_checksums.csv");

	WIN32_FILE_ATTRIBUTE_DATA attr;
	bool fresh = false;
	if (GetFileAttributesExW(path, GetFileExInfoStandard, &attr))
	{
		ULARGE_INTEGER now, mtime;
		FILETIME ft;
		GetSystemTimeAsFileTime(&ft);
		now.LowPart = ft.dwLowDateTime; now.HighPart = ft.dwHighDateTime;
		mtime.LowPart = attr.ftLastWriteTime.dwLowDateTime; mtime.HighPart = attr.ftLastWriteTime.dwHighDateTime;
		// FILETIME counts 100 ns.
		const unsigned long long age = now.QuadPart > mtime.QuadPart ? now.QuadPart - mtime.QuadPart : mtime.QuadPart - now.QuadPart;
		fresh = age < 36ull * 60 * 60 * 10000000;
	}
	if (!fresh)
		HttpsDownload(L"venus.fastdl.me", L"/lump_checksums.csv", path);
	return _wfopen(path, L"rb");
}

DWORD WINAPI LumpChecksumsThread(LPVOID)
{
	fastdl::LoadLumpChecksums();
	return 0;
}

// What Linux's CDownloadManager::Queue does for an https base URL: a .bz2 attempt, then the file.
void fastdl::QueueFromFastdl(void* dlman, const char* url, const char* gamePath, bool bz2, bool plain)
{
	if (bz2)
		oCDownloadManager_QueueInternal(dlman, fastdl::BASE, url, gamePath, true, true);
	if (plain)
		oCDownloadManager_QueueInternal(dlman, fastdl::BASE, url, gamePath, true, false);
}

// CL_QueueDownload calls this once for the .bz2 (only with a server fastdl) and once for the file.
void __fastcall Hooked_CDownloadManager_QueueInternal(void* thisptr, const char* baseURL, const char* urlPath,
	const char* gamePath, bool bAsHTTP, bool bIsBZ2)
{
	if (fastdl::Queue(thisptr, gamePath, bIsBZ2 || !bAsHTTP, !bIsBZ2))
		return;
	oCDownloadManager_QueueInternal(thisptr, baseURL, urlPath, gamePath, bAsHTTP, bIsBZ2);
}

typedef void(__fastcall* LevelInitPreEntityFn)(void*, const char*);
LevelInitPreEntityFn oLevelInitPreEntity;
bool g_teleportPredictReady = false;
bool g_checkpointPredictReady = false;

void __fastcall Hooked_LevelInitPreEntity(void* thisptr, const char* mapname)
{
	settings::ApplyRawInput(true);
	if (g_checkpointPredictReady)
		cp::OnLevelInit();
	// A same-map reconnect keeps the level name, so the sim can't see it itself.
	if (g_teleportPredictReady)
		tp::OnLevelInit();
	oLevelInitPreEntity(thisptr, fastdl::ServerMapName(mapname));
}

typedef void(__fastcall* CInput_CreateMoveFn)(void*, int, float, bool);
CInput_CreateMoveFn oInputCreateMove;
void** g_inputCreateMoveSlot;

void __fastcall Hooked_InputCreateMove(void* thisptr, int sequence, float frametime, bool active)
{
	oInputCreateMove(thisptr, sequence, frametime, active);
	speedo::OnCommand(thisptr, sequence);
	cp::OnCreateMove(sequence);
}

bool InClient(const void* p)
{
	MODULEINFO mi{};
	HMODULE h = GetModuleHandleA("client.dll");
	if (!h || !GetModuleInformation(GetCurrentProcess(), h, &mi, sizeof(mi))) return false;
	return (uintptr_t)p >= (uintptr_t)mi.lpBaseOfDll && (uintptr_t)p < (uintptr_t)mi.lpBaseOfDll + mi.SizeOfImage;
}

// The CInput behind the `input` global that CHLClient::CreateMove loads before its call through slot 3.
void* FindInput(void* hlclient)
{
	const unsigned char* fn = (*reinterpret_cast<const unsigned char* const* const*>(hlclient))[VT_Client_CreateMove];
	if (!InClient(fn)) return nullptr;
	void* const* global = nullptr;
	bool called = false;
	for (int i = 0; i < 0x60 && !called; ++i) {
		if (fn[i] == 0x48 && fn[i + 1] == 0x8B && fn[i + 2] == 0x0D)
			global = reinterpret_cast<void* const*>(fn + i + 7 + *reinterpret_cast<const int32_t*>(fn + i + 3));
		else if (global && fn[i] == 0xFF && fn[i + 1] == 0x50 && fn[i + 2] == 0x18)
			called = true;
	}
	void* input = called && InClient(global) ? *global : nullptr;
	if (!input || !InClient(input)) return nullptr;
	void** vt = *reinterpret_cast<void***>(input);
	return InClient(vt) && InClient(vt[VT_IInput_CreateMove]) && InClient(vt[VT_IInput_GetUserCmd]) ? input : nullptr;
}

BOOL IsProcessRunning(DWORD processID)
{
	HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, processID);
	DWORD ret = WaitForSingleObject(process, 0);
	CloseHandle(process);
	return ret == WAIT_TIMEOUT;
}

void RecvProxy_ZeroToVector(const void* data, void* object, float* out)
{
	for (int i = 0; i < 3; i++)
		out[i] = 0.0;
}

uintptr_t gHUD;

void HudPrint(const char* fmt, ...)
{
	char buf[256];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	void* chat = (gHUD && CHud_FindElement && CBaseHudChat_ChatPrintf) ? CHud_FindElement((void*)gHUD, "CHudChat") : nullptr;
	if (chat)
		CBaseHudChat_ChatPrintf(chat, 0, 0, "%s", buf);
	else
		CoreLog("%s", buf);
}

bool StringPtrIs(const void* p, const char* want)
{
	__try
	{
		const char* s = *(const char* const*)p;
		return s && strcmp(s, want) == 0;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return false;
	}
}

void* FindConCommandBase(const char* name)
{
	HMODULE vstdlib = GetModuleHandleA("vstdlib.dll");
	auto factory = vstdlib ? reinterpret_cast<CreateInterfaceFn>(GetProcAddress(vstdlib, "CreateInterface")) : nullptr;
	void* cvar = factory ? factory("VEngineCvar004", nullptr) : nullptr;
	if (!cvar)
		return nullptr;
	// Slot 10 is CCvar::FindCommandBase.
	typedef void* (*FindCommandBaseFn)(void*, const char*);
	return (*reinterpret_cast<FindCommandBaseFn**>(cvar))[10](cvar, name);
}

DWORD InjectionEntryPoint(DWORD processID)
{
	// The injected image keeps the launcher's import addresses, which only hold if these are loaded
	// here too (DLLs load at the same base in every process). The game doesn't load them itself.
	LoadLibraryA("VCRUNTIME140.dll");
	LoadLibraryA("VCRUNTIME140_1.dll");
	LoadLibraryA("MSVCP140.dll");

	// Patched code in a VAC-secured client is a ban risk. Steam's relaunch after a handoff can drop
	// the launcher's -insecure, so check the game's own command line.
	bool insecure = false;
	for (const char* p = GetCommandLineA(); *p && !insecure; )
	{
		while (*p == ' ' || *p == '\t') p++;
		const char* end = p;
		while (*end && *end != ' ' && *end != '\t') end++;
		insecure = end - p == 9 && !_strnicmp(p, "-insecure", 9);
		p = end;
	}
	if (!insecure)
	{
		CoreLog("-insecure is not on the command line, not installing");
		return 0;
	}

	// The launcher injects as soon as client.dll is in the module list, which can be
	// before CHLClient::Init has registered m_rawinput.
	for (int i = 0; i < 600 && !FindConCommandBase("m_rawinput"); i++)
		Sleep(100);

	Plat_FloatTime = (Plat_FloatTimeFn)GetProcAddress(GetModuleHandleA("tier0.dll"), "Plat_FloatTime");
	oWindowProc = (WindowProcFn)(FindPatternLogged("inputsystem.dll", "CInputSystem::WindowProc", "44 89 44 24 ? 48 89 54 24"));

	oCHostState_OnClientConnected = (CHostState_OnClientConnectedFn)(FindPatternLogged("engine.dll", "CHostState::OnClientConnected", "40 53 48 83 EC 60 80 B9 ? ? ? ? 00 48 8B D9 0F 84"));

	// Host_AccumulateTime raises every frame to at least 1 ms, so above 1000 fps the client's clock
	// runs ahead of real time and the server corrects it. NOP the maxsd against MIN_FRAMETIME.
	if (auto minFrametime = (BYTE*)FindPatternLogged("engine.dll", "Host_AccumulateTime min frametime", SIG_HostAccumulateTime_MinFrametime))
	{
		BYTE* site = minFrametime + 15;
		double floor;
		memcpy(&floor, site + 8 + *(int32_t*)(site + 4), sizeof(floor));
		DWORD protect;
		if (floor != 0.001)
			CoreLog("frametime floor: the clamp isn't 0.001, leaving it");
		else if (VirtualProtect(site, 8, PAGE_EXECUTE_READWRITE, &protect))
		{
			static const BYTE nop8[8] = { 0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00 };
			memcpy(site, nop8, 8);
			VirtualProtect(site, 8, protect, &protect);
			FlushInstructionCache(GetCurrentProcess(), site, 8);
		}
	}

	auto inputsystem_factory = reinterpret_cast<CreateInterfaceFn>(GetProcAddress(GetModuleHandleA("inputsystem.dll"), "CreateInterface"));
	g_InputSystem = inputsystem_factory ? reinterpret_cast<IInputSystem*>(inputsystem_factory("InputSystemVersion001", nullptr)) : nullptr;

	oGetRawMouseAccumulators = (GetRawMouseAccumulatorsFn)(FindPatternLogged("inputsystem.dll", "CInputSystem::GetRawMouseAccumulators", "8B 81 ? ? ? ? 89 02"));
	if (oGetRawMouseAccumulators && g_InputSystem)
	{
		// mov eax, [rcx+m_mouseRawAccumX] ; mov [rdx], eax
		const int accumX = *(int*)((uintptr_t)oGetRawMouseAccumulators + 2);
		if (accumX > 4 && accumX < 0x10000)
		{
			m_mouseRawAccumX = (int*)((uintptr_t)g_InputSystem + accumX);
			m_mouseRawAccumY = (int*)((uintptr_t)g_InputSystem + accumX + 4);
			m_bRawInputSupported = (bool*)((uintptr_t)g_InputSystem + accumX - 4);
		}
	}
	// The call that matters is inlined into CInput::MouseMove and patched below.
	oGetAccumulatedMouseDeltasAndResetAccumulators = (GetAccumulatedMouseDeltasAndResetAccumulatorsFn)(FindPatternLogged("client.dll", "CInput::GetAccumulatedMouseDeltasAndResetAccumulators", "48 89 5C 24 ? 48 89 74 24 ? 57 48 83 EC 20 8B 41 ? 49 8B F8"));
	// ConVar (x64): name +0x18, m_pParent +0x38, m_nValue +0x58.
	if (void* m_rawinput = FindConCommandBase("m_rawinput"))
	{
		if (StringPtrIs((char*)m_rawinput + 0x18, "m_rawinput"))
		{
			char* parent = *(char**)((char*)m_rawinput + 0x38);
			m_rawinput_cvar = (int*)((parent ? parent : (char*)m_rawinput) + 0x58);
		}
	}
	// A rip-relative MOV, not a LEA, but the displacement sits in the same place.
	if (oGetAccumulatedMouseDeltasAndResetAccumulators && !m_rawinput_cvar)
		m_rawinput_cvar = (int*)((uintptr_t)AddrFromLea((uintptr_t)oGetAccumulatedMouseDeltasAndResetAccumulators + 35) + 0x20);
	// Thunk written over the inlined call: calls the
	// hook with rcx = this, rdx = &mx, r8 = &my, then loads the results into xmm8/xmm9.
	// The hook's address is patched in at +41.
	char patch[89+1] = "\x51\x52\x41\x50\x41\x51\x41\x52\x41\x53\x41\x57\x49\x89\xE7\x48\x83\xEC\x40\x48\x83\xE4\xE0\x48\x8D\x14\x24\x4C\x8D\x44\x24\x10\x48\x83\xEC\x20\x48\x89\xD9\x48\xB8\x11\x11\x11\x11\x11\x11\x11\x11\xFF\xD0\xF3\x44\x0F\x10\x44\x24\x20\xF3\x44\x0F\x10\x4C\x24\x30\x4C\x89\xFC\x41\x5F\x41\x5B\x41\x5A\x41\x59\x41\x58\x5A\x59\x90\x90\x90\x90\x90\x90\x90\x90\x90";
	*(void**)(patch + 41) = Hooked_GetAccumulatedMouseDeltasAndResetAccumulators;
	char GetAccumulatedMouseDeltasAndResetAccumulators_inside_MouseMove_original[89]{};
	auto GetAccumulatedMouseDeltasAndResetAccumulators_inside_MouseMove = (void*)FindPatternLogged("client.dll", "CInput::MouseMove (inlined GetAccumulatedMouseDeltas)", "48 8B 05 ? ? ? ? F3 44 0F 10 43");
	DWORD GetAccumulatedMouseDeltasAndResetAccumulators_inside_MouseMove_protect;
	oControllerMove = (ControllerMoveFn)(FindPatternLogged("client.dll", "CInput::ControllerMove", "48 89 5C 24 ? 57 48 83 EC 30 80 B9 ? ? ? ? 00 49 8B F8"));
	oIn_SetSampleTime = (In_SetSampleTimeFn)(FindPatternLogged("client.dll", "CInput::IN_SetSampleTime", "f3 0f 11 49 20 c3"));

	// All or nothing: a partial set would patch MouseMove to call into missing pieces.
	g_rawInput2Ready = Plat_FloatTime && m_mouseRawAccumX && m_rawinput_cvar
		&& oGetRawMouseAccumulators && oGetAccumulatedMouseDeltasAndResetAccumulators
		&& GetAccumulatedMouseDeltasAndResetAccumulators_inside_MouseMove && oControllerMove && oIn_SetSampleTime;
	if (g_rawInput2Ready)
	{
		VirtualProtect(GetAccumulatedMouseDeltasAndResetAccumulators_inside_MouseMove, sizeof(patch) - 1, PAGE_EXECUTE_READWRITE, &GetAccumulatedMouseDeltasAndResetAccumulators_inside_MouseMove_protect);
		memcpy(GetAccumulatedMouseDeltasAndResetAccumulators_inside_MouseMove_original, GetAccumulatedMouseDeltasAndResetAccumulators_inside_MouseMove, sizeof(GetAccumulatedMouseDeltasAndResetAccumulators_inside_MouseMove_original));
		memcpy(GetAccumulatedMouseDeltasAndResetAccumulators_inside_MouseMove, patch, sizeof(patch) - 1);
	}
	else
	{
		CoreLog("m_rawinput 2: disabled, see the missing signature above");
	}

	// The LEA at +25 loads gHUD.
	auto ClientModeShared_Init = FindPatternLogged("client.dll", "ClientModeShared::Init", "48 89 5C 24 ? 48 89 74 24 ? 57 48 83 EC 30 48 8B F1 48 8D 15");
	gHUD = ClientModeShared_Init ? (uintptr_t)AddrFromLea(ClientModeShared_Init + 25) : 0;
	CHud_FindElement = (CHud_FindElementFn)FindPatternLogged("client.dll", "CHud::FindElement", "48 89 5C 24 ? 48 89 6C 24 ? 48 89 74 24 ? 57 48 83 EC 20 33 DB 48 8B EA 48 8B F9 39 59");
	CBaseHudChat_ChatPrintf = (CBaseHudChat_ChatPrintfFn)FindPatternLogged("client.dll", "CBaseHudChat::ChatPrintf", "4C 89 4C 24 ? 48 89 4C 24 ? 55 53");
	settings::Load();
	BYTE nopBuffer[6] = { 0x90,0x90,0x90,0x90,0x90,0x90 };
	BYTE jumpPredOriginalBytes[6];
	// The `test byte ptr [reg+off], 2` of the "don't pogo stick" check.
	auto jumpPred = reinterpret_cast<void*>(FindPatternLogged("client.dll", "autohop prediction", "F6 40 ? 02 75 ? 48 8B 05"));
	DWORD jumpPredOriginalProtect;
	if (jumpPred)
	{
		memcpy(jumpPredOriginalBytes, jumpPred, sizeof(jumpPredOriginalBytes));
		VirtualProtect(jumpPred, sizeof(jumpPredOriginalBytes), PAGE_EXECUTE_READWRITE, &jumpPredOriginalProtect);
	}

	BYTE prleNew[6] = { 0x48, 0x83, 0xc4, 0x40,    0x5f,    0xc3 }; // add rsp, 0x40 ; pop rdi ; ret
	BYTE prleOriginal[6]{};
	auto pPlayerRoughLandingEffects = reinterpret_cast<void*>(FindPatternLogged("client.dll", "PlayerRoughLandingEffects", "40 57 48 83 EC 40 0F 57 C0"));
	if (pPlayerRoughLandingEffects)
		pPlayerRoughLandingEffects = (char*)pPlayerRoughLandingEffects + 84 /* after ->PlayStepSound */;
	DWORD prleProtect;
	if (pPlayerRoughLandingEffects)
	{
		memcpy(prleOriginal, pPlayerRoughLandingEffects, sizeof(prleOriginal));
		VirtualProtect(pPlayerRoughLandingEffects, sizeof(prleOriginal), PAGE_EXECUTE_READWRITE, &prleProtect);
	}
	// RecvProp (x64): m_pVarName [0], m_ProxyFn [6].
	auto m_vecPunchAngle_RecvProp_Lea = FindPatternLogged("client.dll", "RecvProp m_vecPunchAngle", "48 8D 0D ? ? ? ? E8 ? ? ? ? 44 8D 4E ? 48 89 7C 24 ? 41 B8 D0 00 00 00");
	auto m_vecPunchAngle_RecvProp = m_vecPunchAngle_RecvProp_Lea ? (void**)AddrFromLea(m_vecPunchAngle_RecvProp_Lea) : nullptr;
	if (m_vecPunchAngle_RecvProp && !StringPtrIs(&m_vecPunchAngle_RecvProp[0], "m_vecPunchAngle"))
	{
		CoreLog("RecvProp m_vecPunchAngle: the LEA doesn't point at a RecvProp with that name, skipping");
		m_vecPunchAngle_RecvProp = nullptr;
	}
	void* m_vecPunchAngle_RecvProp_Original = m_vecPunchAngle_RecvProp ? m_vecPunchAngle_RecvProp[6] : nullptr;
	bool viewpunchRemoved = false;
	auto setViewpunch = [&](bool remove)
	{
		if (pPlayerRoughLandingEffects)
			memcpy(pPlayerRoughLandingEffects, remove ? prleNew : prleOriginal, sizeof(prleOriginal));
		if (m_vecPunchAngle_RecvProp)
			m_vecPunchAngle_RecvProp[6] = remove ? (void*)RecvProxy_ZeroToVector : m_vecPunchAngle_RecvProp_Original;
		viewpunchRemoved = remove;
	};
	setViewpunch(settings::Get().viewpunch);

	void* hlclient = CreateInterfaceIn("client.dll", "VClient017");
	oLevelInitPreEntity = hlclient ? (LevelInitPreEntityFn)(*(void***)hlclient)[VT_CHLClient_LevelInitPreEntity] : nullptr;
	tp::SetCheckpointPrinter(cp::PrintState);
	g_teleportPredictReady = tp::Init() && tp::ResolveNetvars();
	if (!g_teleportPredictReady)
		CoreLog("trigger prediction: unavailable (init failed)");
	if (hlclient)
		speedo::Attach(hlclient);
	void* input = hlclient ? FindInput(hlclient) : nullptr;
	if (input)
	{
		g_inputCreateMoveSlot = &(*reinterpret_cast<void***>(input))[VT_IInput_CreateMove];
		oInputCreateMove = (CInput_CreateMoveFn)*g_inputCreateMoveSlot;
		if (!PatchPointer(g_inputCreateMoveSlot, reinterpret_cast<void*>(&Hooked_InputCreateMove)))
			g_inputCreateMoveSlot = nullptr;
	}
	if (!g_inputCreateMoveSlot)
		CoreLog("hud keys: CInput::CreateMove not found, keys are drawn by the server");
	g_checkpointPredictReady = cp::ResolveOffsets() && cp::Attach();
	if (g_checkpointPredictReady)
		cp::SetTriggerPrediction(g_teleportPredictReady);
	else
		CoreLog("checkpoint prediction: unavailable");

	// All or nothing: every other piece calls into the progress bar.
	dlprogress::oUpdateProgressBar = (dlprogress::UpdateProgressBarFn)FindPatternLogged("engine.dll", "CDownloadManager::UpdateProgressBar", SIG_CDownloadManager_UpdateProgressBar);
	oCEngineVGui_UpdateCustomProgressBar = (CEngineVGui_UpdateCustomProgressBarFn)FindPatternLogged("engine.dll", "CEngineVGui::UpdateCustomProgressBar", SIG_CEngineVGui_UpdateCustomProgressBar);
	dlprogress::oPersistToDisk = (dlprogress::PersistToDiskFn)FindPatternLogged("engine.dll", "DownloadCache::PersistToDisk", SIG_DownloadCache_PersistToDisk);
	dlprogress::oDecompressBZipToDisk = (dlprogress::DecompressBZipToDiskFn)FindPatternLogged("engine.dll", "DecompressBZipToDisk", SIG_DecompressBZipToDisk);
	dlprogress::oBZ2_bzread = (dlprogress::BZ2_bzreadFn)FindPatternLogged("engine.dll", "BZ2_bzread", SIG_BZ2_bzread);
	const bool downloadProgressReady = dlprogress::oUpdateProgressBar && oCEngineVGui_UpdateCustomProgressBar
		&& dlprogress::oPersistToDisk && dlprogress::oDecompressBZipToDisk && dlprogress::oBZ2_bzread;
	if (!downloadProgressReady)
		CoreLog("download progress: disabled, see the missing signature above");

	fastdl::oProcessServerInfo = (fastdl::ProcessServerInfoFn)FindPatternLogged("engine.dll", "CClientState::ProcessServerInfo", SIG_CClientState_ProcessServerInfo);
	fastdl::MD5_MapFile = (fastdl::MD5_MapFileFn)FindPatternLogged("engine.dll", "MD5_MapFile", SIG_MD5_MapFile);
	oCDownloadManager_QueueInternal = (CDownloadManager_QueueInternalFn)FindPatternLogged("engine.dll", "CDownloadManager::QueueInternal", SIG_CDownloadManager_QueueInternal);
	fastdl::oIsValidFileForTransfer = (fastdl::IsValidFileForTransferFn)FindPatternLogged("engine.dll", "IsValidFileForTransfer", SIG_IsValidFileForTransfer);
	// The level name swap in LevelInitPreEntity is part of it: the game must see the server's map name.
	const bool fastdlReady = oLevelInitPreEntity && fastdl::oProcessServerInfo && fastdl::MD5_MapFile
		&& oCDownloadManager_QueueInternal && fastdl::oIsValidFileForTransfer;
	if (!fastdlReady)
		CoreLog("fastdl.me map fix: disabled, see the missing signature above");
	const bool levelInitHooked = oLevelInitPreEntity && (g_teleportPredictReady || g_checkpointPredictReady || fastdlReady);

	DetourTransactionBegin();
	DetourUpdateThread(GetCurrentThread());
	// A null target fails the whole transaction, so attach only what was found.
	if (oCHostState_OnClientConnected)
		DetourAttach(&(PVOID&)oCHostState_OnClientConnected, Hooked_CHostState_OnClientConnected);
	if (oWindowProc)
		DetourAttach(&(PVOID&)oWindowProc, Hooked_WindowProc);
	if (g_teleportPredictReady)
		tp::Attach();
	if (levelInitHooked)
		DetourAttach(&(PVOID&)oLevelInitPreEntity, Hooked_LevelInitPreEntity);
	if (downloadProgressReady)
	{
		DetourAttach(&(PVOID&)oCEngineVGui_UpdateCustomProgressBar, Hooked_CEngineVGui_UpdateCustomProgressBar);
		DetourAttach(&(PVOID&)dlprogress::oUpdateProgressBar, dlprogress::Hooked_UpdateProgressBar);
		DetourAttach(&(PVOID&)dlprogress::oPersistToDisk, dlprogress::Hooked_PersistToDisk);
		DetourAttach(&(PVOID&)dlprogress::oDecompressBZipToDisk, dlprogress::Hooked_DecompressBZipToDisk);
		DetourAttach(&(PVOID&)dlprogress::oBZ2_bzread, dlprogress::Hooked_BZ2_bzread);
	}
	if (fastdlReady)
	{
		DetourAttach(&(PVOID&)fastdl::oIsValidFileForTransfer, fastdl::Hooked_IsValidFileForTransfer);
		DetourAttach(&(PVOID&)oCDownloadManager_QueueInternal, Hooked_CDownloadManager_QueueInternal);
		DetourAttach(&(PVOID&)fastdl::oProcessServerInfo, fastdl::Hooked_ProcessServerInfo);
	}
	if (g_rawInput2Ready)
	{
		DetourAttach(&(PVOID&)oGetRawMouseAccumulators, Hooked_GetRawMouseAccumulators);
		DetourAttach(&(PVOID&)oGetAccumulatedMouseDeltasAndResetAccumulators, Hooked_GetAccumulatedMouseDeltasAndResetAccumulators);
		DetourAttach(&(PVOID&)oControllerMove, Hooked_ControllerMove);
		DetourAttach(&(PVOID&)oIn_SetSampleTime, Hooked_IN_SetSampleTime);
	}
	if (LONG err = DetourTransactionCommit())
		CoreLog("DetourTransactionCommit failed (%ld), no hooks installed", err);
	else if (fastdlReady)
		CloseHandle(CreateThread(nullptr, 0, LumpChecksumsThread, nullptr, 0, nullptr));

	bool jumpPredPatched = false;
	if (g_teleportPredictReady && !settings::Get().predict)
		tp::SetEnabled(false);
	settings::SetApply([&]
	{
		const Settings& s = settings::Get();
		if (tp::Enabled() != s.predict)
			tp::SetEnabled(s.predict);
		if (viewpunchRemoved != s.viewpunch)
			setViewpunch(s.viewpunch);
	});

	g_hotkey = [&](int key)
	{
		if (key == 1 && !jumpPred)
		{
			HudPrint("BunnyhopAPE: unavailable (signature not found)");
		}
		else if (key == 1)
		{
			if (jumpPredPatched)
				memcpy(jumpPred, jumpPredOriginalBytes, sizeof(jumpPredOriginalBytes));
			else
				memcpy(jumpPred, nopBuffer, sizeof(nopBuffer));
			jumpPredPatched = !jumpPredPatched;
			HudPrint("BunnyhopAPE: %d", jumpPredPatched);
		}
		else if (key == 4)
		{
			settings::Get().predict = !settings::Get().predict;
			settings::Changed();
			HudPrint("prediction %d", tp::Enabled() ? 1 : 0);
		}
	};
	g_hotkeyReady = true;
	while (IsProcessRunning(processID))
		Sleep(200);
	g_hotkeyReady = false;
	settings::SetApply(nullptr);

	if (g_rawInput2Ready)
	{
		memcpy(GetAccumulatedMouseDeltasAndResetAccumulators_inside_MouseMove, GetAccumulatedMouseDeltasAndResetAccumulators_inside_MouseMove_original, sizeof(GetAccumulatedMouseDeltasAndResetAccumulators_inside_MouseMove_original));
		VirtualProtect(GetAccumulatedMouseDeltasAndResetAccumulators_inside_MouseMove, sizeof(GetAccumulatedMouseDeltasAndResetAccumulators_inside_MouseMove_original), GetAccumulatedMouseDeltasAndResetAccumulators_inside_MouseMove_protect, &GetAccumulatedMouseDeltasAndResetAccumulators_inside_MouseMove_protect);
	}

	if (pPlayerRoughLandingEffects)
	{
		memcpy(pPlayerRoughLandingEffects, prleOriginal, sizeof(prleOriginal));
		VirtualProtect(pPlayerRoughLandingEffects, sizeof(prleOriginal), prleProtect, &prleProtect);
	}
	if (m_vecPunchAngle_RecvProp)
		m_vecPunchAngle_RecvProp[6] = m_vecPunchAngle_RecvProp_Original;
	if (jumpPred)
	{
		memcpy(jumpPred, jumpPredOriginalBytes, sizeof(jumpPredOriginalBytes));
		VirtualProtect(jumpPred, sizeof(jumpPredOriginalBytes), jumpPredOriginalProtect, &jumpPredOriginalProtect);
	}

	DetourTransactionBegin();
	DetourUpdateThread(GetCurrentThread());
	if (oCHostState_OnClientConnected)
		DetourDetach(&(PVOID&)oCHostState_OnClientConnected, Hooked_CHostState_OnClientConnected);
	if (g_teleportPredictReady)
		tp::Detach();
	if (g_checkpointPredictReady)
		cp::Detach();
	speedo::Detach();
	options::Uninstall();
	spectate::Uninstall();
	js::UninstallPaint();
	if (g_inputCreateMoveSlot)
		PatchPointer(g_inputCreateMoveSlot, reinterpret_cast<void*>(oInputCreateMove));
	if (levelInitHooked)
		DetourDetach(&(PVOID&)oLevelInitPreEntity, Hooked_LevelInitPreEntity);
	if (downloadProgressReady)
	{
		DetourDetach(&(PVOID&)oCEngineVGui_UpdateCustomProgressBar, Hooked_CEngineVGui_UpdateCustomProgressBar);
		DetourDetach(&(PVOID&)dlprogress::oUpdateProgressBar, dlprogress::Hooked_UpdateProgressBar);
		DetourDetach(&(PVOID&)dlprogress::oPersistToDisk, dlprogress::Hooked_PersistToDisk);
		DetourDetach(&(PVOID&)dlprogress::oDecompressBZipToDisk, dlprogress::Hooked_DecompressBZipToDisk);
		DetourDetach(&(PVOID&)dlprogress::oBZ2_bzread, dlprogress::Hooked_BZ2_bzread);
	}
	if (fastdlReady)
	{
		fastdl::RemoveDownloadVtableHooks();
		DetourDetach(&(PVOID&)fastdl::oIsValidFileForTransfer, fastdl::Hooked_IsValidFileForTransfer);
		DetourDetach(&(PVOID&)oCDownloadManager_QueueInternal, Hooked_CDownloadManager_QueueInternal);
		DetourDetach(&(PVOID&)fastdl::oProcessServerInfo, fastdl::Hooked_ProcessServerInfo);
	}
	if (oWindowProc)
		DetourDetach(&(PVOID&)oWindowProc, Hooked_WindowProc);
	if (g_rawInput2Ready)
	{
		DetourDetach(&(PVOID&)oGetRawMouseAccumulators, Hooked_GetRawMouseAccumulators);
		DetourDetach(&(PVOID&)oGetAccumulatedMouseDeltasAndResetAccumulators, Hooked_GetAccumulatedMouseDeltasAndResetAccumulators);
		DetourDetach(&(PVOID&)oControllerMove, Hooked_ControllerMove);
		DetourDetach(&(PVOID&)oIn_SetSampleTime, Hooked_IN_SetSampleTime);
	}
	DetourTransactionCommit();

	ExitThread(0);
	return 0;
}

// Credits: https://www.ired.team/offensive-security/code-injection-process-injection/pe-injection-executing-pes-inside-remote-processes
void PEInjector(HANDLE targetProcess, DWORD Func(DWORD))
{
	PVOID imageBase = GetModuleHandle(NULL);
	PIMAGE_DOS_HEADER dosHeader = (PIMAGE_DOS_HEADER)imageBase;
	PIMAGE_NT_HEADERS ntHeader = (PIMAGE_NT_HEADERS)((DWORD_PTR)imageBase + dosHeader->e_lfanew);

	PVOID localImage = VirtualAlloc(NULL, ntHeader->OptionalHeader.SizeOfImage, MEM_COMMIT, PAGE_READWRITE);
	memcpy(localImage, imageBase, ntHeader->OptionalHeader.SizeOfImage);

	PVOID targetImage = VirtualAllocEx(targetProcess, NULL, ntHeader->OptionalHeader.SizeOfImage, MEM_COMMIT, PAGE_EXECUTE_READWRITE);

	DWORD_PTR deltaImageBase = (DWORD_PTR)targetImage - (DWORD_PTR)imageBase;

	PIMAGE_BASE_RELOCATION relocationTable = (PIMAGE_BASE_RELOCATION)((DWORD_PTR)localImage + ntHeader->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].VirtualAddress);
	DWORD relocationEntriesCount = 0;
	PDWORD_PTR patchedAddress;
	PBASE_RELOCATION_ENTRY relocationRVA = NULL;

	while (relocationTable->SizeOfBlock > 0)
	{
		relocationEntriesCount = (relocationTable->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / sizeof(USHORT);
		relocationRVA = (PBASE_RELOCATION_ENTRY)(relocationTable + 1);

		for (DWORD i = 0; i < relocationEntriesCount; i++)
		{
			// Type 0 entries only pad a block; a DIR64 entry can sit at page offset 0 too.
			if (relocationRVA[i].Type == IMAGE_REL_BASED_DIR64)
			{
				patchedAddress = (PDWORD_PTR)((DWORD_PTR)localImage + relocationTable->VirtualAddress + relocationRVA[i].Offset);
				*patchedAddress += deltaImageBase;
			}
		}
		relocationTable = (PIMAGE_BASE_RELOCATION)((DWORD_PTR)relocationTable + relocationTable->SizeOfBlock);
	}

	WriteProcessMemory(targetProcess, targetImage, localImage, ntHeader->OptionalHeader.SizeOfImage, NULL);

	CreateRemoteThread(targetProcess, NULL, 0, (LPTHREAD_START_ROUTINE)((DWORD_PTR)Func + deltaImageBase), (LPVOID)GetCurrentProcessId(), 0, NULL);
}

// https://stackoverflow.com/a/14678800
std::string ReplaceString(std::string subject, const std::string& search,
	const std::string& replace) {
	size_t pos = 0;
	while ((pos = subject.find(search, pos)) != std::string::npos) {
		subject.replace(pos, search.length(), replace);
		pos += replace.length();
	}
	return subject;
}

std::string GetSteamPath()
{
	HKEY key;
	if (RegOpenKeyA(HKEY_LOCAL_MACHINE, "SOFTWARE\\WOW6432Node\\Valve\\Steam", &key) != ERROR_SUCCESS)
		return "";
	char buf[256] = {};
	DWORD size = sizeof(buf) - 1;
	RegQueryValueExA(key, "InstallPath", 0, NULL, (BYTE*)buf, &size);
	RegCloseKey(key);
	return std::string(buf);
}

// Assumes the libraryfolders.vdf is "well formed"
std::string GetCSSPath(std::string const & steampath)
{
	std::ifstream libraryfolders(steampath + "\\steamapps\\libraryfolders.vdf");
	std::string line, css_path, library_path;
	std::vector<std::string> libraries = { steampath };
	while (std::getline(libraryfolders, line))
	{
#define PPPPP "\t\t\"path\"\t\t\""
		if (line.rfind(PPPPP, 0) == 0)
		{
			library_path = line.substr(sizeof(PPPPP) - 1, line.size() - sizeof(PPPPP));
			library_path = ReplaceString(library_path, "\\\\", "\\");
			libraries.push_back(library_path);
		}

		if (css_path == "" && line.rfind("\t\t\t\"240\"", 0) == 0)
			css_path = library_path;
	}
	const char* const kRel = "\\steamapps\\common\\Counter-Strike Source\\";
	auto has_game = [&](std::string const & lib) {
		return GetFileAttributesA((lib + kRel + "cstrike_win64.exe").c_str()) != INVALID_FILE_ATTRIBUTES;
	};
	if (css_path == "" || !has_game(css_path))
	{
		css_path.clear();
		for (auto const & lib : libraries)
			if (has_game(lib)) { css_path = lib; break; }
	}
	if (css_path != "")
		css_path += kRel;
	return css_path;
}

ULONGLONG g_launchStart = GetTickCount64();

void LogTime(const char* what)
{
	printf("[%5.1fs] %s\n", (GetTickCount64() - g_launchStart) / 1000.0, what);
}

DWORD ReadActiveProcess(const char* value)
{
	DWORD v = 0, size = sizeof(v);
	RegGetValueA(HKEY_CURRENT_USER, "SOFTWARE\\Valve\\Steam\\ActiveProcess", value, RRF_RT_REG_DWORD, NULL, &v, &size);
	return v;
}

std::wstring ProcessCommandLine(HANDLE process)
{
	typedef LONG(NTAPI* NtQueryInformationProcessFn)(HANDLE, ULONG, PVOID, ULONG, PULONG);
	struct CommandLine { USHORT length, maxLength; PWSTR buffer; };
	static auto query = (NtQueryInformationProcessFn)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryInformationProcess");
	if (!query) return L"";
	ULONG len = 0;
	query(process, 60, nullptr, 0, &len);
	if (len < sizeof(CommandLine)) return L"";
	std::vector<BYTE> buf(len);
	if (query(process, 60, buf.data(), len, &len) < 0) return L"";
	auto cl = (CommandLine*)buf.data();
	return cl->buffer ? std::wstring(cl->buffer, cl->length / sizeof(WCHAR)) : L"";
}

DWORD FindGameProcess(DWORD not_pid)
{
	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snap == INVALID_HANDLE_VALUE) return 0;
	PROCESSENTRY32 pe = {};
	pe.dwSize = sizeof(pe);
	DWORD found = 0;
	for (BOOL ok = Process32First(snap, &pe); ok && !found; ok = Process32Next(snap, &pe))
		if (pe.th32ProcessID != not_pid && !_stricmp(pe.szExeFile, "cstrike_win64.exe"))
			found = pe.th32ProcessID;
	CloseHandle(snap);
	return found;
}

// Assumes "X:\Program Files (x86)\Steam\userdata\STEAMIDHERE\config\localconfig.vdf" is "well formed"
std::string GetCSSLaunchOptions(std::string const & steampath, std::string const & steamid3)
{
	std::ifstream localconfig(steampath + "\\userdata\\" + steamid3 + "\\config\\localconfig.vdf");
	std::string line;
	bool in_css = false;
	while (std::getline(localconfig, line))
	{
		if (line.rfind("\t\t\t\t\t\"240\"", 0) == 0)
			in_css = true;
		if (line.rfind("\t\t\t\t\t}", 0) == 0)
			in_css = false;
#define LLLLL "\t\t\t\t\t\t\"LaunchOptions\"\t\t\""
		if (in_css && line.rfind(LLLLL, 0) == 0)
		{
			line = line.substr(sizeof(LLLLL) - 1, line.size() - sizeof(LLLLL));
			line = ReplaceString(line, "\\\\", "\\");
			return line;
		}
	}
	return "";
}

// Offers to replace this exe with the latest release. True when the new copy was started.
bool SelfUpdate()
{
	if (!strcmp(APP_VERSION, "dev"))
		return false;

	wchar_t buf[MAX_PATH];
	GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH);
	const std::wstring settingsDir = std::wstring(buf) + L"\\bhoptools";
	const std::wstring ignoreFile = settingsDir + L"\\no_update_check";
	const bool ignored = GetFileAttributesW(ignoreFile.c_str()) != INVALID_FILE_ATTRIBUTES;

	GetModuleFileNameW(NULL, buf, MAX_PATH);
	const std::wstring self = buf;
	const std::wstring old = self + L".old";
	const std::wstring fresh = self + L".new";
	// The previous update's exe was still running when it was renamed.
	DeleteFileW(old.c_str());

	GetTempPathW(MAX_PATH, buf);
	const std::wstring jsonFile = std::wstring(buf) + L"bhoptools_release.json";
	if (!HttpsDownload(L"api.github.com", L"/repos/ddyfad/bhoptools/releases?per_page=20", jsonFile.c_str()))
		return false;
	std::ifstream in(jsonFile, std::ios::binary);
	std::string json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	in.close();
	DeleteFileW(jsonFile.c_str());

	std::string changelog;
	bool required = false;
	const std::string tag = Changelog(json, APP_VERSION, changelog, &required);
	if (tag.empty() || (ignored && !required))
		return false;

	wchar_t wtext[2048];
	if (required)
	{
		const std::string text = "A required update is being installed: " + tag + " (you have " APP_VERSION ").\n\nChangelog:\n\n" + changelog;
		MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, wtext, 2048);
		MessageBoxW(NULL, wtext, L"bhoptools update", MB_OK | MB_ICONINFORMATION | MB_TOPMOST);
	}
	else
	{
		const std::string text = tag + " is out, you have " APP_VERSION ".\n\nChangelog:\n\n" + changelog + "\n\nYes: update now\nNo: ask again next time\nCancel: never check again (delete %APPDATA%\\bhoptools\\no_update_check to undo)";
		MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, wtext, 2048);
		const int answer = MessageBoxW(NULL, wtext, L"bhoptools update", MB_YESNOCANCEL | MB_ICONINFORMATION | MB_TOPMOST);
		if (answer == IDCANCEL)
		{
			CreateDirectoryW(settingsDir.c_str(), NULL);
			CloseHandle(CreateFileW(ignoreFile.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL));
		}
		if (answer != IDYES)
			return false;
	}

	const std::wstring path = L"/ddyfad/bhoptools/releases/download/" + std::wstring(tag.begin(), tag.end()) + L"/bhoptools.exe";
	char magic[2] = {};
	if (HttpsDownload(L"github.com", path.c_str(), fresh.c_str()))
		std::ifstream(fresh, std::ios::binary).read(magic, 2);
	if (magic[0] != 'M' || magic[1] != 'Z')
	{
		DeleteFileW(fresh.c_str());
		printf("update download failed, starting the current version\n");
		return false;
	}

	if (!MoveFileExW(self.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING)
			|| !MoveFileExW(fresh.c_str(), self.c_str(), MOVEFILE_REPLACE_EXISTING))
	{
		MoveFileExW(old.c_str(), self.c_str(), MOVEFILE_REPLACE_EXISTING);
		printf("couldn't replace the exe, starting the current version\n");
		return false;
	}
	STARTUPINFOW si = {};
	PROCESS_INFORMATION pi = {};
	return CreateProcessW(self.c_str(), NULL, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi) != 0;
}

// Credits: https://github.com/alkatrazbhop/BunnyhopAPE
int main()
{
	SetConsoleTitle("bhoptools x64");
	printf("bhoptools " APP_VERSION "\nhttps://github.com/ddyfad/bhoptools\n\n");
	if (SelfUpdate())
		return 0;

	auto steam_path = GetSteamPath();
	printf("steampath = %s\n", steam_path.c_str());
	auto steamid3 = std::to_string(ReadActiveProcess("ActiveUser"));
	printf("steamid3  = %s\n", steamid3.c_str());
	auto launch_options = GetCSSLaunchOptions(steam_path, steamid3);
	for (const char* flag : { "-console", "-novid", "-insecure" })
		if ((" " + launch_options + " ").find(std::string(" ") + flag + " ") == std::string::npos)
			launch_options = std::string(flag) + " " + launch_options;
	printf("launchopt = %s\n", launch_options.c_str());
	auto css_path = GetCSSPath(steam_path);
	printf("css path  = %s\n\n", css_path.c_str());
	if (css_path.empty())
	{
		printf("Couldn't find Counter-Strike Source (cstrike_win64.exe) in any Steam library under %s.\nIs it installed? Press Enter to close.\n", steam_path.c_str());
		while (1)
		{
			if (_kbhit() && _getch() == VK_RETURN)
				return 0;
			Sleep(500);
		}
	}
	auto css_exe = css_path + "cstrike_win64.exe";

	HANDLE game = NULL;
	DWORD game_pid = 0;

	if (!FindGameProcess(0) && !getenv("BHOPTOOLS_DIRECT"))
	{
		PROCESS_INFORMATION spi = {};
		STARTUPINFOA ssi = {};
		auto steam_cmd = "\"" + steam_path + "\\steam.exe\" -applaunch 240 " + launch_options;
		if (CreateProcessA(NULL, (char*)steam_cmd.c_str(), NULL, NULL, FALSE, 0, NULL, steam_path.c_str(), &ssi, &spi))
		{
			CloseHandle(spi.hThread);
			CloseHandle(spi.hProcess);
			LogTime("asked Steam to start the game");
			for (int i = 0; i < 240 && !game; i++)
			{
				DWORD pid = FindGameProcess(0);
				HANDLE h = pid ? OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid) : NULL;
				if (h) { game = h; game_pid = pid; }
				else Sleep(500);
			}
			if (game)
				LogTime("game process started by Steam");
			else
				printf("Steam didn't start the game; starting it directly (no Steam overlay)\n");
		}
		if (game)
		{
			auto started_with = ProcessCommandLine(game);
			if (!started_with.empty() && started_with.find(L"-insecure") == std::wstring::npos)
			{
				printf("Steam didn't pass the launch options on (no -insecure); starting the game directly (no Steam overlay)\n");
				TerminateProcess(game, 0);
				WaitForSingleObject(game, 10000);
				CloseHandle(game);
				game = NULL;
				game_pid = 0;
			}
		}
	}

	if (!game)
	{
		PROCESS_INFORMATION pi = {};
		STARTUPINFOA si = {};

		SetEnvironmentVariableA("SteamAppId", "240");
		SetEnvironmentVariableA("SteamGameId", "240");
		auto cmdline = "\"" + css_exe + "\" " + launch_options;
		if (!CreateProcessA(css_exe.c_str(), (char*)cmdline.c_str(), NULL, NULL, FALSE, 0, NULL, css_path.c_str(), &si, &pi))
		{
			auto err = GetLastError();
			char* buf;
			FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPSTR)&buf, 0, NULL);

			printf("CreateProcessA failed (0x%x): %s\n", err, buf);

			while (1)
			{
				if (_kbhit() && _getch() == VK_RETURN)
					return 0;
				Sleep(500);
			}
		}

		LogTime("game process started");
		game = pi.hProcess;
		game_pid = pi.dwProcessId;
	}
	int handoff_wait = 0;
	while (!GetModuleHandleExtern(game_pid, "client.dll"))
	{
		Sleep(1000);
		DWORD exitcode;
		if (GetExitCodeProcess(game, &exitcode) && exitcode != STILL_ACTIVE)
		{
			// It handed itself to Steam, which starts a new copy.
			if (!handoff_wait++) LogTime("game handed itself to steam, waiting for the new process");
			DWORD pid = FindGameProcess(game_pid);
			HANDLE h = pid ? OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid) : NULL;
			if (h)
			{
				CloseHandle(game);
				game = h;
				game_pid = pid;
				LogTime("found the new game process");
			}
			else if (handoff_wait > 60)
				return 0;
		}
	}

	printf("Options > bhoptools switches mouse interpolation (it sets m_rawinput) and the rest\n\nPress F5 to toggle BunnyhopAPE autobhop prediction (OFF by default)\nPress F8 to toggle all client-side prediction: triggers, rngfix, checkpoints, HUD speed (on by default)\n");

	LogTime("client.dll loaded, injecting");
	PEInjector(game, InjectionEntryPoint);

	WaitForSingleObject(game, INFINITE);
	return 0;
}
