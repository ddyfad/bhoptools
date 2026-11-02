#pragma once
#include <Windows.h>
#include <TlHelp32.h>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <Psapi.h>

#define CLIENT_MODULE  "client.dll"
#define ENGINE_MODULE  "engine.dll"
#define VSTDLIB_MODULE "vstdlib.dll"
#define SURFACE_MODULE "vguimatsurface.dll"
#define GAMEUI_MODULE  "GameUI.dll"
#define VGUI_MODULE    "vgui2.dll"

#define INRANGE(x, a, b) (x >= a && x <= b)
#define getBits(x) (INRANGE((x & (~0x20)), 'A', 'F') ? ((x & (~0x20)) - 'A' + 0xA): (INRANGE(x, '0', '9') ? x - '0': 0))
#define getByte(x) (getBits(x[0]) << 4 | getBits(x[1]))

typedef struct BASE_RELOCATION_ENTRY {
	USHORT Offset : 12;
	USHORT Type : 4;
} BASE_RELOCATION_ENTRY, * PBASE_RELOCATION_ENTRY;

typedef void* (*CreateInterfaceFn)(const char* pName, int* pReturnCode);

struct MemRange { uintptr_t start, end; };

class IInputSystem {};
class CInput {};

inline void* AddrFromLea(uintptr_t start)
{
	auto offset = *(int32_t*)(start + 3);
	auto rip = start + 7;
	return (void*)((intptr_t)rip + offset);
}

inline uintptr_t GetModuleHandleExtern(DWORD dwProcessId, const char* lpModuleName)
{
	MODULEENTRY32 lpModuleEntry = { 0 };
	HANDLE hSnapShot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, dwProcessId);
	if (!hSnapShot)	return NULL;

	lpModuleEntry.dwSize = sizeof(lpModuleEntry);
	BOOL bModule = Module32First(hSnapShot, &lpModuleEntry);

	while (bModule)
	{
		if (!strcmp(lpModuleEntry.szModule, lpModuleName))
		{
			CloseHandle(hSnapShot);
			return (uintptr_t)lpModuleEntry.modBaseAddr;
		}

		bModule = Module32Next(hSnapShot, &lpModuleEntry);
	}

	CloseHandle(hSnapShot);
	return NULL;
}

inline uintptr_t FindPatternIn(const char* szModule, const char* szSignature)
{
	MODULEINFO modInfo{};
	HMODULE hModule = GetModuleHandleA(szModule);
	if (!hModule || !GetModuleInformation(GetCurrentProcess(), hModule, &modInfo, sizeof(MODULEINFO)))
		return NULL;
	uintptr_t startAddress = reinterpret_cast<uintptr_t>(modInfo.lpBaseOfDll);
	uintptr_t endAddress = startAddress + modInfo.SizeOfImage;
	const char* pat = szSignature;
	uintptr_t firstMatch = 0;
	for (uintptr_t pCur = startAddress; pCur < endAddress; pCur++)
	{
		if (!*pat) return firstMatch;
		if (*(PBYTE)pat == '\?' || *(BYTE*)pCur == getByte(pat))
		{
			if (!firstMatch) firstMatch = pCur;
			if (!pat[2]) return firstMatch;
			if (*(PWORD)pat == '\?\?' || *(PBYTE)pat != '\?') pat += 3;
			else pat += 2;
		}
		else
		{
			pat = szSignature;
			firstMatch = 0;
		}
	}

	return NULL;
}

inline float MIN(float a, float b)
{
	return a < b ? a : b;
}

void TeleportPredict_LogLine(const char* line);

inline void CoreLog(const char* fmt, ...)
{
	typedef void(__cdecl* ConMsgFn)(const char*, ...);
	ConMsgFn conMsg = (ConMsgFn)GetProcAddress(GetModuleHandleA("tier0.dll"), "?ConMsg@@YAXPEBDZZ");
	if (!conMsg) return;
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	conMsg("[bhoptools] %s\n", buf);
	TeleportPredict_LogLine(buf);
}

