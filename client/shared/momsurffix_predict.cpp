#include "momsurffix_predict.h"
#include "utils.h"
#include "game_layout.h"

#include <cstdint>
#include <cstdio>
#include <vector>

namespace msf {
namespace {

#ifdef _WIN32
// client.dll x64 CCSGameMovement. MSVC puts CGameMovement's GetPlayerMins()/GetPlayerMaxs() next to
// IGameMovement's bool overloads, so the order isn't the server's.
const int kVT_GetPlayerMins   = 8;   // () const: the current hull, by hidden pointer
const int kVT_GetPlayerMaxs   = 9;
const int kVT_TracePlayerBBox = 10;
const int kVT_TryPlayerMove   = 32;  // (Vector* firstDest, CGameTrace* firstTrace, float)
const int kVT_GetCheckInterval   = 28;  // (IntervalType_t)
const int kVT_CheckStuck         = 39;
const int kVT_TestPlayerPosition = 51;  // (const Vector&, int group, trace_t&), CBaseHandle by hidden pointer
const char kSigTryPlayerMove[] =
	"4C 8B DC 49 89 5B 18 49 89 53 10 55 56 57 41 54 41 55 41 56 41 57 49 8D AB 88 FE FF FF 48 81 EC 40 02 00 00 48 8B 41 10 45 33 E4";
// GetPlayerMaxs through [rax+48h], GetPlayerMins through [r10+40h], then Ray_t::Init from start (a
// twin at a lower address inits from end).
const char kSigTracePlayerBBox[] =
	"48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 48 81 EC D0 00 00 00 48 8B 01 48 8B F2 "
	"48 8D 54 24 40 41 8B E9 49 8B F8 4C 8B F1 FF 50 48 4D 8B 16 48 8D 54 24 4C 49 8B CE 48 8B D8 41 FF 52 40 "
	"4C 8B C7 48 89 5C 24 20 4C 8B C8 48 8D 8C 24 80 00 00 00 48 8B D6 E8";
const char kSigCheckStuck[] =
	"40 55 56 57 48 8D 6C 24 B9 48 81 EC F0 00 00 00 48 8B F1 E8 ? ? ? ? 4C 8B 46 10 48 8D 4D 97 48 8B 06 48 8D 55 67";
// STUCK (1): every command while m_StuckLast is set, else once a second of ticks.
const char kSigGetCheckInterval[] =
	"85 D2 74 ? 83 EA 01 74 ? 83 FA 01 74 ? B8 01 00 00 00 C3 48 8B 05 ? ? ? ? F3 0F 10 05 ? ? ? ? F3 0F 5E 40 1C "
	"F3 0F 2C C0 C3 48 8B 41 08 83 B8";
#else
// Itanium order with two destructor slots, as in the Linux server's CCSGameMovement.
const int kVT_TracePlayerBBox = 9;
const int kVT_GetPlayerMins   = 27;  // () const: the current hull, in xmm0:xmm1
const int kVT_GetPlayerMaxs   = 28;
const int kVT_TryPlayerMove   = 33;
const int kVT_GetCheckInterval   = 29;
const int kVT_CheckStuck         = 40;
const int kVT_TestPlayerPosition = 52;  // CBaseHandle has a copy constructor: returned by hidden pointer
#endif
const int kIntervalStuck = 1;
// IMoveHelper has no destructor before it on either ABI.
const int kVT_IMoveHelper_AddToTouched = 2;  // (const trace_t&, const Vector& impactvelocity)
// CGameMovement: vptr, player, mv.
const ptrdiff_t kGM_Player = 0x08;
const ptrdiff_t kGM_Mv     = 0x10;
const int kCollisionGroupPlayerMovement = 8;
const uint8_t kMoveTypeWalk = 2;
const size_t kTryPlayerMoveScan = 0x900;
const size_t kTracePlayerBBoxScan = 0x100;

struct Vec3 { float x, y, z; };

typedef int  (*TryPlayerMoveFn)(void* self, float* firstDest, etrace::GameTraceX* firstTrace, float slide);
typedef void (*TracePlayerBBoxFn)(void* self, const float* start, const float* end, unsigned int mask, int group,
                                  etrace::GameTraceX* tr);
typedef bool (*AddToTouchedFn)(void* self, const etrace::GameTraceX* tr, const float* velocity);
typedef int  (*CheckStuckFn)(void* self);
typedef int  (*GetCheckIntervalFn)(void* self, int type);
#ifdef _WIN32
typedef Vec3* (*HullFn)(void* self, Vec3* out);
typedef uint32_t* (*TestPlayerPositionFn)(void* self, uint32_t* out, const float* pos, int group, etrace::GameTraceX* tr);
#else
typedef Vec3 (*HullFn)(void* self);
typedef void (*TestPlayerPositionFn)(uint32_t* out, void* self, const float* pos, int group, etrace::GameTraceX* tr);
#endif

void**            g_vt;
void*             g_gm;
TryPlayerMoveFn   oTryPlayerMove;
TracePlayerBBoxFn g_tracePlayerBBox;
HullFn            oGetPlayerMins, oGetPlayerMaxs;
void**            g_moveHelper;  // &IMoveHelper::sm_pSingleton
bool              g_ready, g_attached;

MsfSettings g_settings;
Player      g_player;
bool        g_armed;
// The retrace's grown hull, returned by the getters while TracePlayerBBox runs for it.
bool        g_override;
Vec3        g_mins, g_maxs;

CheckStuckFn         oCheckStuck;
GetCheckIntervalFn   oGetCheckInterval;
TestPlayerPositionFn g_testPlayerPosition;
bool                 g_stuckReady, g_stuckAttached, g_stuckArmed;
MsfStuckState        g_stuck;
// The state each command started with, so prediction replays run CheckStuck from the same state.
struct StuckRecord { int key; bool used; MsfStuckState state; };
StuckRecord g_stuckLog[256];

std::vector<MemRange> g_clientText;

void FindClientText()
{
#ifdef _WIN32
	const uintptr_t base = (uintptr_t)GetModuleHandleA(CLIENT_MODULE);
	if (!base) return;
	const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)(base + ((const IMAGE_DOS_HEADER*)base)->e_lfanew);
	g_clientText.push_back({ base, base + nt->OptionalHeader.SizeOfImage });
#else
	for (const ModuleSegment& s : FindExecSegments(CLIENT_MODULE)) g_clientText.push_back({ s.start, s.end });
#endif
}

size_t ClientBytes(const void* p)
{
	const uintptr_t a = (uintptr_t)p;
	for (const MemRange& r : g_clientText)
		if (a >= r.start && a < r.end) return r.end - a;
	return 0;
}

size_t Min(size_t a, size_t b) { return a < b ? a : b; }

#ifdef _WIN32
Vec3* Hooked_GetPlayerMins(void* self, Vec3* out)
{
	if (!g_override) return oGetPlayerMins(self, out);
	*out = g_mins;
	return out;
}

Vec3* Hooked_GetPlayerMaxs(void* self, Vec3* out)
{
	if (!g_override) return oGetPlayerMaxs(self, out);
	*out = g_maxs;
	return out;
}

Vec3 Hull(HullFn fn, void* self)
{
	Vec3 v;
	fn(self, &v);
	return v;
}
#else
Vec3 Hooked_GetPlayerMins(void* self) { return g_override ? g_mins : oGetPlayerMins(self); }
Vec3 Hooked_GetPlayerMaxs(void* self) { return g_override ? g_maxs : oGetPlayerMaxs(self); }
Vec3 Hull(HullFn fn, void* self) { return fn(self); }
#endif

// The client's own TracePlayerBBox filters and traces as the server's UTIL_TraceRay does; the
// retrace's grown hull goes in through the hooked getters.
class ClientWorld : public MsfWorld {
public:
	explicit ClientWorld(void* gm) : gm_(gm) {}

