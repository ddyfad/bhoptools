#include "checkpoint_predict.h"
#include "teleport_predict.h"
#include "utils.h"
#include "datamap.h"
#include "sigs.h"
#include "game_layout.h"
#include "speedo.h"
#include "menu_hash.h"
#include "noclip_predict.h"
#include "restart_predict.h"
#include "momsurffix_predict.h"
#include "style_keys.h"
#include "autosync.h"
#include "jumpstats.h"
#include "settings.h"

#include <climits>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace cp {
namespace {

// Must match the server plugin (plugin/README.md).
const int IMPULSE_SAVE = 150;
const int IMPULSE_TELE = 151;
const int IMPULSE_PREV = 152;
const int IMPULSE_NEXT = 153;
// bhoptools_caps flags
const int kServerPrevNext = 1 << 1;
const int kServerRngfix   = 1 << 2;
const int kServerAutobhop = 1 << 3;
const int kServerNoclip   = 1 << 4;
const int kServerMove     = 1 << 5;
const int kServerUntouch  = 1 << 6;
const int kServerMsf      = 1 << 7;
const int kServerMpbhops  = 1 << 8;
const int kServerState    = 1 << 9;
const int kServerCpset    = 1 << 10;
const int kServerRestart  = 1 << 11;
const int kServerKeys     = 1 << 12;
const int kServerAutosync = 1 << 13;
const int kServerZones    = 1 << 16;
const int kServerTeleportView = 1 << 17;
const int kServerHold = 1 << 18;
// bhoptools_hello flags
const int kClientTriggers = 1 << 0;
const int kClientMenus    = 1 << 1;
const int kClientRngfix   = 1 << 2;
const int kClientAutobhop = 1 << 3;
const int kClientNoclip   = 1 << 4;
const int kClientMove     = 1 << 5;
const int kClientUntouch  = 1 << 6;
const int kClientMsf      = 1 << 7;
const int kClientState    = 1 << 8;
const int kClientCpset    = 1 << 9;
const int kClientRestart  = 1 << 10;
const int kClientKeys     = 1 << 11;
const int kClientAutosync = 1 << 12;
const int kClientZones    = 1 << 15;
const int kClientTeleportView = 1 << 16;
const int kClientMenuSound = 1 << 17;
const int kClientJumpstats = 1 << 18;
const int kClientHold = 1 << 20;
const int kMenuKeySave = 1 << 0;     // bhoptools_menu keys
const int kMenuKeyTele = 1 << 1;
const int kRestoreAngles   = 1 << 0;  // bhoptools_cpset, shavit's CP_ANGLES and CP_VELOCITY
const int kRestoreVelocity = 1 << 1;
const int kRestoreAll      = kRestoreAngles | kRestoreVelocity;
const int kProto = 2;
const int kOldFrameWindow = 4;   // must match the server plugin
const double kHelloInterval = 2.0;
// After a few unanswered hellos the server likely has no plugin; keep asking, but quietly.
const double kHelloSlowInterval = 30.0;
const int    kHelloFastTries = 5;
const double kMenuWindow = 1.0;
const int kMaxSlots = 32;

// public/const.h, this branch (FL_ANIMDUCKING at bit 2).
const int FL_ONGROUND   = 1 << 0;
const int FL_DUCKING    = 1 << 1;
const int FL_ATCONTROLS = 1 << 7;
const int FL_CLIENT     = 1 << 8;
const int FL_FAKECLIENT = 1 << 9;
const int PLAYER_FLAG_BITS = 11;
// Shavit restores every flag it saved; bits above PLAYER_FLAG_BITS aren't
// networked and belong to the trigger sim.
const int kRestoreFlags = ((1 << PLAYER_FLAG_BITS) - 1) & ~(FL_ATCONTROLS | FL_CLIENT | FL_FAKECLIENT);

const uint8_t MOVETYPE_NONE = 0, MOVETYPE_WALK = 2, MOVETYPE_NOCLIP = 8, MOVETYPE_LADDER = 9, MOVETYPE_OBSERVER = 10;

const int FCVAR_HIDDEN = 1 << 4;
const int FCVAR_SERVER_CAN_EXECUTE = 1 << 28;

struct Vec { float x, y, z; };

const char kChatNumber[] = "1234567890";  // must match the server plugin

enum SlotState : uint8_t { kPredicted, kConfirmed, kUnknown };

struct Slot {
	int     key;      // command number, or tickbase if that isn't available
	int     id;       // stable id for the trigger sim's snapshots
	int     index;    // shavit's checkpoint number, 0 if unknown
	bool    selected; // made current by prev/next, not saved
	uint8_t state;
	bool    kz;       // server teleports with zero velocity and leaves gravity
	uint8_t movetype;
	bool    ducked, ducking;
	int     flags;
	Vec     origin, velocity, angles;
	float   ducktime, stamina, gravity;
};

struct Turn { int key; float d; float eff; int left; };
struct Shift { int key; float yaw; int tele; };
struct Undo { int key; float pitch, yaw; Vec to; bool angles; };
struct TeleRestore { int key; int restore; };

struct Offsets {
	int tickbase, flags, stamina, ducked, ducking, ducktime;
	int baseVelocity, velocity, absVelocity, lifeState, moveType, gravity;
	int currentCommand;
};

// On Linux written on the installer thread, read only after preload publishes the install flag.
Offsets g_off;
bool    g_offsetsOk;
const char* g_offsetError;  // logged later from Attach

typedef void (*ProcessMovementFn)(void*, void*, void*);
ProcessMovementFn oProcessMovement;
void** g_slot;
void* g_engine;
void* g_engineSound;

bool  g_checked, g_disabled;
#ifdef _WIN32
bool  g_registered;
#endif
bool  g_cmdKeys = true;   // false once m_pCurrentCommand fails validation
bool  g_on = true;        // prediction as last told to the server

Slot  g_slots[kMaxSlots];
std::vector<Slot> g_cps;
int   g_slotCount;
int   g_nextId = 1;
Turn  g_turn;
Shift g_shifts[kMaxSlots];
int   g_shiftCount;
Undo  g_undo[8];
int   g_undoCount;
// What each teleport command restores, fixed when it's first run so replays match it.
TeleRestore g_teleRestore[8];
int   g_teleRestoreCount;

int    g_session;
double g_lastHello;
int    g_helloTries;
bool   g_helloWanted;
int    g_helloSent = -1;
bool   g_caps;           // this session has a protocol-2 server
// Raw mouse totals for the server's cheat detection: off until it's agreed on, whatever the server asks.
const bool kSendFrames = false;
bool   g_framesOn;
int64_t g_inputX, g_inputY;
const int kFrameBatch = 8;
int    g_frameFirst, g_frameCount;
int32_t g_frameX[kFrameBatch], g_frameY[kFrameBatch];
int    g_capsCmd;        // saves after this command get acked
int    g_serverFlags;
bool   g_lastKz;
int    g_clientFlags = kClientMenus | kClientRngfix | kClientAutobhop | kClientNoclip | kClientMove | kClientUntouch | kClientMsf | kClientState | kClientCpset | kClientKeys | kClientRestart | kClientAutosync | kClientZones | kClientTeleportView | kClientMenuSound | kClientJumpstats | kClientHold;
NcLog  g_noclip;
int    g_noclipAllow = NC_ALLOW_ALL;
// Servers without bhoptools_cpset restore both.
int    g_restore = kRestoreAll;
SkTracker g_keys;
AsTracker g_autosync;
// shavit-syncstyle 1.2.1 and later ignore teleports that only set the velocity.
bool g_autosyncMovesOnly;
RsPredictor g_restart;
// laggedmovement to put back once, where a restart we predicted set it and the server didn't.
struct { bool pending; float from, to; } g_laggedFix;

struct Hold {
	bool active;
	int  teleKey;
	bool released;
	int  releaseKey;
	Slot slot;
};
Hold g_hold;
bool g_holdArmed;

bool   g_cpMenu;
int    g_menuSlots;
double g_menuShown = -1;  // when the menu on screen arrived, -1 once a key closed it
// ShowMenu text so far; a long menu comes in parts.
char   g_menuText[1024];
int    g_menuLen;
bool   g_menuMore;
bool   g_menuWasCp;  // the checkpoint menu was on screen when this one's first part came
int    g_menuTime;
// The checkpoint menu as shavit last drew it, so a predicted prev/next or save shows a round
// trip before shavit redraws it.
char   g_menuBase[1024];
int    g_menuBaseLen;
int    g_menuBaseKeys;
bool   g_menuBaseOk;
bool   g_menuHashed;  // the server's hash matched its first g_menuPrefix bytes
int    g_menuPrefix;  // where the items we edit are: the hashed bytes, or all of it
int    g_menuCur;     // current checkpoint when drawn (0 with none)
int    g_menuTotal;
int    g_menuCmd;     // last command the server had run when it drew the menu
int    g_menuDrawn;   // current checkpoint and count the menu on screen shows
int    g_menuDrawnTotal;
// Menu keys 1 and 2 that go out as impulses; 2 only while the teleport item can be picked.
int    g_menuKeys;
bool   g_menuTeleOn;
// What each item of the checkpoint menu is, from bhoptools_menu: one letter per item from item 1.
char   g_menuItems[16];
// The menu on screen, shavit's or our redraw of it. CS:S closes a menu the moment a key picks an
// item, so a key we predict shows it again rather than leave it gone until shavit redraws it.
char   g_shown[sizeof(g_menuBase) + 64];
int    g_shownLen;
int    g_shownKeys;
// The last checkpoint menu the server named, without its numbers and "->" marks, over its first
// g_cpShapeLines lines. shavit redraws the menu twice a second and bhoptools_menu can come a round
// trip after it, so a redraw that only changes numbers stays the checkpoint menu meanwhile.
char   g_cpShape[sizeof(g_menuBase)];
int    g_cpShapeLen;
int    g_cpShapeLines;
// When key 1 last sent a save: shavit's menu doesn't print the chat line sm_save does.
double g_menuSaveAt = -1;

speedo::DispatchUserMessageFn g_dispatch;
void*  g_client;
int    g_chatClient;
int    g_chatMsg = -1;

struct ChatLine {
	char text[256];
	int  len = -1;
	int  have = 0;

