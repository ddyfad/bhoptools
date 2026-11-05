#include "engine_trace.h"
#include "utils.h"
#include "sigs.h"

#include <cstdint>
#include <vector>

namespace etrace {
namespace {

// Vtable slots from public/engine/IEngineTrace.h: no virtual destructor, no overloads before TraceRay.
const int kVT_IEngineTrace_TraceRay = 4;

const int kVT_IVEngineClient_GetMaxClients = 21;

typedef void  (*TraceRayFn)(void* self, const RayX* ray, unsigned int mask, PlayerFilter* filter, GameTraceX* tr);
typedef int   (*GetMaxClientsFn)(void* self);
typedef bool  (*IsInGameFn)(void* self);
typedef const uint32_t* (*GetRefEHandleFn)(void* self);

std::vector<MemRange> g_engineText;
std::vector<MemRange> g_entityText;

bool InRanges(const std::vector<MemRange>& rs, const void* p)
{
	const uintptr_t a = (uintptr_t)p;
	for (const MemRange& r : rs)
		if (a >= r.start && a < r.end) return true;
	return false;
}

#ifdef _WIN32
void AddText(std::vector<MemRange>& out, const char* module)
{
	const uintptr_t base = (uintptr_t)GetModuleHandleA(module);
	if (!base) return;
	const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)base;
	if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
	const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
	if (nt->Signature != IMAGE_NT_SIGNATURE) return;
	out.push_back({ base, base + nt->OptionalHeader.SizeOfImage });
}

// Plain functions: __try can't share a frame with objects that need unwinding.
bool CallTraceRay(TraceRayFn fn, void* self, const RayX* ray, unsigned int mask, PlayerFilter* filter, GameTraceX* tr)
{
	__try
	{
		fn(self, ray, mask, filter, tr);
		return true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return false;
	}
}

int CallMaxClients(GetMaxClientsFn fn, void* self)
{
	__try
	{
		return fn(self);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return -1;
	}
}

bool CallInGame(IsInGameFn fn, void* self)
{
	__try
	{
		return fn(self);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return false;
	}
}
#else
// No SEH here, so every vtable slot we call is checked to point into engine.so's or
// client.so's code first.
void AddText(std::vector<MemRange>& out, const char* module)
{
	for (const ModuleSegment& s : FindExecSegments(module)) out.push_back({ s.start, s.end });
}

bool CallTraceRay(TraceRayFn fn, void* self, const RayX* ray, unsigned int mask, PlayerFilter* filter, GameTraceX* tr)
{
	fn(self, ray, mask, filter, tr);
	return true;
}

int CallMaxClients(GetMaxClientsFn fn, void* self) { return fn(self); }

bool CallInGame(IsInGameFn fn, void* self) { return fn(self); }
#endif

void* VSlot(void* inst, int index)
{
	void** vt = *reinterpret_cast<void***>(inst);
	return vt ? vt[index] : nullptr;
}

// On Windows this runs inside TraceRay, so a fault here unwinds to CallTraceRay's __except.
bool EntityHandle(void* ent, uint32_t* handle)
{
	void* fn = VSlot(ent, VT_IHandleEntity_GetRefEHandle);
	if (!InRanges(g_entityText, fn)) return false;
	const uint32_t* h = reinterpret_cast<GetRefEHandleFn>(fn)(ent);
	if (!h) return false;
	*handle = *h;
	return true;
}

class GameEngineTracer : public EngineTracer {
public:
	GameEngineTracer(void* trace, void* engine, TraceRayFn traceRay, GetMaxClientsFn maxClients, IsInGameFn inGame)
		: trace_(trace), engine_(engine), traceRay_(traceRay), maxClients_(maxClients), inGame_(inGame) {}

protected:
	bool RawTrace(const RayX& ray, unsigned int mask, PlayerFilter& filter, TraceBufferX& buf) override
	{
		return CallTraceRay(traceRay_, trace_, &ray, mask, &filter, &buf.tr);
	}
	bool InGame() override { return CallInGame(inGame_, engine_); }
	int  MaxClients() override { return CallMaxClients(maxClients_, engine_); }
	void Log(const char* msg) override { CoreLog("%s", msg); }
	EntityHandleFn HandleFn() override { return &EntityHandle; }

private:
	void*           trace_;
	void*           engine_;
	TraceRayFn      traceRay_;
	GetMaxClientsFn maxClients_;
	IsInGameFn      inGame_;
};

}

EngineTracer* CreateEngineTracer()
{
	CreateInterfaceFn create = InterfaceFactory(ENGINE_MODULE);
	void* trace  = create ? create("EngineTraceClient003", nullptr) : nullptr;
	void* engine = create ? create("VEngineClient014", nullptr) : nullptr;
	if (!trace || !engine) {
		CoreLog("rngfix: engine trace unavailable: %s not found", !create ? ENGINE_MODULE " CreateInterface"
			: !trace ? "EngineTraceClient003" : "VEngineClient014");
		return nullptr;
	}

	AddText(g_engineText, ENGINE_MODULE);
	AddText(g_entityText, CLIENT_MODULE);
	AddText(g_entityText, ENGINE_MODULE);

	void* traceRay   = VSlot(trace, kVT_IEngineTrace_TraceRay);
	void* maxClients = VSlot(engine, kVT_IVEngineClient_GetMaxClients);
	void* inGame     = VSlot(engine, VT_IVEngineClient_IsInGame);
	if (!InRanges(g_engineText, traceRay) || !InRanges(g_engineText, maxClients) || !InRanges(g_engineText, inGame)) {
		CoreLog("rngfix: engine trace unavailable: a vtable slot points outside " ENGINE_MODULE);
		return nullptr;
	}
	return new GameEngineTracer(trace, engine, reinterpret_cast<TraceRayFn>(traceRay),
		reinterpret_cast<GetMaxClientsFn>(maxClients), reinterpret_cast<IsInGameFn>(inGame));
}

}

namespace etrace {
namespace {

TraceWatchFn g_watchFn;
TraceRayFn   g_watchOrig;
void**       g_watchSlot;

void Hooked_TraceRay(void* self, const RayX* ray, unsigned int mask, PlayerFilter* filter, GameTraceX* tr)
{
	g_watchOrig(self, ray, mask, filter, tr);
	if (g_watchFn && ray && tr) g_watchFn(*ray, mask, *tr);
}

}

bool WatchTraces(TraceWatchFn fn)
{
	if (g_watchSlot) return true;
	CreateInterfaceFn create = InterfaceFactory(ENGINE_MODULE);
	void* trace = create ? create("EngineTraceClient003", nullptr) : nullptr;
	if (!trace) return false;
	void** slot = &(*reinterpret_cast<void***>(trace))[kVT_IEngineTrace_TraceRay];
	if (!InRanges(g_engineText, *slot)) return false;
	g_watchOrig = reinterpret_cast<TraceRayFn>(*slot);
	g_watchFn = fn;
	if (!PatchPointer(slot, reinterpret_cast<void*>(&Hooked_TraceRay))) return false;
	g_watchSlot = slot;
	return true;
}

#ifdef _WIN32
void UnwatchTraces()
{
	if (g_watchSlot) PatchPointer(g_watchSlot, reinterpret_cast<void*>(g_watchOrig));
	g_watchSlot = nullptr;
}
#endif

int EntityIndex(void* ent)
{
	uint32_t h;
	if (!ent || !EntityHandle(ent, &h) || h == kInvalidEHandle) return -1;
	return (int)(h & kEntEntryMask);
}

}