	void Trace(const float start[3], const float end[3], const float* subMins, const float* addMaxs,
	           etrace::GameTraceX& tr) override
	{
		if (subMins) {
			const Vec3 mins = ::msf::Hull(oGetPlayerMins, gm_), maxs = ::msf::Hull(oGetPlayerMaxs, gm_);
			g_mins = { mins.x - subMins[0], mins.y - subMins[1], mins.z - subMins[2] };
			g_maxs = { maxs.x + addMaxs[0], maxs.y + addMaxs[1], maxs.z + addMaxs[2] };
			g_override = true;
		}
		g_tracePlayerBBox(gm_, start, end, etrace::kMaskPlayerSolid, kCollisionGroupPlayerMovement, &tr);
		g_override = false;
	}

	void AddToTouched(const etrace::GameTraceX& tr, float* velocity) override
	{
		void* helper = *g_moveHelper;
		reinterpret_cast<AddToTouchedFn>((*reinterpret_cast<void***>(helper))[kVT_IMoveHelper_AddToTouched])(helper, &tr, velocity);
	}

	void Hull(float mins[3], float maxs[3]) override
	{
		const Vec3 lo = ::msf::Hull(oGetPlayerMins, gm_), hi = ::msf::Hull(oGetPlayerMaxs, gm_);
		mins[0] = lo.x; mins[1] = lo.y; mins[2] = lo.z;
		maxs[0] = hi.x; maxs[1] = hi.y; maxs[2] = hi.z;
	}

private:
	void* gm_;
};

int Hooked_TryPlayerMove(void* self, float* firstDest, etrace::GameTraceX* firstTrace, float slide)
{
	char* gm = static_cast<char*>(self);
	char* mv = *reinterpret_cast<char**>(gm + kGM_Mv);
	const char* pl = g_player.pl;
	if (!g_armed || !g_settings.known || self != g_gm || !mv || *reinterpret_cast<char**>(gm + kGM_Player) != pl)
		return oTryPlayerMove(self, firstDest, firstTrace, slide);

	MsfMove m;
	m.origin          = reinterpret_cast<float*>(mv + MV_AbsOrigin);
	m.velocity        = reinterpret_cast<float*>(mv + MV_Velocity);
	m.frametime       = *g_player.frametime;
	m.onGround        = *reinterpret_cast<const uint32_t*>(pl + g_player.groundOffset) != 0xFFFFFFFFu;
	m.walking         = *reinterpret_cast<const uint8_t*>(pl + g_player.moveTypeOffset) == kMoveTypeWalk;
	m.surfaceFriction = *reinterpret_cast<const float*>(pl + g_player.frictionOffset);
	m.bounce          = g_player.bounce;
	ClientWorld w(self);
	return MsfTryPlayerMove(w, g_settings, m, firstDest, firstTrace);
}

class ClientStuckWorld : public MsfStuckWorld {
public:
	explicit ClientStuckWorld(void* gm) : gm_(gm) {}