inline uintptr_t FindPatternLogged(const char* szModule, const char* name, const char* szSignature)
{
	const uintptr_t hit = FindPatternIn(szModule, szSignature);
	if (!hit)
		CoreLog("sig %s: NOT FOUND in %s", name, szModule);
	return hit;
}

inline void* PatchPointer(void** slot, void* value)
{
	DWORD prot;
	if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &prot)) return nullptr;
	void* old = InterlockedExchangePointer(slot, value);
	VirtualProtect(slot, sizeof(void*), prot, &prot);
	return old;
}

inline CreateInterfaceFn InterfaceFactory(const char* module)
{
	HMODULE h = GetModuleHandleA(module);
	return h ? (CreateInterfaceFn)GetProcAddress(h, "CreateInterface") : nullptr;
}

inline void* CreateInterfaceIn(const char* module, const char* name)
{
	CreateInterfaceFn create = InterfaceFactory(module);
	return create ? create(name, nullptr) : nullptr;
}

// A class's primary vtable through MSVC's RTTI: the TypeDescriptor holding the decorated name, the
// CompleteObjectLocator for the complete object (offset 0) pointing at it, and the vtable whose
// slot -1 points at that. `name` is the decorated name, e.g. ".?AVPropertySheet@vgui@@".
inline void** FindVTable(const char* module, const char* name)
{
	MODULEINFO mi{};
	HMODULE h = GetModuleHandleA(module);
	if (!h || !GetModuleInformation(GetCurrentProcess(), h, &mi, sizeof(mi))) return nullptr;
	const uintptr_t base = (uintptr_t)mi.lpBaseOfDll, end = base + mi.SizeOfImage;
	const size_t len = strlen(name) + 1;
	uintptr_t td = 0;
	for (uintptr_t p = base + 16; !td && p + len <= end; ++p)
		if (*(const char*)p == name[0] && !memcmp((const void*)p, name, len)) td = p - 16;
	if (!td) return nullptr;
	const DWORD tdRva = (DWORD)(td - base);
	// signature 1 (x64), offset, cdOffset, pTypeDescriptor, pClassDescriptor, pSelf: RVAs.
	uintptr_t col = 0;
	for (uintptr_t p = base; !col && p + 24 <= end; p += 4) {
		const DWORD* c = (const DWORD*)p;
		if (c[0] == 1 && c[1] == 0 && c[3] == tdRva && c[5] == (DWORD)(p - base)) col = p;
	}
	if (!col) return nullptr;
	for (uintptr_t p = base; p + 16 <= end; p += 8)
		if (*(const uintptr_t*)p == col) return (void**)(p + 8);
	return nullptr;
}

inline bool InModuleCode(const char* module, const void* p)
{
	MODULEINFO mi{};
	HMODULE h = GetModuleHandleA(module);
	if (!h || !GetModuleInformation(GetCurrentProcess(), h, &mi, sizeof(mi))) return false;
	return (uintptr_t)p >= (uintptr_t)mi.lpBaseOfDll && (uintptr_t)p < (uintptr_t)mi.lpBaseOfDll + mi.SizeOfImage;
}

inline std::vector<MemRange> ModuleDataRanges(const char* module)
{
	std::vector<MemRange> out;
	const unsigned char* base = reinterpret_cast<const unsigned char*>(GetModuleHandleA(module));
	if (!base) return out;
	const IMAGE_NT_HEADERS* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
	const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
	for (int i = 0; i < nt->FileHeader.NumberOfSections && out.size() < 2; ++i) {
		if (memcmp(sec[i].Name, ".rdata", 7) != 0 && memcmp(sec[i].Name, ".data", 6) != 0) continue;
		const uintptr_t start = reinterpret_cast<uintptr_t>(base) + sec[i].VirtualAddress;
		out.push_back({ start, start + sec[i].Misc.VirtualSize });
	}
	return out;
}

inline unsigned long long MonotonicMs()
{
	return GetTickCount64();
}

// The options slider polls it while it's dragged.
inline bool LeftMouseDown() { return (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0; }

inline double MonotonicSeconds()
{
	return (double)GetTickCount64() / 1000.0;
}