	void Clear() { len = -1; have = 0; }
	bool Ready() const { return len > 0 && have == len; }
};
// kChatNumber stands for the checkpoint number.
ChatLine g_chat;
int    g_cpTotal;
int    g_cpTotalCmd;
int    g_cpMax;
// Last value handed to tp::SetEventQueueFix, which restarts the sim, so only on change.
bool   g_eventQueueFix = false;
RngSettings g_rng;
// Last live, non-observer move. Observers get plain commands: shavit prints and redraws for those,
// and the hook may not run at all while spectating.
double g_aliveMoveAt = -1.0;
// bhoptools_lfskip so far; a long list comes in parts.
std::vector<int> g_hazeSkip;
// Fields the current bhoptools_sync dump corrected, -1 outside one.
int    g_syncFixed = -1;
int    g_syncEntries;
bool   g_syncWanted;     // no complete dump yet this session
double g_syncAsked;

template <typename T>
T& Field(void* base, ptrdiff_t off) { return *reinterpret_cast<T*>(static_cast<char*>(base) + off); }

float NormalizeAngle(float a)
{
	while (a > 180.0f)  a -= 360.0f;
	while (a < -180.0f) a += 360.0f;
	return a;
}

float ClampPitch(float p) { return p < -89.0f ? -89.0f : (p > 89.0f ? 89.0f : p); }

// The engine keeps pointers to these for the whole process.
const int kCmdCount = 45;
ConCommandX g_cmds[kCmdCount];

// IN_Impulse, so sm_save and sm_tele set the next usercmd's impulse like a bind.
void (*g_impulseFn)(const CCommandX&);
void (*g_menuSelectFn)(const CCommandX&);
ConCommandX* g_menuSelect;
// say and say_team: ours when the client has none, else the client's with our callback.
ConCommandX  g_sayCmds[2];
ConCommandX* g_say[2];
void (*g_sayFn[2])(const CCommandX&);

int ArgInt(const CCommandX& c, int i) { return i < c.argc ? atoi(c.argv[i]) : 0; }

uint32_t ArgHexBits(const CCommandX& c, int i)
{
	const char* s = i < c.argc ? c.argv[i] : "0";
	const bool neg = *s == '-';
#ifdef _WIN32
	uint32_t bits = (uint32_t)strtoul(neg ? s + 1 : s, nullptr, 16);
#else
	// Not strtoul: glibc 2.38 headers redirect it to __isoc23_strtoul.
	uintptr_t v;
	ParseHex(neg ? s + 1 : s, &v);
	uint32_t bits = (uint32_t)v;
#endif
	return neg ? 0u - bits : bits;  // SourcePawn's %x of a negative cell
}

float ArgHexFloat(const CCommandX& c, int i)
{
	const uint32_t bits = ArgHexBits(c, i);
	float f;
	memcpy(&f, &bits, 4);
	return std::isfinite(f) && fabsf(f) < 1e6f ? f : 0.0f;
}

Slot* FindSlot(int key)
{
	for (int i = 0; i < g_slotCount; ++i) if (g_slots[i].key == key) return &g_slots[i];
	return nullptr;
}

void RemoveSlot(Slot* s)
{
	const int i = (int)(s - g_slots);
	memmove(&g_slots[i], &g_slots[i + 1], sizeof(Slot) * (g_slotCount - i - 1));
	--g_slotCount;
}

// Kept sorted by key; the oldest falls off at the cap.
Slot* AddSlot(int key)
{
	if (Slot* s = FindSlot(key)) return s;
	if (g_slotCount == kMaxSlots) {
		memmove(&g_slots[0], &g_slots[1], sizeof(Slot) * (kMaxSlots - 1));
		--g_slotCount;
	}
	int i = g_slotCount;
	while (i > 0 && g_slots[i - 1].key > key) --i;
	memmove(&g_slots[i + 1], &g_slots[i], sizeof(Slot) * (g_slotCount - i));
	++g_slotCount;
	Slot& s = g_slots[i];
	memset(&s, 0, sizeof(s));
	s.key = key;
	s.id = g_nextId++;
	return &s;
}

// Newest save before this command; the server runs commands in order, so it has the same one.
const Slot* SlotBefore(int key)
{
	for (int i = g_slotCount - 1; i >= 0; --i) if (g_slots[i].key < key) return &g_slots[i];
	return nullptr;
}

void SetRngfix(const RngSettings& s);
void SetEventQueueFixIfChanged(bool on);
bool RedrawMenu(bool force = false);

void ResetMove()
{
	TeleportPredict_SetMoveSettings(0.0f, LANDFIX_OFF, false, INT_MIN);
	g_hazeSkip.clear();
	TeleportPredict_SetHazeSkip(g_hazeSkip);
}

void ResetZones()
{
	TeleportPredict_ClearZones();
}

void ResetSession()
{
	g_hold = Hold();
	g_holdArmed = false;
	g_framesOn = false;
	SetEventQueueFixIfChanged(false);
	RngSettings rng = g_rng;
	rng.known = false;
	rng.autobhop = false;
	SetRngfix(rng);
	ResetMove();
	ResetZones();
	msf::SetSettings(MsfSettings(), false);
	TeleportPredict_SetStateSync(false);
	g_syncFixed = -1;
	g_syncWanted = false;
	g_slotCount = 0;
	g_shiftCount = 0;
	g_undoCount = 0;
	g_turn.left = 0;
	g_caps = false;
	tp::SetUntouchEachCommand(false);
	tp::SetMpbhopsLoaded(false);
	g_capsCmd = 0;
	g_chatMsg = -1;
	g_chat.Clear();
	g_cpTotal = g_cpTotalCmd = g_cpMax = 0;
	g_cps.clear();
	g_noclip.Clear();
	g_noclipAllow = NC_ALLOW_ALL;
	g_restore = kRestoreAll;
	g_keys.Clear();
	g_autosync.Clear();
	g_autosyncMovesOnly = false;
	g_teleRestoreCount = 0;
	g_restart.Clear();
	g_laggedFix.pending = false;
	g_cpMenu = false;
	g_menuShown = -1;
	g_menuMore = false;
	g_menuBaseOk = false;
	g_menuKeys = 0;
	g_menuItems[0] = '\0';
	g_shownLen = 0;
	g_cpShapeLen = 0;
	g_menuSaveAt = -1;
	g_session = (int)((long long)(MonotonicSeconds() * 1000.0) & 0x7fffffff);
	if (!g_session) g_session = 1;
	js::Reset(g_session);
	g_lastHello = 0.0;
	g_helloTries = 0;
	g_aliveMoveAt = -1.0;
}

// Acks arrive in command order, so an unacked save older than an acked command was lost.
void DropLost(int cmdnum)
{
	for (int i = g_slotCount - 1; i >= 0; --i) {
		const Slot& s = g_slots[i];
		if (s.state != kConfirmed && s.key > g_capsCmd && s.key < cmdnum) RemoveSlot(&g_slots[i]);
	}
}

// Saves still in flight add to shavit's count; a confirmed save's own count ack comes a command later.
int CountWithPending(int key)
{
	int n = g_cpTotal;
	for (int i = 0; i < g_slotCount; ++i) {
		const Slot& s = g_slots[i];
		if (s.key <= g_cpTotalCmd || s.key > key || s.selected) continue;
		if (s.state == kConfirmed && s.index > n) n = s.index;
		else if (s.state == kPredicted) ++n;
	}
	return g_cpMax > 0 && n > g_cpMax ? g_cpMax : n;
}

void SetEventQueueFixIfChanged(bool on)
{
	if (on == g_eventQueueFix) return;
	g_eventQueueFix = on;
	tp::SetEventQueueFix(on);
}

void SetRngfix(const RngSettings& s)
{
	const RngSettings& o = g_rng;
	if (s.known == o.known && s.autobhop == o.autobhop && (!s.known || (s.downhill == o.downhill
			&& s.uphill == o.uphill && s.edge == o.edge && s.triggerjump == o.triggerjump
			&& s.telehop == o.telehop && s.stairs == o.stairs && s.oldSlopefix == o.oldSlopefix)))
		return;
	g_rng = s;
	TeleportPredict_SetRngfixSettings(g_rng);
}

void Cmd_Caps(const CCommandX& c)
{
	if (ArgInt(c, 2) != g_session) return;
	// Protocol 0: the plugin is unloading, so back to plain commands until it asks again.
	// Older protocols aren't predicted.
	if (ArgInt(c, 1) < 2) {
		g_caps = false;
		g_framesOn = false;
		g_hold = Hold();
		g_holdArmed = false;
		SetEventQueueFixIfChanged(false);
		tp::SetUntouchEachCommand(false);
		tp::SetMpbhopsLoaded(false);
		SetRngfix(RngSettings());
		ResetMove();
		ResetZones();
		msf::SetSettings(MsfSettings(), false);
		TeleportPredict_SetStateSync(false);
		g_restart.Clear();
		js::Reset(g_session);
		return;
	}
	// A second caps for the same session means the plugin reloaded: it never saw the
	// impulses sent meanwhile and takes the newest confirmed save as current.
	if (g_caps)
		for (int i = g_slotCount - 1; i >= 0; --i)
			if (g_slots[i].state != kConfirmed) RemoveSlot(&g_slots[i]);
	g_caps = true;
	g_capsCmd = ArgInt(c, 3);
	g_hold = Hold();
	g_holdArmed = false;
	g_serverFlags = ArgInt(c, 4);
	tp::SetUntouchEachCommand((g_serverFlags & kServerUntouch) != 0);
	tp::SetMpbhopsLoaded((g_serverFlags & kServerMpbhops) != 0);
	g_chatClient = ArgInt(c, 5);
	g_chatMsg = c.argc > 6 ? ArgInt(c, 6) : -1;
	js::SetChat(g_chatClient, g_chatMsg);
	// The server knows whether it runs eventqueuefix; that beats the default.
	SetEventQueueFixIfChanged((g_serverFlags & 1) != 0);
	// bhoptools_rngcvars, bhoptools_autobhop and bhoptools_noclip follow from servers that report them.
	RngSettings rng = g_rng;
	if (!(g_serverFlags & kServerRngfix)) rng.known = false;
	if (!(g_serverFlags & kServerAutobhop)) rng.autobhop = false;
	SetRngfix(rng);
	if (!(g_serverFlags & kServerMove)) ResetMove();
	if (!(g_serverFlags & kServerMsf)) msf::SetSettings(MsfSettings(), false);
	TeleportPredict_SetStateSync((g_serverFlags & kServerState) != 0);
	// The server dumps its state after the hello; ask again if that doesn't arrive whole.
	g_syncWanted = (g_serverFlags & kServerState) != 0;
	g_syncAsked = MonotonicSeconds();
	g_noclipAllow = NC_ALLOW_ALL;
	if (!(g_serverFlags & kServerCpset)) g_restore = kRestoreAll;
	// The server sends its state again after every caps.
	g_keys.Clear();
	g_autosync.Clear();
	g_autosyncMovesOnly = false;
	// bhoptools_restart follows from servers that take restart impulses.
	g_restart.Clear();
	// The list is read again after every caps; bhoptools_zplayer follows.
	ResetZones();
	TeleportPredict_WatchZones((g_serverFlags & kServerZones) != 0);
	// Past what each packet backs up, the server runs the last command again for every one lost, which
	void* cvar = CreateInterfaceIn(VSTDLIB_MODULE, "VEngineCvar004");
	if (cvar && VFunc<void* (*)(void*, const char*)>(cvar, VT_Cvar_FindBase)(cvar, "cl_cmdbackup"))
		VFunc<void (*)(void*, const char*)>(g_engine, VT_IVEngineClient_ClientCmdUnrestricted)(g_engine, "cl_cmdbackup 7\n");
}

// Not through the engine: the trigger sim would drop an angle matching a teleport snap it awaits.
void SetView(Vec v)
{
	if (TeleportPredict_SetOwnViewAngles(v.x, v.y, v.z)) return;
	VFunc<void (*)(void*, Vec*)>(g_engine, VT_IVEngineClient_SetViewAngles)(g_engine, &v);
}

void ReadState(const CCommandX& c, Slot* s)
{
	s->index    = ArgInt(c, 4);
	s->state    = kConfirmed;
	s->selected = false;
	s->kz       = (ArgInt(c, 5) & 1) != 0;
	s->origin   = { ArgHexFloat(c, 6), ArgHexFloat(c, 7), ArgHexFloat(c, 8) };
	s->velocity = { ArgHexFloat(c, 9), ArgHexFloat(c, 10), ArgHexFloat(c, 11) };
	s->angles   = { ArgHexFloat(c, 12), ArgHexFloat(c, 13), ArgHexFloat(c, 14) };
	s->gravity  = ArgHexFloat(c, 15);
	s->stamina  = ArgHexFloat(c, 16);
	s->ducktime = ArgHexFloat(c, 17);
	s->flags    = ArgInt(c, 18);
	s->movetype = (uint8_t)ArgInt(c, 19);
	s->ducked   = ArgInt(c, 20) != 0;
	s->ducking  = ArgInt(c, 21) != 0;
}

// Keeps the trigger sim snapshot id while the number still holds the same checkpoint;
// after shavit renumbers, the id would belong to another one.
void Remember(Slot& s)
{
	if ((int)g_cps.size() <= s.index) g_cps.resize(s.index + 1);
	Slot& m = g_cps[s.index];
	if (m.index && memcmp(&m.origin, &s.origin, sizeof(Vec)) == 0) s.id = m.id;
	m = s;
}

// <flags> <x> <y> <z> <pitch> <yaw> <gravity> <laggedmovement> from argument `first`, floats as bits.
RsDest ReadDest(const CCommandX& c, int first)
{
	RsDest d;
	d.flags     = ArgInt(c, first);
	d.origin[0] = ArgHexFloat(c, first + 1);
	d.origin[1] = ArgHexFloat(c, first + 2);
	d.origin[2] = ArgHexFloat(c, first + 3);
	d.pitch     = ArgHexFloat(c, first + 4);
	d.yaw       = ArgHexFloat(c, first + 5);
	d.gravity   = ArgHexFloat(c, first + 6);
	d.lagged    = ArgHexFloat(c, first + 7);
	return d;
}

// The view turn predicted for `cmdnum`, settled with what the server did: turned to `to`, or not at
// all. Mouse movement since is kept.
void SettleTurn(int cmdnum, bool turned, const Vec& to)
{
	for (int i = 0; i < g_undoCount; ++i) {
		if (g_undo[i].key != cmdnum) continue;
		const Undo u = g_undo[i];
		g_undo[i] = g_undo[--g_undoCount];
		// The command's own angles, where any turn we made started.
		const float fromPitch = u.to.x - u.pitch;
		const float fromYaw = u.to.y - u.yaw;
		const float dPitch = (turned ? to.x - fromPitch : 0.0f) - u.pitch;
		const float dYaw = NormalizeAngle((turned ? NormalizeAngle(to.y - fromYaw) : 0.0f) - u.yaw);
		if (!g_engine || (fabsf(dPitch) <= 0.01f && fabsf(dYaw) <= 0.01f)) return;
		Vec view;
		VFunc<void (*)(void*, Vec*)>(g_engine, VT_IVEngineClient_GetViewAngles)(g_engine, &view);
		view.x = ClampPitch(view.x + dPitch);
		view.y = NormalizeAngle(view.y + dYaw);
		SetView(view);
		return;
	}
}

// bhoptools_ack <session> <cmd> r <done> <flags> <x> <y> <z> <pitch> <yaw> <gravity> <laggedmovement>
void RestartAck(const CCommandX& c, int cmdnum, bool done)
{
	done = done && c.argc >= 13;
	RsDest d;
	if (done) {
		d = ReadDest(c, 5);
		d.flags |= RS_TELEPORT;
	}
	// laggedmovement isn't restored by replays, so undo ours if the server set something else.
	if (const RsRecord* r = g_restart.Find(cmdnum)) {
		if (r->apply && (r->dest.flags & RS_START) && r->oldLagged >= 0.0f) {
			const float to = (d.flags & RS_START) ? d.lagged : r->oldLagged;
			if (to != r->dest.lagged) g_laggedFix = { true, r->dest.lagged, to };
		}
	}
	const RsRecord* r = g_restart.Ack(cmdnum, done, d);
	SettleTurn(cmdnum, r->apply && (r->dest.flags & RS_ANGLES), Vec{ r->dest.pitch, r->dest.yaw, 0.0f });
}

void Cmd_Ack(const CCommandX& c)
{
	if (c.argc < 5 || !g_caps || !g_cmdKeys || ArgInt(c, 1) != g_session) return;
	const int cmdnum = ArgInt(c, 2);
	const char kind = c.argv[3][0];
	const int index = ArgInt(c, 4);
	DropLost(cmdnum);

	if (kind == 's') {
		if (!index || c.argc < 22) {
			if (Slot* s = FindSlot(cmdnum)) RemoveSlot(s);
			RedrawMenu();
			return;
		}
		Slot* s = AddSlot(cmdnum);
		ReadState(c, s);
		g_lastKz = s->kz;
		Remember(*s);
	} else if (kind == 'l') {
		if (!index || c.argc < 22) return;
		Slot s = {};
		ReadState(c, &s);
		Remember(s);
	} else if (kind == 't') {
		if (!index) {
			// The server shifts nothing after a teleport it refused.
			if (g_turn.key == cmdnum) g_turn.left = 0;
			int n = 0;
			for (int i = 0; i < g_shiftCount; ++i)
				if (g_shifts[i].tele != cmdnum) g_shifts[n++] = g_shifts[i];
			g_shiftCount = n;
		}
		for (int i = 0; i < g_undoCount; ++i) {
			if (g_undo[i].key != cmdnum) continue;
			Vec view;
			// Acks come in command order, so this is the save the server teleported to.
			const Slot* s = SlotBefore(cmdnum);
			if (g_engine && !index) {
				// The server refused a teleport we turned the view for: turn back, keeping mouse movement since.
				VFunc<void (*)(void*, Vec*)>(g_engine, VT_IVEngineClient_GetViewAngles)(g_engine, &view);
				view.x = ClampPitch(view.x - g_undo[i].pitch);
				view.y = NormalizeAngle(view.y - g_undo[i].yaw);
				SetView(view);
			} else if (g_engine && s && s->state == kConfirmed && g_undo[i].angles) {
				const float dPitch = s->angles.x - g_undo[i].to.x;
				const float dYaw = NormalizeAngle(s->angles.y - g_undo[i].to.y);
				if (fabsf(dPitch) > 0.01f || fabsf(dYaw) > 0.01f) {
					VFunc<void (*)(void*, Vec*)>(g_engine, VT_IVEngineClient_GetViewAngles)(g_engine, &view);
					view.x = ClampPitch(view.x + dPitch);
					view.y = NormalizeAngle(view.y + dYaw);
					SetView(view);
				}
			}
			g_undo[i] = g_undo[--g_undoCount];
			break;
		}
	} else if (kind == 'n') {
		g_cpTotal = index;
		g_cpTotalCmd = cmdnum;
		g_cpMax = ArgInt(c, 5);
		const int keep = CountWithPending(INT_MAX);
		if ((int)g_cps.size() > keep + 1) g_cps.resize(keep + 1);
	} else if (kind == 'o') {
		// Replays still running that command take the server's answer; later ones start after it.
		NcAck(g_noclip, cmdnum, index != 0);
	} else if (kind == 'r') {
		RestartAck(c, cmdnum, index != 0);
	} else if (kind == 'c') {
		// The server dropped its impulse checkpoint.
		for (int i = g_slotCount - 1; i >= 0; --i)
			if (g_slots[i].key <= cmdnum) RemoveSlot(&g_slots[i]);
	}
	// A save we drew into the menu may have been refused or lost.
	RedrawMenu();
}

int Clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// bhoptools_rngcvars <session> <loaded> <downhill> <uphill> <edge> <triggerjump> <telehop> <stairs> <oldslopefix>
void Cmd_Rngfix(const CCommandX& c)
{
	if (c.argc < 10 || !g_caps || ArgInt(c, 1) != g_session) return;
	RngSettings rng = g_rng;
	rng.known       = ArgInt(c, 2) != 0;
	rng.downhill    = Clamp(ArgInt(c, 3), 0, 1);
	rng.uphill      = Clamp(ArgInt(c, 4), -1, 1);
	rng.edge        = Clamp(ArgInt(c, 5), 0, 1);
	rng.triggerjump = Clamp(ArgInt(c, 6), 0, 1);
	rng.telehop     = Clamp(ArgInt(c, 7), 0, 1);
	rng.stairs      = Clamp(ArgInt(c, 8), 0, 1);
	rng.oldSlopefix = Clamp(ArgInt(c, 9), 0, 1);
	SetRngfix(rng);
}

// bhoptools_autobhop <session> <0/1>
void Cmd_Autobhop(const CCommandX& c)
{
	if (c.argc < 3 || !g_caps || !(g_serverFlags & kServerAutobhop) || ArgInt(c, 1) != g_session) return;
	RngSettings rng = g_rng;
	rng.autobhop = ArgInt(c, 2) != 0;
	SetRngfix(rng);
}

// bhoptools_cpset <session> <kRestore* flags>
void Cmd_Cpset(const CCommandX& c)
{
	if (c.argc < 3 || !g_caps || !(g_serverFlags & kServerCpset) || ArgInt(c, 1) != g_session) return;
	g_restore = ArgInt(c, 2) & kRestoreAll;
}

// bhoptools_tsnap <session> <command> <pitch> <yaw> <roll>: a teleport turned the view on the server,
// which sent no snap of its own.
void Cmd_TeleportSnap(const CCommandX& c)
{
	if (c.argc < 6 || !g_caps || !(g_serverFlags & kServerTeleportView) || ArgInt(c, 1) != g_session) return;
	TeleportPredict_ServerViewSnap(ArgInt(c, 2), ArgHexFloat(c, 3), ArgHexFloat(c, 4), ArgHexFloat(c, 5));
}

// bhoptools_keys <session> <SK_* flags> <key combo> <command>
void Cmd_Keys(const CCommandX& c)
{
	if (c.argc < 5 || !g_caps || !(g_serverFlags & kServerKeys) || ArgInt(c, 1) != g_session) return;
	g_keys.AddReport(ArgInt(c, 4), ArgInt(c, 2), ArgInt(c, 3));
}

void Cmd_Hold(const CCommandX& c)
{
	if (c.argc < 3 || !g_caps || !(g_serverFlags & kServerHold) || ArgInt(c, 1) != g_session) return;
	g_holdArmed = ArgInt(c, 2) != 0;
}

// bhoptools_autosync <session> <command> <on> <AS_* order> <zero> [movesonly]
void Cmd_Autosync(const CCommandX& c)
{
	if (c.argc < 6 || !g_caps || !(g_serverFlags & kServerAutosync) || ArgInt(c, 1) != g_session) return;
	g_autosync.AddReport(ArgInt(c, 2), ArgInt(c, 3) != 0, ArgInt(c, 4), ArgInt(c, 5));
	g_autosyncMovesOnly = c.argc > 6 && ArgInt(c, 6) != 0;
}

// bhoptools_noclip <session> <NC_ALLOW_* flags>. Only decides commands first seen after it.
void Cmd_NoclipAllow(const CCommandX& c)
{
	if (c.argc < 3 || !g_caps || !(g_serverFlags & kServerNoclip) || ArgInt(c, 1) != g_session) return;
	g_noclipAllow = ArgInt(c, 2) & NC_ALLOW_ALL;
}

// bhoptools_restart <session> <cmd> <flags> <x> <y> <z> <pitch> <yaw> <gravity> <laggedmovement>
void Cmd_RestartState(const CCommandX& c)
{
	if (c.argc < 11 || !g_caps || !(g_serverFlags & kServerRestart) || !g_cmdKeys || ArgInt(c, 1) != g_session) return;
	const RsDest d = ReadDest(c, 3);
	g_restart.SetState(ArgInt(c, 2), d);
}

// bhoptools_move <session> <runspeed, float bits> <landfix> [<easybhop> <cmd>], the last two from pingfix 0.10
void Cmd_Move(const CCommandX& c)
{
	if (c.argc < 4 || !g_caps || !(g_serverFlags & kServerMove) || ArgInt(c, 1) != g_session) return;
	const int landfix = ArgInt(c, 3) >= LANDFIX_OFF && ArgInt(c, 3) <= LANDFIX_OLIVIA ? ArgInt(c, 3) : LANDFIX_OFF;
	const float runspeed = ArgHexFloat(c, 2);
	const bool easybhop = c.argc > 4 && ArgInt(c, 4) != 0;
	const int fromCmd = c.argc > 5 ? ArgInt(c, 5) : INT_MIN;
	TeleportPredict_SetMoveSettings(runspeed, landfix, easybhop, fromCmd);
	CoreLog("bhoptools: runspeed %.2f, landfix %d, easybhop %d, from command %d", runspeed, landfix, easybhop ? 1 : 0, fromCmd);
}

// bhoptools_zplayer <session> <cmd> <settings>
void Cmd_ZonePlayer(const CCommandX& c)
{
	if (c.argc < 3 || !g_caps || !(g_serverFlags & kServerZones) || !g_cmdKeys || ArgInt(c, 1) != g_session) return;
	ZsSettings s;
	if (ZsParseSettings(&c.argv[3], c.argc - 3, s)) TeleportPredict_SetZoneSettings(ArgInt(c, 2) + 1, s);
}

// bhoptools_lfskip <session> <total> <offset> <entity>...
void Cmd_LandfixSkip(const CCommandX& c)
{
	if (c.argc < 4 || !g_caps || !(g_serverFlags & kServerMove) || ArgInt(c, 1) != g_session) return;
	const int total = ArgInt(c, 2);
	const int offset = ArgInt(c, 3);
	if (total < 0 || total > 4096) return;
	if (offset == 0) g_hazeSkip.clear();
	if (offset != (int)g_hazeSkip.size()) return;
	for (int i = 4; i < c.argc && (int)g_hazeSkip.size() < total; ++i) g_hazeSkip.push_back(ArgInt(c, i));
	if ((int)g_hazeSkip.size() == total) TeleportPredict_SetHazeSkip(g_hazeSkip);
}

// bhoptools_msf <session> <loaded> <bumpcount> <retrace length, float bits> <noclip workaround> <os, 1 Windows>
void Cmd_Msf(const CCommandX& c)
{
	if (c.argc < 7 || !g_caps || !(g_serverFlags & kServerMsf) || ArgInt(c, 1) != g_session) return;
	MsfSettings s;
	s.known            = ArgInt(c, 2) != 0;
	s.bumpcount        = Clamp(ArgInt(c, 3), 4, 16);
	const float retrace = ArgHexFloat(c, 4);
	s.retrace          = retrace < 0.2f ? 0.2f : (retrace > 5.0f ? 5.0f : retrace);
	s.noclipWorkaround = ArgInt(c, 5) != 0;
	s.windowsServer    = ArgInt(c, 6) == 1;
	msf::SetSettings(s, true);
}

int HexDigit(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

// They name our commands by number, so the sim's snapshots have to be keyed the same way.
bool SyncMessage(const CCommandX& c, int minArgs)
{
	return c.argc >= minArgs && g_caps && g_cmdKeys && !g_disabled && ArgInt(c, 1) == g_session;
}

// bhoptools_ents <session> <cmd> <count> [<hammerid> <flags> <value, float bits> <wait>]...
void Cmd_Ents(const CCommandX& c)
{
	if (!SyncMessage(c, 4)) return;
	const int cmd = ArgInt(c, 2);
	const int count = ArgInt(c, 3);
	if (count < 0 || count > (c.argc - 4) / 4) return;
	if (g_syncFixed >= 0) g_syncEntries += count;
	for (int k = 0; k < count; ++k) {
		const int a = 4 + k * 4;
		const int fixed = TeleportPredict_SyncEntity(cmd, ArgInt(c, a), ArgInt(c, a + 1), ArgHexFloat(c, a + 2), ArgInt(c, a + 3));
		if (fixed > 0 && g_syncFixed >= 0) g_syncFixed += fixed;
		if (fixed > 0) TeleportPredict_PredLog("state sync after cmd %d: entity hammerid %d had %d fields wrong", cmd, ArgInt(c, a), fixed);
	}
}

// "-" for empty. The plugin writes spaces, quotes, semicolons, % and non-ASCII bytes as %XX, like bhoptools_chat.
std::string SyncName(const char* s)
{
	std::string out;
	if (!strcmp(s, "-")) return out;
	for (; *s; ++s) {
		if (*s == '%' && HexDigit(s[1]) >= 0 && HexDigit(s[2]) >= 0) {
			out += (char)(HexDigit(s[1]) * 16 + HexDigit(s[2]));
			s += 2;
		} else {
			out += *s;
		}
	}
	return out;
}

// bhoptools_player <session> <cmd> <gravity, float bits> <basevelflag> <targetname> <classname>
void Cmd_Player(const CCommandX& c)
{
	if (!SyncMessage(c, 7)) return;
	const int cmd = ArgInt(c, 2);
	const int fixed = TeleportPredict_SyncPlayer(cmd, ArgHexFloat(c, 3), ArgInt(c, 4) != 0,
		SyncName(c.argv[5]).c_str(), SyncName(c.argv[6]).c_str());
	if (fixed > 0 && g_syncFixed >= 0) g_syncFixed += fixed;
	if (fixed > 0) TeleportPredict_PredLog("state sync after cmd %d: player had %d fields wrong", cmd, fixed);
}

// bhoptools_sync <session> <cmd> begin|end [entries]. The engine drops server commands that overflow
// its buffer, so a dump that lost entries is asked for again.
void Cmd_Sync(const CCommandX& c)
{
	if (!SyncMessage(c, 4)) return;
	if (!strcmp(c.argv[3], "begin")) {
		g_syncFixed = 0;
		g_syncEntries = 0;
		g_syncWanted = true;
		g_syncAsked = MonotonicSeconds();
	} else if (!strcmp(c.argv[3], "end") && g_syncFixed >= 0) {
		const int sent = c.argc > 4 ? ArgInt(c, 4) : g_syncEntries;
		if (g_syncEntries == sent) g_syncWanted = false;
		else g_syncAsked = 0.0;
		g_syncFixed = -1;
	}
}

void Cmd_Ask(const CCommandX&)
{
	g_helloWanted = true;
	g_lastHello = 0.0;
	g_helloTries = 0;
}

void AddChatPart(ChatLine& l, const CCommandX& c, int first)
{
	const int total = ArgInt(c, first);
	const int offset = ArgInt(c, first + 1);
	if (total < 0 || total >= (int)sizeof(l.text)) return;
	if (offset == 0) {
		l.len = total;
		l.have = 0;
	}
	if (offset != l.have || l.len != total) return;
	for (const char* p = c.argv[first + 2]; *p && l.have < l.len; ++p) {
		int b = (unsigned char)*p;
		if (b == '%' && HexDigit(p[1]) >= 0 && HexDigit(p[2]) >= 0) {
			b = HexDigit(p[1]) * 16 + HexDigit(p[2]);
			p += 2;
		}
		l.text[l.have++] = (char)b;
	}
	l.text[l.have] = '\0';
}

void Cmd_Chat(const CCommandX& c)
{
	if (c.argc < 5 || ArgInt(c, 1) != g_session) return;
	AddChatPart(g_chat, c, 2);
}

// The same bytes as Shavit_PrintToChat's SayText2.
void PrintChat(const char* text, int len)
{
	unsigned char buf[264] = {};  // zero padded: bf_read reads whole dwords
	buf[0] = (unsigned char)g_chatClient;
	buf[1] = 1;
	if (len > 252) len = 252;
	memcpy(buf + 2, text, len);
	len += 3;
	BfRead msg = { buf, len, len * 8, 0, false, false, "bhoptools" };
	g_dispatch(g_client, g_chatMsg, &msg);
}

void PrintSaved(int key)
{
	if (!g_dispatch || g_chatMsg < 0 || !g_chat.Ready()) return;
	const char* at = strstr(g_chat.text, kChatNumber);
	if (!at) return;

	const int n = CountWithPending(key);
	char line[256];
	const int len = snprintf(line, sizeof(line), "%.*s%d%s", (int)(at - g_chat.text), g_chat.text, n, at + sizeof(kChatNumber) - 1);
	PrintChat(line, len < (int)sizeof(line) ? len : (int)sizeof(line) - 1);
}

bool SendImpulse(int impulse)
{
	const bool known = NcIsImpulse(impulse) ? (g_serverFlags & kServerNoclip) != 0
		: RsIsImpulse(impulse) ? (g_serverFlags & kServerRestart) != 0
		: impulse < IMPULSE_PREV || (g_serverFlags & kServerPrevNext);
	const bool alive = g_aliveMoveAt >= 0.0 && MonotonicSeconds() - g_aliveMoveAt < 0.25;
	if (!tp::Enabled() || !g_caps || !known || !alive || g_disabled || !g_impulseFn) return false;
	char n[8];
	snprintf(n, sizeof(n), "%d", impulse);
	CCommandX imp = {};
	imp.argc = 2;
	imp.argv[0] = "impulse";
	imp.argv[1] = n;
	g_impulseFn(imp);
	return true;
}

void ForwardToServer(const CCommandX& c)
{
	if (g_engine) VFunc<void (*)(void*, const char*, bool)>(g_engine, VT_IVEngineClient_ServerCmd)(g_engine, c.argS, true);
}

// Numbered teleports and servers without protocol 2 get the command as typed.
void Checkpoint(const CCommandX& c, int impulse)
{
	if (c.argc == 1 && SendImpulse(impulse)) {
		g_menuSaveAt = -1;  // this impulse replaces a menu key's
		return;
	}
	ForwardToServer(c);
}

// The last line of the menu starting with the item's number: "->3. " when it can be picked, "3. " when not.
int MenuItemLine(const char* text, int len, int item, bool* enabled)
{
	const char num[4] = { (char)('0' + item), '.', ' ', 0 };
	int found = -1;
	for (int i = 0; i < len; ) {
		const char* p = text + i;
		if (!strncmp(p, "->", 2) && !strncmp(p + 2, num, 3)) { found = i; *enabled = true; }
		else if (!strncmp(p, num, 3)) { found = i; *enabled = false; }
		const char* nl = static_cast<const char*>(memchr(p, '\n', len - i));
		if (!nl) break;
		i = (int)(nl - text) + 1;
	}
	return found;
}

// Where the item's text (after "->1. " or "1. ") holds the number `n` exactly once, and the
// biggest other number in it; false if it doesn't, since then we can't tell which to change.
bool ItemNumber(int line, bool enabled, int n, int* at, int* len, int* other)
{
	const char* end = static_cast<const char*>(memchr(g_menuBase + line, '\n', g_menuPrefix - line));
	const int lineEnd = end ? (int)(end - g_menuBase) : g_menuPrefix;
	*at = -1;
	*other = -1;
	for (int i = line + (enabled ? 5 : 3); i < lineEnd; ) {
		if (g_menuBase[i] < '0' || g_menuBase[i] > '9') { ++i; continue; }
		int j = i;
		while (j < lineEnd && g_menuBase[j] >= '0' && g_menuBase[j] <= '9') ++j;
		const int v = j - i < 8 ? atoi(g_menuBase + i) : -1;
		if (v == n) {
			if (*at >= 0) return false;
			*at = i;
			*len = j - i;
		} else if (v > *other) {
			*other = v;
		}
		i = j;
	}
	return *at >= 0;
}

// Shows a menu the way SourceMod's radio style sends it: in 240-byte parts the client joins.
void SendMenu(const char* text, int len, int keys)
{
	const int id = speedo::WatchedMessage();
	if (!g_dispatch || id < 0 || len <= 0) return;
	for (int off = 0; off < len; ) {
		const int part = len - off > 240 ? 240 : len - off;
		unsigned char buf[264] = {};  // zero padded: bf_read reads whole dwords
		buf[0] = (unsigned char)(keys & 0xff);
		buf[1] = (unsigned char)(keys >> 8);
		buf[2] = (unsigned char)g_menuTime;
		buf[3] = off + part < len ? 1 : 0;
		memcpy(buf + 4, text + off, part);
		const int bytes = part + 5;
		BfRead msg = { buf, bytes, bytes * 8, 0, false, false, "bhoptools" };
		g_dispatch(g_client, id, &msg);
		off += part;
	}
	if (text != g_shown) memcpy(g_shown, text, len);
	g_shownLen = len;
	g_shownKeys = keys;
}

bool RedrawMenu(bool force)
{
	const int id = speedo::WatchedMessage();
	if (!g_menuBaseOk || !g_cpMenu || g_menuMore || !g_cmdKeys || !g_dispatch || id < 0) return false;
	int cur = g_menuCur, total = g_menuTotal, saves = 0;
	for (int i = 0; i < g_slotCount; ++i) {
		const Slot& s = g_slots[i];
		if (s.key <= g_menuCmd) continue;
		if (s.selected) {
			cur = s.index;
			continue;
		}
		// Unknown: the server may or may not have saved. Unhashed: the first item may not be the save.
		if (s.state == kUnknown || !g_menuHashed) return false;
		cur = ++total;
		++saves;
		// A confirmed one that isn't the newest is the server following a current checkpoint it
		// changed some other way, which it redraws the menu for itself.
		if (s.state == kConfirmed && s.index != total) return false;
	}
	// At the segmented cap a save drops the oldest and renumbers the rest.
	if (g_cpMax > 0 && total > g_cpMax) return false;
	if (!force && cur == g_menuDrawn && total == g_menuDrawnTotal) return false;

	bool en[5];
	int line[5];
	for (int item = 1; item <= 4; ++item)
		if ((line[item] = MenuItemLine(g_menuBase, g_menuPrefix, item, &en[item])) < 0) return false;

	struct Edit { int at, cut; const char* put; } edits[5];
	int n = 0;
	char num[12], count[12];
	// shavit shows checkpoint 1 on the greyed-out teleport item while there are none.
	int at, len, other;
	if (!ItemNumber(line[2], en[2], g_menuTotal > 0 ? g_menuCur : 1, &at, &len, &other)) return false;
	snprintf(num, sizeof(num), "%d", total > 0 ? cur : 1);
	edits[n++] = { at, len, num };
	if (saves) {
		// The save item shows the count after a save, then the most there can be: only while
		// that stays above it, since shavit shows the smaller of the two.
		if (!ItemNumber(line[1], en[1], g_menuTotal + 1, &at, &len, &other) || total + 1 > other) return false;
		snprintf(count, sizeof(count), "%d", total + 1);
		edits[n++] = { at, len, count };
		edits[n++] = { line[2], en[2] ? 2 : 0, total > 0 ? "->" : "" };
	}
	edits[n++] = { line[3], en[3] ? 2 : 0, cur > 1 ? "->" : "" };
	edits[n++] = { line[4], en[4] ? 2 : 0, cur < total ? "->" : "" };

	// Applied in text order.
	for (int a = 0; a < n; ++a)
		for (int b = a + 1; b < n; ++b)
			if (edits[b].at < edits[a].at) { const Edit t = edits[a]; edits[a] = edits[b]; edits[b] = t; }
	char out[sizeof(g_menuBase) + 64];
	int outLen = 0, from = 0;
	for (int e = 0; e < n; ++e) {
		memcpy(out + outLen, g_menuBase + from, edits[e].at - from);
		outLen += edits[e].at - from;
		const int put = (int)strlen(edits[e].put);
		memcpy(out + outLen, edits[e].put, put);
		outLen += put;
		from = edits[e].at + edits[e].cut;
	}
	memcpy(out + outLen, g_menuBase + from, g_menuBaseLen - from);
	outLen += g_menuBaseLen - from;

	// SourceMod marks every key valid on games whose menu closes on an invalid one.
	int keys = g_menuBaseKeys;
	if (keys != 0x7ff) {
		keys &= ~(saves ? 0xe : 0xc);
		keys |= (cur > 1 ? 0x4 : 0) | (cur < total ? 0x8 : 0) | (saves && total > 0 ? 0x2 : 0);
	}
	SendMenu(out, outLen, keys);
	g_menuSlots = keys;
	g_menuDrawn = cur;
	g_menuDrawnTotal = total;
	g_menuTeleOn = saves ? total > 0 : en[2];
	return true;
}

// Puts the menu back after a key CS:S closed it for: redrawn if our predictions change it, else as it was.
void KeepMenu()
{
	if (!RedrawMenu(true) && g_cpMenu && g_shownLen > 0) SendMenu(g_shown, g_shownLen, g_shownKeys);
}

bool FlipBox(char* text, int len, int item)
{
	bool enabled = false;
	const int line = MenuItemLine(text, len, item, &enabled);
	if (line < 0) return false;
	char* box = text + line + (enabled ? 5 : 3);
	if (box + 4 > text + len || box[0] != '[' || box[2] != ']' || box[3] != ' ' || (box[1] != 'x' && box[1] != ' ')) return false;
	box[1] = box[1] == 'x' ? ' ' : 'x';
	return true;
}

// Not on a greyed-out teleport, which SourceMod takes as closing the menu.
void Cmd_MenuSelect(const CCommandX& c)
{
	const int key = ArgInt(c, 1);
	const char item = g_cpMenu && key >= 1 && key <= (int)strlen(g_menuItems) ? g_menuItems[key - 1] : 0;
	// The plugin mutes SourceMod's sounds on this menu (kClientMenuSound). Its sounds by slot, from
	// core.games: exit on 0, the select sound on any other live key, none on a greyed-out one.
	// Not through `play`: menuselect runs restricted, and so would anything it adds to the buffer.
	if (g_engineSound && g_cpMenu && key >= 1 && key <= 10 && (g_shownKeys & (1 << (key - 1))))
		VFunc<void (*)(void*, const char*, float, int, int, float)>(g_engineSound, VT_IEngineSound_EmitAmbientSound)(
			g_engineSound, key == 10 ? "buttons/combine_button7.wav" : "buttons/button14.wav", 1.0f, 100, 0, 0.0f);
	if (g_cpMenu && (key == 3 || key == 4) && SendImpulse(key == 4 ? IMPULSE_NEXT : IMPULSE_PREV)) {
		g_menuSaveAt = -1;  // this impulse replaces a key 1 still waiting for its command
		KeepMenu();
		return;
	}
	if (g_cpMenu && key == 1 && (g_menuKeys & kMenuKeySave) && SendImpulse(IMPULSE_SAVE)) {
		g_menuSaveAt = MonotonicSeconds();
		KeepMenu();
		return;
	}
	if (g_cpMenu && key == 2 && (g_menuKeys & kMenuKeyTele) && g_menuTeleOn && SendImpulse(IMPULSE_TELE)) {
		g_menuSaveAt = -1;
		KeepMenu();
		return;
	}
	// The rest go to the server, and shavit redraws the menu after these: the angle and velocity
	// boxes are ticked here straight away, the others stay up as they were until the redraw.
	if (item == 'a' || item == 'v' || item == 'd' || item == '-' || item == '+' || item == 'z') {
		g_menuSelectFn(c);
		if (item == 'a' || item == 'v') {
			const int bit = item == 'a' ? kRestoreAngles : kRestoreVelocity;
			if (FlipBox(g_shown, g_shownLen, key)) {
				if (g_menuBaseOk) FlipBox(g_menuBase, g_menuBaseLen, key);
				// The server takes the new setting from its next command, which comes after this.
				if (g_serverFlags & kServerCpset) g_restore ^= bit;
				else g_menuKeys &= ~kMenuKeyTele;
			}
		}
		if (g_shownLen > 0) SendMenu(g_shown, g_shownLen, g_shownKeys);
		return;
	}
	g_cpMenu = false;  // any other selection closes the menu on screen
	g_menuShown = -1;
	g_menuBaseOk = false;
	g_menuKeys = 0;
	g_menuSelectFn(c);
}

int MenuShape(const char* text, int len, int lines, char* out, int size)
{
	int n = 0, seen = 0;
	for (int i = 0; i < len && seen < lines && n < size; ++i) {
		if (text[i] >= '0' && text[i] <= '9') continue;
		if (text[i] == '-' && i + 1 < len && text[i + 1] == '>') { ++i; continue; }
		if (text[i] == '\n') ++seen;
		out[n++] = text[i];
	}
	return seen == lines ? n : -1;
}

// ShowMenu is dispatched while the packet is read and this stuffed command runs later, so it
// refers to the menu already on screen. Without the hash it's whichever menu came last, within
// kMenuWindow; with it, however late the server's reliable stream brings it.
// bhoptools_menu <session> [<current> <total> <last command run> [<length> <hash> <keys> [<items>]]]
void Cmd_Menu(const CCommandX& c)
{
	const bool hashed = c.argc >= 8;
	if (ArgInt(c, 1) != g_session || g_menuShown < 0 || (!hashed && MonotonicSeconds() - g_menuShown >= kMenuWindow)) return;
	int prefix = g_menuLen;
	if (hashed) {
		prefix = ArgInt(c, 5);
		const uint32_t hash = (uint32_t)(int32_t)strtol(c.argv[6], nullptr, 10);
		// For another menu than the one on screen.
		if (g_menuMore || prefix <= 0 || prefix > g_menuLen || MenuHash(g_menuText, prefix) != hash) return;
		int lines = 0;
		for (int i = 0; i < prefix; ++i) lines += g_menuText[i] == '\n';
		g_cpShapeLines = lines;
		g_cpShapeLen = MenuShape(g_menuText, prefix, lines, g_cpShape, sizeof(g_cpShape));
	}
	g_cpMenu = true;
	g_menuKeys = hashed ? ArgInt(c, 7) & (kMenuKeySave | kMenuKeyTele) : 0;
	snprintf(g_menuItems, sizeof(g_menuItems), "%s", c.argc >= 9 ? c.argv[8] : "");
	g_menuTeleOn = false;
	g_menuBaseOk = false;
	const int cur = ArgInt(c, 2), total = ArgInt(c, 3);
	if (c.argc < 5 || g_menuMore) return;
	if (!(cur >= 1 && cur <= total) && !(hashed && cur == 0 && total == 0)) return;
	memcpy(g_menuBase, g_menuText, g_menuLen + 1);
	g_menuBaseLen = g_menuLen;
	g_menuBaseKeys = g_menuSlots;
	g_menuBaseOk = true;
	g_menuHashed = hashed;
	g_menuPrefix = prefix;
	g_menuCur = g_menuDrawn = cur;
	g_menuTotal = g_menuDrawnTotal = total;
	g_menuCmd = ArgInt(c, 4);
	bool on = false;
	g_menuTeleOn = MenuItemLine(g_menuBase, g_menuPrefix, 2, &on) >= 0 && on;
	// Drawn before the server ran a prev/next or save we predicted, like shavit's twice-a-second redraw.
	RedrawMenu();
}

// ShowMenu: keys (short), time (char), more to come (byte), text.
void OnShowMenu(const BfRead& msg)
{
	if (msg.curBit & 7) return;
	const int at = msg.curBit / 8;
	if (at + 4 > msg.bytes) return;
	if (!g_menuMore) {
		g_menuLen = 0;
		g_menuWasCp = g_cpMenu;
	}
	for (int i = at + 4; i < msg.bytes && msg.data[i] && g_menuLen < (int)sizeof(g_menuText) - 1; ++i)
		g_menuText[g_menuLen++] = (char)msg.data[i];
	g_menuText[g_menuLen] = '\0';
	g_menuMore = msg.data[at + 3] != 0;
	g_menuBaseOk = false;
	g_cpMenu = false;
	if (g_menuMore) return;
	g_menuSlots = msg.data[at] | (msg.data[at + 1] << 8);
	g_menuTime = msg.data[at + 2];
	g_menuShown = MonotonicSeconds();
	memcpy(g_shown, g_menuText, g_menuLen);
	g_shownLen = g_menuLen;
	g_shownKeys = g_menuSlots;
	// The same checkpoint menu with other numbers: keys stay predicted until its bhoptools_menu says
	// what the numbers are, and a predicted key shows this one again meanwhile.
	char shape[sizeof(g_cpShape)];
	if (g_menuWasCp && g_cpShapeLen > 0 && MenuShape(g_menuText, g_menuLen, g_cpShapeLines, shape, sizeof(shape)) == g_cpShapeLen
			&& memcmp(shape, g_cpShape, g_cpShapeLen) == 0) {
		g_cpMenu = true;
		bool on = false;
		g_menuTeleOn = MenuItemLine(g_menuText, g_menuLen, 2, &on) >= 0 && on;
		return;
	}
	g_menuKeys = 0;
	g_menuItems[0] = '\0';
}

void Cmd_Save(const CCommandX& c) { Checkpoint(c, IMPULSE_SAVE); }
void Cmd_Tele(const CCommandX& c) { Checkpoint(c, IMPULSE_TELE); }
void Cmd_PrevCp(const CCommandX& c) { Checkpoint(c, IMPULSE_PREV); }
void Cmd_NextCp(const CCommandX& c) { Checkpoint(c, IMPULSE_NEXT); }

// sm_noclip <target> is SourceMod's admin noclip, so only argument-less toggles are predicted.
// A bound +noclip/-noclip gets the key number as an argument.
void Noclip(const CCommandX& c, int impulse)
{
	if ((c.argc == 1 || impulse != NC_IMPULSE_TOGGLE) && SendImpulse(impulse)) {
		g_menuSaveAt = -1;  // this impulse replaces a menu key 1's
		return;
	}
	ForwardToServer(c);
}

// shavit-core's Command_StartTimer ignores sm_r's arguments, but only the plain command is sent as
// the impulse; sm_b, sm_m and the rest go to the server as typed.
void Cmd_Restart(const CCommandX& c) { Checkpoint(c, RS_IMPULSE_RESTART); }
void Cmd_SetStart(const CCommandX& c) { Checkpoint(c, RS_IMPULSE_SETSTART); }

void Cmd_NoclipToggle(const CCommandX& c) { Noclip(c, NC_IMPULSE_TOGGLE); }
void Cmd_NoclipOn(const CCommandX& c) { Noclip(c, NC_IMPULSE_ON); }
void Cmd_NoclipOff(const CCommandX& c) { Noclip(c, NC_IMPULSE_OFF); }

// SourceMod runs "!cmd" and "/cmd" said in chat as sm_cmd. The ones we take are run here instead,
// so "!cmd" doesn't show the line in chat; the rest are said as usual.
bool ChatTrigger(const CCommandX& c)
{
	const char* p = c.argS + c.argv0Size;
	while (*p == ' ') ++p;
	char text[256];
	snprintf(text, sizeof(text), "%s", *p == '"' ? p + 1 : p);
	size_t len = strlen(text);
	if (len && text[len - 1] == '"') text[--len] = '\0';
	if (text[0] != '!' && text[0] != '/') return false;

	CCommandX cmd = {};
	cmd.argv0Size = snprintf(cmd.argS, sizeof(cmd.argS), "sm_%s", text + 1);
	for (char* q = cmd.argS; *q && *q != ' '; ++q) *q = (char)tolower((unsigned char)*q);
	int used = 0;
	for (const char* q = cmd.argS; *q && cmd.argc < 64; ) {
		while (*q == ' ') ++q;
		if (!*q) break;
		const char* start = q;
		while (*q && *q != ' ') ++q;
		const int n = (int)(q - start);
		if (used + n + 1 > (int)sizeof(cmd.argvBuf)) break;
		memcpy(cmd.argvBuf + used, start, n);
		cmd.argvBuf[used + n] = '\0';
		if (!cmd.argc) cmd.argv0Size = (int)(q - cmd.argS) + (*q ? 1 : 0);
		cmd.argv[cmd.argc++] = cmd.argvBuf + used;
		used += n + 1;
	}
	if (!cmd.argc) return false;
	for (const ConCommandX& ours : g_cmds) {
		if (strncmp(ours.name, "sm_", 3) || strcmp(ours.name, cmd.argv[0])) continue;
		reinterpret_cast<void (*)(const CCommandX&)>(ours.callback)(cmd);
		return true;
	}
	return false;
}

void Say(const CCommandX& c, int i)
{
	if (ChatTrigger(c)) return;
	if (g_sayFn[i]) g_sayFn[i](c);
	else ForwardToServer(c);
}

void FlushFrames()
{
	if (g_frameCount && g_framesOn && g_engine) {
		char line[320];
		int n = snprintf(line, sizeof(line), "bhoptools_frame %d %d", g_session, g_frameFirst);
		for (int i = 0; i < g_frameCount; ++i)
			n += snprintf(line + n, sizeof(line) - n, " %d %d", g_frameX[i], g_frameY[i]);
		VFunc<void (*)(void*, const char*, bool)>(g_engine, VT_IVEngineClient_ServerCmd)(g_engine, line, false);
	}
	g_frameCount = 0;
}

void Cmd_Frames(const CCommandX& c)
{
	if (kSendFrames && g_session && ArgInt(c, 1) == g_session) g_framesOn = ArgInt(c, 2) != 0;
}

void Cmd_Say(const CCommandX& c) { Say(c, 0); }
void Cmd_SayTeam(const CCommandX& c) { Say(c, 1); }

bool RegisterCommands()
{
	void* cvar = CreateInterfaceIn(VSTDLIB_MODULE, "VEngineCvar004");
	if (!cvar) return false;

	void* echo = VFunc<void* (*)(void*, const char*)>(cvar, VT_Cvar_FindBase)(cvar, "echo");
	if (!echo || !VFunc<bool (*)(void*)>(echo, VT_CCB_IsCommand)(echo)) return false;
	const char* name = VFunc<const char* (*)(void*)>(echo, VT_CCB_GetName)(echo);
	if (!name || strcmp(name, "echo") != 0) return false;
	void* vtable = *reinterpret_cast<void**>(echo);

	const struct { const char* name; const char* help; void (*fn)(const CCommandX&); int flags; } defs[] = {
		{ "bhoptools_caps", "Sent by the bhoptools server plugin.", Cmd_Caps, FCVAR_SERVER_CAN_EXECUTE },
		{ "bhoptools_ack", "Sent by the bhoptools server plugin.", Cmd_Ack, FCVAR_SERVER_CAN_EXECUTE },
		{ "bhoptools_chat", "Sent by the bhoptools server plugin.", Cmd_Chat, FCVAR_SERVER_CAN_EXECUTE },
		{ "bhoptools_menu", "Sent by the bhoptools server plugin.", Cmd_Menu, FCVAR_SERVER_CAN_EXECUTE },
		{ "bhoptools_ask", "Sent by the bhoptools server plugin.", Cmd_Ask, FCVAR_SERVER_CAN_EXECUTE },
		{ "bhoptools_rngcvars", "Sent by the bhoptools server plugin.", Cmd_Rngfix, FCVAR_SERVER_CAN_EXECUTE },
		{ "bhoptools_autobhop", "Sent by the bhoptools server plugin.", Cmd_Autobhop, FCVAR_SERVER_CAN_EXECUTE },
		{ "bhoptools_noclip", "Sent by the bhoptools server plugin.", Cmd_NoclipAllow, FCVAR_SERVER_CAN_EXECUTE },
		{ "bhoptools_move", "Sent by the bhoptools server plugin.", Cmd_Move, FCVAR_SERVER_CAN_EXECUTE },
		{ "bhoptools_lfskip", "Sent by the bhoptools server plugin.", Cmd_LandfixSkip, FCVAR_SERVER_CAN_EXECUTE },
		{ "bhoptools_msf", "Sent by the bhoptools server plugin.", Cmd_Msf, FCVAR_SERVER_CAN_EXECUTE },
		{ "bhoptools_cpset", "Sent by the bhoptools server plugin.", Cmd_Cpset, FCVAR_SERVER_CAN_EXECUTE },
		{ "bhoptools_keys", "Sent by the bhoptools server plugin.", Cmd_Keys, FCVAR_SERVER_CAN_EXECUTE },
		{ "bhoptools_hold", "Sent by the bhoptools server plugin.", Cmd_Hold, FCVAR_SERVER_CAN_EXECUTE },
		{ "bhoptools_autosync", "Sent by the bhoptools server plugin.", Cmd_Autosync, FCVAR_SERVER_CAN_EXECUTE },
		{ "bhoptools_ents", "Sent by the bhoptools server plugin.", Cmd_Ents, FCVAR_SERVER_CAN_EXECUTE },
		{ "bhoptools_player", "Sent by the bhoptools server plugin.", Cmd_Player, FCVAR_SERVER_CAN_EXECUTE },
		{ "bhoptools_sync", "Sent by the bhoptools server plugin.", Cmd_Sync, FCVAR_SERVER_CAN_EXECUTE },
		{ "bhoptools_restart", "Sent by the bhoptools server plugin.", Cmd_RestartState, FCVAR_SERVER_CAN_EXECUTE },
		{ "bhoptools_frames", "Sent by the bhoptools server plugin.", Cmd_Frames, FCVAR_SERVER_CAN_EXECUTE },
		{ "bhoptools_zplayer", "Sent by the bhoptools server plugin.", Cmd_ZonePlayer, FCVAR_SERVER_CAN_EXECUTE },
		{ "bhoptools_tsnap", "Sent by the bhoptools server plugin.", Cmd_TeleportSnap, FCVAR_SERVER_CAN_EXECUTE },
		{ "bhoptools_jsset", "Sent by bgs-jumpstats.", js::Cmd_Settings, FCVAR_SERVER_CAN_EXECUTE },
		{ "bhoptools_jscc", "Sent by bgs-jumpstats.", js::Cmd_Colors, FCVAR_SERVER_CAN_EXECUTE },
		{ "bhoptools_jscmp", "Sent by bgs-jumpstats.", js::Cmd_Comparison, FCVAR_SERVER_CAN_EXECUTE },
		{ "bhoptools_jsjump", "Sent by bgs-jumpstats.", js::Cmd_Jump, FCVAR_SERVER_CAN_EXECUTE },
		{ "bhoptools_jstime", "Sent by bgs-jumpstats.", js::Cmd_Time, FCVAR_SERVER_CAN_EXECUTE },
		// Server-executable because a server could always stuff these before we took them over.
		{ "sm_save", "Save a checkpoint (predicted on bhoptools servers).", Cmd_Save, FCVAR_SERVER_CAN_EXECUTE },
		{ "sm_tele", "Teleport to the current checkpoint (predicted on bhoptools servers).", Cmd_Tele, FCVAR_SERVER_CAN_EXECUTE },
		{ "sm_prevcp", "Select the previous checkpoint (predicted on bhoptools servers).", Cmd_PrevCp, FCVAR_SERVER_CAN_EXECUTE },
		{ "sm_nextcp", "Select the next checkpoint (predicted on bhoptools servers).", Cmd_NextCp, FCVAR_SERVER_CAN_EXECUTE },
		{ "sm_nc", "Toggle noclip (predicted on bhoptools servers).", Cmd_NoclipToggle, FCVAR_SERVER_CAN_EXECUTE },
		{ "sm_noclip", "Toggle noclip (predicted on bhoptools servers).", Cmd_NoclipToggle, FCVAR_SERVER_CAN_EXECUTE },
		{ "sm_noclipme", "Toggle noclip (predicted on bhoptools servers).", Cmd_NoclipToggle, FCVAR_SERVER_CAN_EXECUTE },
		{ "sm_prac", "Toggle noclip (predicted on bhoptools servers).", Cmd_NoclipToggle, FCVAR_SERVER_CAN_EXECUTE },
		{ "sm_practice", "Toggle noclip (predicted on bhoptools servers).", Cmd_NoclipToggle, FCVAR_SERVER_CAN_EXECUTE },
		{ "+noclip", "Noclip while held (predicted on bhoptools servers).", Cmd_NoclipOn, FCVAR_SERVER_CAN_EXECUTE },
		{ "-noclip", "Noclip while held (predicted on bhoptools servers).", Cmd_NoclipOff, FCVAR_SERVER_CAN_EXECUTE },
		{ "sm_r", "Restart the timer (predicted on bhoptools servers).", Cmd_Restart, FCVAR_SERVER_CAN_EXECUTE },
		{ "sm_restart", "Restart the timer (predicted on bhoptools servers).", Cmd_Restart, FCVAR_SERVER_CAN_EXECUTE },
		{ "sm_start", "Restart the timer (predicted on bhoptools servers).", Cmd_Restart, FCVAR_SERVER_CAN_EXECUTE },
		{ "sm_sp", "Set the restart position (predicted on bhoptools servers).", Cmd_SetStart, FCVAR_SERVER_CAN_EXECUTE },
		{ "sm_ss", "Set the restart position (predicted on bhoptools servers).", Cmd_SetStart, FCVAR_SERVER_CAN_EXECUTE },
		{ "sm_setstart", "Set the restart position (predicted on bhoptools servers).", Cmd_SetStart, FCVAR_SERVER_CAN_EXECUTE },
		{ "sm_startpoint", "Set the restart position (predicted on bhoptools servers).", Cmd_SetStart, FCVAR_SERVER_CAN_EXECUTE },
	};
	static_assert(sizeof(defs) / sizeof(defs[0]) == kCmdCount, "kCmdCount must match defs");
	for (int i = 0; i < kCmdCount; ++i) {
		ConCommandX& c = g_cmds[i];
		c.vtable = vtable;
		c.name = defs[i].name;
		c.help = defs[i].help;
		c.flags = defs[i].flags | FCVAR_HIDDEN;  // the plugin's protocol and shavit's names: nothing to browse
		c.callback = (void*)defs[i].fn;
		c.bits = 0x02;  // new-style callback taking const CCommand&
		VFunc<void (*)(void*, void*)>(cvar, VT_Cvar_Register)(cvar, &c);
	}

	// Only a plain new-style callback (no callback interface) can be called this way.
	ConCommandX* imp = static_cast<ConCommandX*>(
		VFunc<void* (*)(void*, const char*)>(cvar, VT_Cvar_FindBase)(cvar, "impulse"));
	if (imp && VFunc<bool (*)(void*)>(imp, VT_CCB_IsCommand)(imp) && imp->name && !strcmp(imp->name, "impulse")
			&& imp->callback && (imp->bits & 0x06) == 0x02)
		g_impulseFn = reinterpret_cast<void (*)(const CCommandX&)>(imp->callback);
	else
		CoreLog("bhoptools: no usable impulse command, sm_save and sm_tele go to the server as usual");

	// The client registers menuselect itself, so take over its callback rather than the name.
	ConCommandX* ms = static_cast<ConCommandX*>(
		VFunc<void* (*)(void*, const char*)>(cvar, VT_Cvar_FindBase)(cvar, "menuselect"));
	if (g_impulseFn && ms && VFunc<bool (*)(void*)>(ms, VT_CCB_IsCommand)(ms) && ms->name && !strcmp(ms->name, "menuselect")
			&& ms->callback && (ms->bits & 0x06) == 0x02) {
		g_menuSelect = ms;
		g_menuSelectFn = reinterpret_cast<void (*)(const CCommandX&)>(ms->callback);
		ms->callback = reinterpret_cast<void*>(&Cmd_MenuSelect);
	}

	const struct { const char* name; void (*fn)(const CCommandX&); } says[2] = {
		{ "say", Cmd_Say }, { "say_team", Cmd_SayTeam },
	};
	for (int i = 0; i < 2; ++i) {
		ConCommandX* have = static_cast<ConCommandX*>(
			VFunc<void* (*)(void*, const char*)>(cvar, VT_Cvar_FindBase)(cvar, says[i].name));
		if (have) {
			if (!VFunc<bool (*)(void*)>(have, VT_CCB_IsCommand)(have) || !have->callback || (have->bits & 0x06) != 0x02) continue;
			g_say[i] = have;
			g_sayFn[i] = reinterpret_cast<void (*)(const CCommandX&)>(have->callback);
			have->callback = reinterpret_cast<void*>(says[i].fn);
			continue;
		}
		ConCommandX& c = g_sayCmds[i];
		c.vtable = vtable;
		c.name = says[i].name;
		c.help = "Chat; SourceMod triggers for predicted commands run here.";
		c.flags = FCVAR_HIDDEN;
		c.callback = reinterpret_cast<void*>(says[i].fn);
		c.bits = 0x02;  // new-style callback taking const CCommand&
		VFunc<void (*)(void*, void*)>(cvar, VT_Cvar_Register)(cvar, &c);
	}
	return true;
}

// Unlike the tickbase, the command number survives clock correction across prediction replays.
int CommandKey(void* player, void* mv)
{
	if (g_cmdKeys) {
		const char* cmd = Field<const char*>(player, g_off.currentCommand);
		if (cmd && (uint8_t)cmd[UC_Impulse] == (uint8_t)Field<int>(mv, MV_Impulse)
				&& memcmp(cmd + UC_ViewAngles, static_cast<char*>(mv) + MV_ViewAngles, sizeof(Vec)) == 0)
			return *reinterpret_cast<const int*>(cmd + UC_CommandNumber);
		// Only possible if the offset is wrong, so switch to tickbase keys for good.
		g_cmdKeys = false;
		g_slotCount = g_shiftCount = g_undoCount = 0;
		g_turn.left = 0;
		CoreLog("bhoptools: m_pCurrentCommand check failed, keying saves by tickbase");
	}
	return Field<int>(player, g_off.tickbase);
}

// Off drops the trigger flag, so the server plugin compensates again. The rest stay: raw impulse binds are still predicted, and the
// server treats them as predicted after any hello.
// "clientside hud fixes" off leaves bgs-jumpstats' HUD to the server.
int HelloFlags()
{
	int flags = g_on ? g_clientFlags : g_clientFlags & ~kClientTriggers;
	if (!settings::Get().speedo) flags &= ~kClientJumpstats;
	return flags;
}

void MaybeHello()
{
	const double now = MonotonicSeconds();
	if (!g_session) ResetSession();
	if (g_caps && g_syncWanted && now - g_syncAsked >= kHelloInterval) {
		g_syncAsked = now;
		char cmd[48];
		snprintf(cmd, sizeof(cmd), "bhoptools_resync %d", g_session);
		VFunc<void (*)(void*, const char*, bool)>(g_engine, VT_IVEngineClient_ServerCmd)(g_engine, cmd, true);
	}
	if (g_caps && HelloFlags() != g_helloSent) g_helloWanted = true;
	if (!g_helloWanted && g_caps) return;
	const double interval = g_caps || g_helloTries < kHelloFastTries ? kHelloInterval : kHelloSlowInterval;
	if (now - g_lastHello < interval) return;
	g_lastHello = now;
	g_helloTries++;
	g_helloWanted = false;
	g_helloSent = HelloFlags();
	if (!(g_helloSent & kClientJumpstats)) js::Reset(g_session);
	char cmd[64];
	snprintf(cmd, sizeof(cmd), "bhoptools_hello %d %d %d", g_session, kProto, g_helloSent);
	// Until caps arrive it's retried, and only one reliable message is in flight at a time: reliable, it
	// would hold up the next checkpoint or noclip command sent as text for a round trip. After that it
	// only carries a flag change, which goes once, so it has to be reliable.
	VFunc<void (*)(void*, const char*, bool)>(g_engine, VT_IVEngineClient_ServerCmd)(g_engine, cmd, g_caps != 0);
}

// Same rule as the server: shift while adding d lands a command closer to the previous one.
void DecideShift(int key, float yaw)
{
	if (g_turn.left <= 0 || key <= g_turn.key) return;
	if (fabsf(NormalizeAngle(yaw + g_turn.d - g_turn.eff)) >= fabsf(NormalizeAngle(yaw - g_turn.eff))) {
		g_turn.left = 0;
		return;
	}
	g_turn.eff = NormalizeAngle(yaw + g_turn.d);
	g_turn.left--;
	if (g_shiftCount == kMaxSlots) {
		memmove(&g_shifts[0], &g_shifts[1], sizeof(Shift) * (kMaxSlots - 1));
		g_shiftCount--;
	}
	g_shifts[g_shiftCount++] = { key, g_turn.eff, g_turn.key };
}

void ApplyShift(void* mv, int key)
{
	for (int i = 0; i < g_shiftCount; ++i) {
		if (g_shifts[i].key != key) continue;
		Field<Vec>(mv, MV_ViewAngles).y    = g_shifts[i].yaw;
		Field<Vec>(mv, MV_AbsViewAngles).y = g_shifts[i].yaw;
		Field<Vec>(mv, MV_Angles).y        = g_shifts[i].yaw;
		return;
	}
}

void Save(void* player, void* mv, int key)
{
	Slot* s = AddSlot(key);
	// Replays of the save command must not overwrite what the server confirmed.
	if (s->state == kConfirmed) return;
	s->state    = kPredicted;
	s->kz       = g_lastKz;
	s->origin   = Field<Vec>(mv, MV_AbsOrigin);
	s->velocity = Field<Vec>(mv, MV_Velocity);
	s->angles   = Field<Vec>(mv, MV_ViewAngles);
	s->flags    = Field<int>(player, g_off.flags);
	uint8_t mt  = Field<uint8_t>(player, g_off.moveType);
	// shavit saves noclip and none as walk.
	if (mt == MOVETYPE_NOCLIP || mt == MOVETYPE_NONE) mt = MOVETYPE_WALK;
	s->movetype = mt;
	s->ducked   = Field<bool>(player, g_off.ducked);
	s->ducking  = Field<bool>(player, g_off.ducking);
	s->ducktime = Field<float>(player, g_off.ducktime);
	s->stamina  = Field<float>(player, g_off.stamina);
	s->gravity  = Field<float>(player, g_off.gravity);
	s->selected = false;
	s->index    = CountWithPending(key);
	Remember(*s);
}

// Both, as SetAbsVelocity does: the local one alone can go stale.
void SetVelocity(void* player, const Vec& vel)
{
	Field<Vec>(player, g_off.velocity)    = vel;
	Field<Vec>(player, g_off.absVelocity) = vel;
}

// Mirrors shavit's LoadCheckpointCache plus the plugin's TeleportEntity(NULL, angles, velocity).
void Restore(void* player, void* mv, const Slot& s, int restore)
{
	const Vec vel = s.kz ? Vec{ 0, 0, 0 } : (restore & kRestoreVelocity) ? s.velocity : Field<Vec>(mv, MV_Velocity);
	Field<Vec>(mv, MV_AbsOrigin)     = s.origin;
	Field<Vec>(mv, MV_Velocity)      = vel;
	if (restore & kRestoreAngles) {
		Field<Vec>(mv, MV_ViewAngles)    = s.angles;
		Field<Vec>(mv, MV_AbsViewAngles) = s.angles;
		Field<Vec>(mv, MV_Angles)        = s.angles;
	}
	// The server teleports before SetupMove, which sets m_bGameCodeMovedPlayer so PlayerMove
	// recategorizes at the checkpoint. Ours ran at the old origin, so set it here.
	Field<uint8_t>(mv, MV_Flags) |= 2;

	int& flags = Field<int>(player, g_off.flags);
	flags = (flags & ~kRestoreFlags) | (s.flags & kRestoreFlags);
	Field<uint8_t>(player, g_off.moveType) = s.movetype;
	Field<bool>(player, g_off.ducked)      = s.ducked;
	Field<bool>(player, g_off.ducking)     = s.ducking;
	Field<float>(player, g_off.ducktime)   = s.ducktime;
	Field<float>(player, g_off.stamina)    = s.stamina;
	// kzcheckpoints returns before SetEntityGravity.
	if (!s.kz) Field<float>(player, g_off.gravity) = s.gravity;
	// TeleportEntity with a velocity zeroes the base velocity too.
	Field<Vec>(player, g_off.baseVelocity) = Vec{ 0, 0, 0 };
	SetVelocity(player, vel);
}

void ApplyHold(void* player, void* mv, int key, bool firstRun, float forward, float side)
{
	Hold& h = g_hold;
	if (!h.released && firstRun && (forward != 0.0f || side != 0.0f || !g_holdArmed)) {
		h.released = true;
		h.releaseKey = key;
	}
	if (h.released && key > h.releaseKey) return;
	Slot s = h.slot;
	if (!h.released || key < h.releaseKey) {
		s.velocity = Vec{ 0, 0, 0 };
		s.movetype = MOVETYPE_NONE;
	}
	Restore(player, mv, s, kRestoreVelocity);
}

void PushUndo(const Undo& u)
{
	if (g_undoCount == 8) { memmove(&g_undo[0], &g_undo[1], sizeof(Undo) * 7); g_undoCount--; }
	g_undo[g_undoCount++] = u;
}

void TurnView(const Vec& to, const Vec& cmd, int key)
{
	Vec view;
	VFunc<void (*)(void*, Vec*)>(g_engine, VT_IVEngineClient_GetViewAngles)(g_engine, &view);
	const float dPitch = to.x - cmd.x;
	const float dYaw = NormalizeAngle(to.y - cmd.y);
	view.x = ClampPitch(view.x + dPitch);
	view.y = NormalizeAngle(view.y + dYaw);
	view.z = 0.0f;
	SetView(view);

	PushUndo({ key, dPitch, dYaw, to, true });
}

// What the teleport on this command restores: g_restore when the command first runs, the same on replays.
int TeleRestoreFor(int key)
{
	for (int i = 0; i < g_teleRestoreCount; ++i)
		if (g_teleRestore[i].key == key) return g_teleRestore[i].restore;
	if (g_teleRestoreCount == 8) {
		memmove(&g_teleRestore[0], &g_teleRestore[1], sizeof(TeleRestore) * 7);
		g_teleRestoreCount--;
	}
	g_teleRestore[g_teleRestoreCount++] = { key, g_restore };
	return g_restore;
}

// shavit-zones' TeleportEntity(destination, its angles or none, zero velocity), shavit-misc's stamina and
// movetype, and StartTimer's gravity and laggedmovement: the server runs sm_r in OnPlayerRunCmd, before the move.
bool Restart(void* player, void* mv, int key, bool firstRun, bool observing)
{
	const bool server = g_caps && (g_serverFlags & kServerRestart) && g_cmdKeys;
	RsRecord* r = g_restart.Decide(key, server && !observing);
	const Vec cmd = Field<Vec>(mv, MV_ViewAngles);
	if (r->apply) {
		const RsDest& d = r->dest;
		Vec& origin = Field<Vec>(mv, MV_AbsOrigin);
		// The server's teleport only counts as moved by game code if the origin changes.
		if (origin.x != d.origin[0] || origin.y != d.origin[1] || origin.z != d.origin[2]) Field<uint8_t>(mv, MV_Flags) |= 2;
		origin = Vec{ d.origin[0], d.origin[1], d.origin[2] };
		Field<Vec>(mv, MV_Velocity) = Vec{ 0, 0, 0 };
		SetVelocity(player, Vec{ 0, 0, 0 });
		Field<Vec>(player, g_off.baseVelocity) = Vec{ 0, 0, 0 };
		// The server moves with the destination's angles on this command too.
		if (d.flags & RS_ANGLES) {
			Vec* angles[] = { &Field<Vec>(mv, MV_ViewAngles), &Field<Vec>(mv, MV_AbsViewAngles), &Field<Vec>(mv, MV_Angles) };
			for (Vec* a : angles) {
				a->x = d.pitch;
				a->y = d.yaw;
			}
		}
		if (d.flags & RS_START) {
			Field<float>(player, g_off.gravity) = d.gravity;
			if (float* lagged = TeleportPredict_LaggedMovement(player)) {
				if (r->oldLagged < 0.0f) r->oldLagged = *lagged;
				*lagged = d.lagged;
			}
		}
		if (d.flags & RS_STAMINA) Field<float>(player, g_off.stamina) = 0.0f;
		if (d.flags & RS_WALK) Field<uint8_t>(player, g_off.moveType) = MOVETYPE_WALK;
		// No checkpoint to load: touch links and the player's queued events end here.
		TeleportPredict_OnCheckpointTeleport();
	}
	if (!firstRun || observing || !server) return r->apply;
	// The ack turns the view to wherever the server went, or back.
	if (r->apply && (r->dest.flags & RS_ANGLES)) TurnView(Vec{ r->dest.pitch, r->dest.yaw, 0.0f }, cmd, key);
	else PushUndo({ key, 0.0f, 0.0f, cmd, false });
	return r->apply;
}

void Select(int key, int dir)
{
	const Slot* cur = SlotBefore(key);
	if (!cur || cur->index <= 0) return;
	const int target = cur->index + dir;
	if (target < 1 || target > CountWithPending(key) || target >= (int)g_cps.size() || !g_cps[target].index) return;
	Slot* s = AddSlot(key);
	*s = g_cps[target];
	s->key = key;
	s->state = kPredicted;
	s->selected = true;
	RedrawMenu();
}

// `zeroNow`: this command, if it teleported before the plugin runs.
void Autosync(void* player, void* mv, int key, bool firstRun, int zeroNow)
{
	const int flags = Field<int>(player, g_off.flags);
	const AsPlayer p = { (flags & FL_ONGROUND) != 0, Field<uint8_t>(player, g_off.moveType), TeleportPredict_WaterLevel(player) };
	const Vec view = Field<Vec>(mv, MV_ViewAngles);
	AsMove m = { { view.x, view.y, view.z }, Field<float>(mv, MV_SideMove), Field<int>(mv, MV_Buttons) };
	const Vec& origin = Field<Vec>(mv, MV_AbsOrigin);
	// IsSurfing: the player's hull 64 units down, anything hit with a normal z within +-0.7. Other
	// players are left out, as the client can't know where the server has them.
	auto surfing = [&]() {
		RngTraceResult tr;
		if (!TeleportPredict_TraceDown(&origin.x, 64.0f, (flags & FL_DUCKING) != 0, p.onGround, firstRun, tr)) return false;
		const bool hit = tr.fraction < 1.0f || tr.allSolid || tr.startSolid;
		return hit && tr.normal[2] >= -0.7f && tr.normal[2] <= 0.7f;
	};
	if (!g_autosync.Apply(key, m, p, zeroNow, surfing)) return;
	for (Vec* a : { &Field<Vec>(mv, MV_ViewAngles), &Field<Vec>(mv, MV_AbsViewAngles), &Field<Vec>(mv, MV_Angles) })
		*a = Vec{ m.angles[0], m.angles[1], m.angles[2] };
	Field<float>(mv, MV_SideMove) = m.side;
	Field<int>(mv, MV_Buttons)    = m.buttons;
}

void Hooked_ProcessMovement(void* self, void* player, void* mv)
{
#ifdef _WIN32
	// Not in Attach, which runs on the injected thread: the game thread walks the
	// command list while it runs console commands.
	if (!g_registered) {
		g_registered = true;
		if (!RegisterCommands())
			CoreLog("bhoptools: couldn't register console commands; running without server acks");
	}
#endif

	if (!player || !mv || g_disabled) { oProcessMovement(self, player, mv); return; }

	// One look at a live player catches a layout change; there's none before spawning.
	if (!g_checked) {
		if (Field<uint8_t>(player, g_off.lifeState) != 0 || Field<uint8_t>(player, g_off.moveType) == MOVETYPE_OBSERVER
				|| Field<int>(player, g_off.tickbase) <= 0) {
			oProcessMovement(self, player, mv);
			return;
		}
		g_checked = true;
		const int flags = Field<int>(player, g_off.flags);
		const int impulse = Field<int>(mv, MV_Impulse);
		const float pitch = Field<Vec>(mv, MV_ViewAngles).x;
		if (!(flags & FL_CLIENT) || (flags & FL_FAKECLIENT) || impulse < 0 || impulse > 255
				|| !(pitch >= -90.0f && pitch <= 90.0f) || Field<int>(player, g_off.tickbase) <= 0) {
			g_disabled = true;
			CoreLog("bhoptools: player layout looks wrong (flags 0x%x impulse %d), disabled", flags, impulse);
			oProcessMovement(self, player, mv);
			return;
		}
	}

	// Off, the client sends no impulses itself; raw impulse binds are still predicted below.
	if (tp::Enabled() != g_on) {
		g_on = !g_on;
		g_helloWanted = true;
		g_lastHello = 0.0;
	}

	const int impulse = Field<int>(mv, MV_Impulse);
	const bool firstRun = (Field<uint8_t>(mv, MV_Flags) & 1) != 0;
	const int key = CommandKey(player, mv);
	const float rawForward = Field<float>(mv, MV_ForwardMove), rawSide = Field<float>(mv, MV_SideMove);
	if (firstRun) MaybeHello();
	if (g_laggedFix.pending) {
		g_laggedFix.pending = false;
		float* lagged = TeleportPredict_LaggedMovement(player);
		if (lagged && *lagged == g_laggedFix.from) *lagged = g_laggedFix.to;
	}
	// A replay's m_bGameCodeMovedPlayer compares against our newest prediction, not the server's
	// move.
	if (!firstRun && g_on) Field<uint8_t>(mv, MV_Flags) &= ~2;

	// Dead or spectating: shavit saves the spectated player's state and refuses teleports.
	const bool observing = Field<uint8_t>(player, g_off.lifeState) != 0
		|| Field<uint8_t>(player, g_off.moveType) == MOVETYPE_OBSERVER;
	if (firstRun && !observing) g_aliveMoveAt = MonotonicSeconds();

	// shavit-syncstyle runs where the server says it does among these plugins.
	const bool autosync = g_caps && (g_serverFlags & kServerAutosync) && g_on && g_cmdKeys && !observing;
	const int syncOrder = autosync ? g_autosync.OrderFor(key) : -1;
	if (syncOrder == AS_BEFORE_CORE) Autosync(player, mv, key, firstRun, INT_MIN);

	// shavit-core's OnPlayerRunCmd is taken to run before bhoptools' (load order), so it sees the
	// player as it was before a teleport or noclip impulse. Its zone work doesn't touch what key
	// blocking reads, so it goes first.
	int inStart = -1, freestyle = -1;
	if (g_caps && (g_serverFlags & kServerZones) && g_on && !observing) {
		Vec& vel = Field<Vec>(mv, MV_Velocity);
		ZsCmd z = { { vel.x, vel.y, vel.z }, Field<int>(mv, MV_Buttons), Field<float>(mv, MV_UpMove),
			(Field<int>(player, g_off.flags) & FL_ONGROUND) != 0, Field<uint8_t>(player, g_off.moveType) == MOVETYPE_NOCLIP, false };
		if (TeleportPredict_ZonesPreCmd(player, g_cmdKeys ? key : -1, z, &inStart, &freestyle)) {
			if (z.velocitySet) {
				vel = Vec{ z.velocity[0], z.velocity[1], z.velocity[2] };
				SetVelocity(player, vel);
				Field<Vec>(player, g_off.baseVelocity) = Vec{ 0, 0, 0 };
			}
			Field<int>(mv, MV_Buttons) = z.buttons;
			Field<float>(mv, MV_UpMove) = z.upmove;
		}
	}

	if (g_caps && (g_serverFlags & kServerKeys) && g_on && g_cmdKeys && !observing) {
		SkMove m = { Field<float>(mv, MV_ForwardMove), Field<float>(mv, MV_SideMove), Field<int>(mv, MV_Buttons) };
		const int zoneMask = (inStart >= 0 ? SK_INSTART : 0) | (freestyle >= 0 ? SK_FREESTYLE : 0);
		const int zoneBits = (inStart > 0 ? SK_INSTART : 0) | (freestyle > 0 ? SK_FREESTYLE : 0);
		g_keys.Apply(key, m, (Field<int>(player, g_off.flags) & FL_ONGROUND) != 0, Field<uint8_t>(player, g_off.moveType), zoneMask, zoneBits);
		Field<float>(mv, MV_ForwardMove) = m.forward;
		Field<float>(mv, MV_SideMove)    = m.side;
		Field<int>(mv, MV_Buttons)       = m.buttons;
	}

	if (syncOrder == AS_BEFORE_BHOPTOOLS) Autosync(player, mv, key, firstRun, INT_MIN);

	bool impulseTeleport = false;
	if (g_caps && impulse == IMPULSE_TELE) {
		if (firstRun) g_turn.left = 0;
		const Slot* s = observing ? nullptr : SlotBefore(key);
		const Vec cmd = Field<Vec>(mv, MV_ViewAngles);
		const int restore = TeleRestoreFor(key);
		const bool turn = (restore & kRestoreAngles) != 0;
		if (s && s->state != kUnknown) {
			Restore(player, mv, *s, restore);
			impulseTeleport = true;
			if (g_on && (g_serverFlags & kServerHold) && g_holdArmed && !(g_hold.active && g_hold.teleKey == key)) {
				g_hold = Hold();
				g_hold.active = true;
				g_hold.teleKey = key;
				g_hold.slot = *s;
				g_hold.slot.velocity = Field<Vec>(mv, MV_Velocity);
			}
			TeleportPredict_OnCheckpointTeleport();
			TeleportPredict_OnCheckpointLoad(s->id, s->kz);
			if (firstRun) {
				js::OnTeleport();
				// The server only shifts old frames after a teleport that turned the view.
				if (turn) {
					g_turn = { key, NormalizeAngle(s->angles.y - cmd.y), s->angles.y, kOldFrameWindow };
					TurnView(s->angles, cmd, key);
				} else {
					PushUndo({ key, 0.0f, 0.0f, cmd, false });
				}
			}
		} else if (firstRun) {
			// The server may still have a checkpoint for this command.
			PushUndo({ key, 0.0f, 0.0f, cmd, turn });
		}
	} else {
		// Saves too: the server's ShiftOldFrame runs on every command but a teleport.
		if (firstRun) DecideShift(key, Field<Vec>(mv, MV_ViewAngles).y);
		ApplyShift(mv, key);
	}

	if (observing) g_hold.active = false;
	else if (g_hold.active && g_on && g_caps && key > g_hold.teleKey) ApplyHold(player, mv, key, firstRun, rawForward, rawSide);

	// shavit sets the movetype in OnPlayerRunCmd, before the move, so this command already
	// moves with it. It refuses while dead or spectating.
	if (NcIsImpulse(impulse) && !observing) {
		uint8_t& mt = Field<uint8_t>(player, g_off.moveType);
		mt = (uint8_t)NcPredict(g_noclip, key, impulse, mt, g_caps && (g_serverFlags & kServerNoclip), g_noclipAllow);
	}

	if (impulse == RS_IMPULSE_RESTART && Restart(player, mv, key, firstRun, observing)) impulseTeleport = true;

	if (syncOrder == AS_AFTER_BHOPTOOLS) Autosync(player, mv, key, firstRun, impulseTeleport ? key : INT_MIN);

	// bhop-get-stats reads the command after every plugin's OnPlayerRunCmd changed it.
	const bool stats = firstRun && !observing;
	if (stats) {
		const uint8_t mt = Field<uint8_t>(player, g_off.moveType);
		float* lagged = TeleportPredict_LaggedMovement(player);
		js::Move m = {};
		m.cmd = key;
		m.buttons = Field<int>(mv, MV_Buttons);
		m.forward = Field<float>(mv, MV_ForwardMove);
		m.side = Field<float>(mv, MV_SideMove);
		memcpy(m.angles, static_cast<char*>(mv) + MV_ViewAngles, sizeof(m.angles));
		memcpy(m.velocity, static_cast<char*>(mv) + MV_Velocity, sizeof(m.velocity));
		memcpy(m.origin, static_cast<char*>(mv) + MV_AbsOrigin, sizeof(m.origin));
		m.onGround = (Field<int>(player, g_off.flags) & FL_ONGROUND) != 0;
		m.noStats = mt == MOVETYPE_NONE || mt == MOVETYPE_NOCLIP || mt == MOVETYPE_LADDER || TeleportPredict_WaterLevel(player) >= 2;
		m.maxSpeed = Field<float>(mv, MV_MaxSpeed);
		m.lagged = lagged ? *lagged : 1.0f;
		js::PreMove(m);
	}

	tp::SetRunningCommand(key, g_cmdKeys ? key : -1);
	oProcessMovement(self, player, mv);
	tp::SetRunningCommand(INT_MIN, -1);
	if (stats) js::PostMove(&Field<Vec>(mv, MV_AbsOrigin).x, &Field<Vec>(mv, MV_Velocity).x);

	if (autosync && (TeleportPredict_MoveTeleported(!g_autosyncMovesOnly) || (impulseTeleport && syncOrder != AS_AFTER_BHOPTOOLS)))
		g_autosync.NoteTeleport(key);

	if (g_caps && (impulse == IMPULSE_PREV || impulse == IMPULSE_NEXT) && firstRun && !observing)
		Select(key, impulse == IMPULSE_NEXT ? 1 : -1);

	// shavit-zones saves where the move left us, in the air only the view, until the server reports it.
	if (impulse == RS_IMPULSE_SETSTART && !observing && g_caps && (g_serverFlags & kServerRestart) && g_cmdKeys) {
		const Vec& origin = Field<Vec>(mv, MV_AbsOrigin);
		const Vec& view = Field<Vec>(mv, MV_ViewAngles);
		const bool ground = (Field<int>(player, g_off.flags) & FL_ONGROUND) != 0;
		g_restart.SetStart(key, ground, &origin.x, view.x, view.y);
	}

	// After the move: shavit saves in OnPlayerRunCmdPost, after touches.
	if (g_caps && impulse == IMPULSE_SAVE) {
		// The first command after a menu key 1 carries its impulse, and shavit's menu saves quietly.
		const bool menuSave = firstRun && g_menuSaveAt >= 0 && MonotonicSeconds() - g_menuSaveAt < kMenuWindow;
		if (firstRun) g_menuSaveAt = -1;
		// kzcheckpoints styles refuse saves off the ground (ladders depend on a style setting).
		if (observing || (g_lastKz && !(Field<int>(player, g_off.flags) & FL_ONGROUND))) {
			Slot* s = AddSlot(key);
			if (s->state != kConfirmed) s->state = kUnknown;
		} else {
			Save(player, mv, key);
			TeleportPredict_OnCheckpointSave(FindSlot(key)->id);
			if (firstRun) {
				if (!menuSave) PrintSaved(key);
				RedrawMenu();
			}
		}
	}
}

}  // namespace

bool ResolveOffsets()
{
	using namespace dm;
	FieldReq req[] = {
		{ "m_nTickBase",       FIELD_INTEGER,   4,  {}, 0, -1 },
		{ "m_fFlags",          FIELD_INTEGER,   4,  {}, 0, -1 },
		{ "m_flStamina",       FIELD_FLOAT,     4,  {}, 0, -1 },
		{ "m_Local",           FIELD_EMBEDDED,  -1, {}, 0, -1 },
		{ "m_bDucked",         FIELD_BOOLEAN,   1,  {}, 0, -1 },
		{ "m_bDucking",        FIELD_BOOLEAN,   1,  {}, 0, -1 },
		{ "m_flDucktime",      FIELD_FLOAT,     4,  {}, 0, -1 },
		{ "m_vecBaseVelocity", FIELD_VECTOR,    12, {}, 0, -1 },
		{ "m_vecVelocity",     FIELD_VECTOR,    12, {}, 0, -1 },
		{ "m_lifeState",       FIELD_CHARACTER, 1,  {}, 0, -1 },
		{ "m_MoveType",        FIELD_CHARACTER, 1,  {}, 0, -1 },
		{ "m_flGravity",       FIELD_FLOAT,     4,  {}, 0, -1 },
		{ "m_vecAbsVelocity",  FIELD_VECTOR,    12, {}, 0, -1 },
	};
	const int n = sizeof(req) / sizeof(req[0]);

	const std::vector<MemRange> segs = ModuleDataRanges(CLIENT_MODULE);
	if (segs.empty()) return false;
	ResolveFields(segs, req, n);

	for (int i = 0; i < n; ++i) {
		if (req[i].offset < 0) {
			g_offsetError = req[i].name;
			return false;
		}
	}

	// m_bDucked and friends are CPlayerLocalData members, relative to m_Local.
	const int local = req[3].offset;
	g_off.tickbase     = req[0].offset;
	g_off.flags        = req[1].offset;
	g_off.stamina      = req[2].offset;
	g_off.ducked       = local + req[4].offset;
	g_off.ducking      = local + req[5].offset;
	g_off.ducktime     = local + req[6].offset;
	g_off.baseVelocity = req[7].offset;
	g_off.velocity     = req[8].offset;
	g_off.lifeState    = req[9].offset;
	g_off.moveType     = req[10].offset;
	g_off.gravity      = req[11].offset;
	g_off.absVelocity  = req[12].offset;

	const uintptr_t store = FindPatternIn(CLIENT_MODULE, SIG_StartCommandStore);
	g_off.currentCommand = store ? *reinterpret_cast<const int*>(store + OFF_StartCommandStore_Disp) : 0;
	if (g_off.currentCommand <= 0 || g_off.currentCommand > 0x8000) {
		g_off.currentCommand = 0;
		g_cmdKeys = false;
	}

	g_offsetsOk = true;
	return true;
}

// On Linux ConMsg may not be safe off the main thread, so offset errors are logged here.
bool Attach()
{
	if (!g_offsetsOk) {
		CoreLog("bhoptools: %s not found in " CLIENT_MODULE "'s prediction tables", g_offsetError ? g_offsetError : CLIENT_MODULE);
		return false;
	}
	void* movement = CreateInterfaceIn(CLIENT_MODULE, "GameMovement001");
	g_engine = CreateInterfaceIn(ENGINE_MODULE, "VEngineClient014");
	if (!movement || !g_engine) return false;
	g_engineSound = CreateInterfaceIn(ENGINE_MODULE, "IEngineSoundClient003");

	g_client = CreateInterfaceIn(CLIENT_MODULE, "VClient017");
	g_dispatch = speedo::OriginalDispatch();
	speedo::Watch("ShowMenu", OnShowMenu);

#ifndef _WIN32
	if (!RegisterCommands())
		CoreLog("bhoptools: couldn't register console commands; running without server acks");
#endif

	g_slot = &(*reinterpret_cast<void***>(movement))[VT_IGameMovement_ProcessMovement];
	oProcessMovement = reinterpret_cast<ProcessMovementFn>(*g_slot);
	if (PatchPointer(g_slot, reinterpret_cast<void*>(&Hooked_ProcessMovement))) return true;
	g_slot = nullptr;
	return false;
}

#ifdef _WIN32
void Detach()
{
	if (g_slot) PatchPointer(g_slot, reinterpret_cast<void*>(oProcessMovement));
	g_slot = nullptr;
	if (g_menuSelect) g_menuSelect->callback = reinterpret_cast<void*>(g_menuSelectFn);
	g_menuSelect = nullptr;
	for (int i = 0; i < 2; ++i) {
		if (g_say[i]) g_say[i]->callback = reinterpret_cast<void*>(g_sayFn[i]);
		g_say[i] = nullptr;
		g_sayFn[i] = nullptr;
	}
	// The commands stay registered; this sends sm_save and friends to the server again.
	g_disabled = true;
}
#endif

void OnLevelInit()
{
	ResetSession();
}

void SetTriggerPrediction(bool on)
{
	const int flags = (g_clientFlags & ~kClientTriggers) | (on ? kClientTriggers : 0);
	if (flags == g_clientFlags) return;
	g_clientFlags = flags;
	g_helloWanted = true;
	g_lastHello = 0.0;
}

void PrintState()
{
	if (!g_slot || g_disabled) { CoreLog("checkpoints: off (unavailable)"); return; }
	if (!g_caps) { CoreLog("checkpoints: off (server has no bhoptools plugin)"); return; }
	if (!tp::Enabled()) { CoreLog("checkpoints: off (impulse binds still predicted)"); return; }
	static const char* const kLandfix[] = { "off", "haze", "olivia" };
	const MoveSettings& m = TeleportPredict_MoveSettings();
	char runspeed[16] = "stock";
	if (m.runspeed > 0.0f) snprintf(runspeed, sizeof(runspeed), "%.2f", m.runspeed);
	char momsurffix[128];
	msf::Describe(momsurffix, sizeof(momsurffix));
	CoreLog("checkpoints: on (proto %d%s%s%s%s%s%s%s%s%s%s), runspeed %s, landfix %s, easybhop %d, momsurffix %s, teleports restore %d", kProto,
		g_serverFlags & kServerPrevNext ? ", prev/next" : "",
		g_serverFlags & kServerNoclip ? ", noclip" : "",
		g_serverFlags & kServerRestart ? ", restart" : "",
		g_serverFlags & kServerAutobhop ? ", autobhop" : "",
		g_serverFlags & kServerMpbhops ? ", mpbhops" : "",
		g_serverFlags & kServerState ? ", state" : "",
		g_serverFlags & kServerCpset ? ", cpset" : "",
		g_serverFlags & kServerKeys ? ", keys" : "",
		g_serverFlags & kServerAutosync ? ", autosync" : "",
		g_menuSelect ? ", menu" : "",
		runspeed, kLandfix[m.landfix], m.easybhop ? 1 : 0, momsurffix, g_restore);
}

void AddInput(float dx, float dy)
{
	g_inputX += lroundf(dx);
	g_inputY += lroundf(dy);
}

void OnCreateMove(int sequence)
{
	if (g_frameCount && sequence != g_frameFirst + g_frameCount) FlushFrames();
	if (!g_frameCount) g_frameFirst = sequence;
	g_frameX[g_frameCount] = (int32_t)(uint32_t)g_inputX;
	g_frameY[g_frameCount] = (int32_t)(uint32_t)g_inputY;
	if (++g_frameCount == kFrameBatch) FlushFrames();
}

}  // namespace cp