	bool Blocked(const float pos[3], etrace::GameTraceX& tr) override
	{
		uint32_t hit = 0xFFFFFFFFu;
#ifdef _WIN32
		g_testPlayerPosition(gm_, &hit, pos, kCollisionGroupPlayerMovement, &tr);
#else
		g_testPlayerPosition(&hit, gm_, pos, kCollisionGroupPlayerMovement, &tr);
#endif
		return hit != 0xFFFFFFFFu;
	}

	void AddToTouched(const etrace::GameTraceX& tr, float* velocity) override
	{
		ClientWorld(gm_).AddToTouched(tr, velocity);
	}

private:
	void* gm_;
};

bool ForOurPlayer(void* self)
{
	char* gm = static_cast<char*>(self);
	return self == g_gm && *reinterpret_cast<char**>(gm + kGM_Mv) && *reinterpret_cast<char**>(gm + kGM_Player) == g_player.pl;
}

int Hooked_CheckStuck(void* self)
{
	if (!g_stuckArmed || !ForOurPlayer(self)) return oCheckStuck(self);
	char* mv = *reinterpret_cast<char**>(static_cast<char*>(self) + kGM_Mv);
	ClientStuckWorld w(self);
	return MsfCheckStuck(w, g_stuck, reinterpret_cast<float*>(mv + MV_AbsOrigin), reinterpret_cast<float*>(mv + MV_Velocity),
		g_player.curtime);
}

// PlayerMove checks every command while the server's own nudging is under way.
int Hooked_GetCheckInterval(void* self, int type)
{
	if (g_stuckArmed && type == kIntervalStuck && g_stuck.last != 0 && ForOurPlayer(self)) return 1;
	return oGetCheckInterval(self, type);
}

void InitStuck()
{
	void* cs = g_vt[kVT_CheckStuck];
	void* gci = g_vt[kVT_GetCheckInterval];
	if (!ClientBytes(cs) || !ClientBytes(gci) || !ClientBytes(g_vt[kVT_TestPlayerPosition])) {
		CoreLog("server stuck check: unavailable (a GameMovement vtable slot points outside " CLIENT_MODULE ")");
		return;
	}
#ifdef _WIN32
	if (FindPatternLogged(CLIENT_MODULE, "CGameMovement::CheckStuck", kSigCheckStuck) != (uintptr_t)cs
			|| FindPatternLogged(CLIENT_MODULE, "CGameMovement::GetCheckInterval", kSigGetCheckInterval) != (uintptr_t)gci) {
		CoreLog("server stuck check: unavailable (CheckStuck or GetCheckInterval isn't in its vtable slot)");
		return;
	}
#endif
	if (!MsfHasVcall(static_cast<const uint8_t*>(cs), Min(ClientBytes(cs), 0x200), kVT_TestPlayerPosition * 8)) {
		CoreLog("server stuck check: unavailable (CheckStuck doesn't call TestPlayerPosition)");
		return;
	}
	oCheckStuck          = reinterpret_cast<CheckStuckFn>(cs);
	oGetCheckInterval    = reinterpret_cast<GetCheckIntervalFn>(gci);
	g_testPlayerPosition = reinterpret_cast<TestPlayerPositionFn>(g_vt[kVT_TestPlayerPosition]);
	g_stuckReady = true;
}

bool Fail(const char* why)
{
	CoreLog("momsurffix prediction: unavailable (%s)", why);
	return false;
}

}

bool Init(void* gameMovement)
{
	g_gm = gameMovement;
	g_vt = *reinterpret_cast<void***>(gameMovement);
	FindClientText();
	void* tpm  = g_vt[kVT_TryPlayerMove];
	void* tpbb = g_vt[kVT_TracePlayerBBox];
	if (!ClientBytes(tpm) || !ClientBytes(tpbb) || !ClientBytes(g_vt[kVT_GetPlayerMins]) || !ClientBytes(g_vt[kVT_GetPlayerMaxs]))
		return Fail("a GameMovement vtable slot points outside " CLIENT_MODULE);
#ifdef _WIN32
	if (FindPatternLogged(CLIENT_MODULE, "CGameMovement::TryPlayerMove", kSigTryPlayerMove) != (uintptr_t)tpm
			|| FindPatternLogged(CLIENT_MODULE, "CGameMovement::TracePlayerBBox", kSigTracePlayerBBox) != (uintptr_t)tpbb)
		return Fail("TryPlayerMove or TracePlayerBBox isn't in its vtable slot");
#endif
	const uint8_t* tpbbCode = static_cast<const uint8_t*>(tpbb);
	const size_t tpbbLen = Min(ClientBytes(tpbb), kTracePlayerBBoxScan);
	if (!MsfHasVcall(tpbbCode, tpbbLen, kVT_GetPlayerMins * 8) || !MsfHasVcall(tpbbCode, tpbbLen, kVT_GetPlayerMaxs * 8))
		return Fail("TracePlayerBBox doesn't get the hull from its vtable");
	ptrdiff_t ref = 0;
	if (!MsfFindMoveHelperRef(static_cast<const uint8_t*>(tpm), Min(ClientBytes(tpm), kTryPlayerMoveScan), &ref))
		return Fail("MoveHelper not found in TryPlayerMove");
	g_moveHelper = reinterpret_cast<void**>(static_cast<uint8_t*>(tpm) + ref);
	// A static in the client module, set by its constructor when the module loaded.
	void* helper = *g_moveHelper;
	if (!helper || !ClientBytes((*reinterpret_cast<void***>(helper))[kVT_IMoveHelper_AddToTouched]))
		return Fail("MoveHelper doesn't look like one");

	oTryPlayerMove    = reinterpret_cast<TryPlayerMoveFn>(tpm);
	g_tracePlayerBBox = reinterpret_cast<TracePlayerBBoxFn>(tpbb);
	oGetPlayerMins    = reinterpret_cast<HullFn>(g_vt[kVT_GetPlayerMins]);
	oGetPlayerMaxs    = reinterpret_cast<HullFn>(g_vt[kVT_GetPlayerMaxs]);
	g_ready = true;
	InitStuck();
	return true;
}

void Attach()
{
	if (!g_ready || g_attached) return;
	// The getters first: TryPlayerMove's hook relies on them.
	if (!PatchPointer(&g_vt[kVT_GetPlayerMins], reinterpret_cast<void*>(&Hooked_GetPlayerMins))
			|| !PatchPointer(&g_vt[kVT_GetPlayerMaxs], reinterpret_cast<void*>(&Hooked_GetPlayerMaxs))
			|| !PatchPointer(&g_vt[kVT_TryPlayerMove], reinterpret_cast<void*>(&Hooked_TryPlayerMove))) {
		g_attached = true;
		Detach();
		g_ready = false;
		Fail("couldn't patch the GameMovement vtable");
		return;
	}
	g_attached = true;
	if (!g_stuckReady) return;
	if (PatchPointer(&g_vt[kVT_GetCheckInterval], reinterpret_cast<void*>(&Hooked_GetCheckInterval))
			&& PatchPointer(&g_vt[kVT_CheckStuck], reinterpret_cast<void*>(&Hooked_CheckStuck))) {
		g_stuckAttached = true;
		return;
	}
	PatchPointer(&g_vt[kVT_GetCheckInterval], reinterpret_cast<void*>(oGetCheckInterval));
	CoreLog("server stuck check: unavailable (couldn't patch the GameMovement vtable)");
}

void Detach()
{
	if (!g_attached) return;
	PatchPointer(&g_vt[kVT_TryPlayerMove], reinterpret_cast<void*>(oTryPlayerMove));
	PatchPointer(&g_vt[kVT_GetPlayerMins], reinterpret_cast<void*>(oGetPlayerMins));
	PatchPointer(&g_vt[kVT_GetPlayerMaxs], reinterpret_cast<void*>(oGetPlayerMaxs));
	g_attached = false;
	if (!g_stuckAttached) return;
	PatchPointer(&g_vt[kVT_CheckStuck], reinterpret_cast<void*>(oCheckStuck));
	PatchPointer(&g_vt[kVT_GetCheckInterval], reinterpret_cast<void*>(oGetCheckInterval));
	g_stuckAttached = false;
}

void Arm(const Player* p)
{
	g_armed = g_attached && p && p->pl && p->frametime
		&& p->groundOffset > 0 && p->moveTypeOffset > 0 && p->frictionOffset > 0;
	g_stuckArmed = g_stuckAttached && p && p->pl;
	if (p) g_player = *p;
	if (!g_stuckArmed) return;
	// The server's clock never runs backwards; ours restarts with the tickbase on a new connection.
	if (g_stuck.checkTime > p->curtime + 1.0f) g_stuck = MsfStuckState();
	StuckRecord& r = g_stuckLog[p->key & 255];
	if (r.used && r.key == p->key) {
		g_stuck = r.state;
	} else {
		r.key = p->key;
		r.used = true;
		r.state = g_stuck;
	}
}

void SetSettings(const MsfSettings& s, bool log)
{
	const MsfSettings& o = g_settings;
	if (s.known == o.known && (!s.known || (s.bumpcount == o.bumpcount && s.retrace == o.retrace
			&& s.noclipWorkaround == o.noclipWorkaround && s.windowsServer == o.windowsServer && s.sourceJump == o.sourceJump)))
		return;
	g_settings = s;
	if (!log) return;
	char buf[128];
	Describe(buf, sizeof(buf));
	CoreLog("momsurffix prediction: %s", buf);
}

void* MoveHelper()
{
	return g_ready ? *g_moveHelper : nullptr;
}

void Describe(char* buf, size_t size)
{
	const MsfSettings& s = g_settings;
	if (!s.known) snprintf(buf, size, "off (server has no momsurffix2)");
	else snprintf(buf, size, "%sbumpcount %d, retrace length %g, noclip workaround %d%s%s", g_attached ? "" : "unavailable, server: ",
		s.bumpcount, s.retrace, s.noclipWorkaround ? 1 : 0, s.windowsServer ? ", windows server" : "",
		s.sourceJump ? ", bevel and disp fixes" : "");
}

}
