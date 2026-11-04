#include "teleport_predict.h"
#include "prediction.h"
#include "mapdiff.h"
// Before Windows.h, whose wingdi.h has a PC_NONE macro.
#include "rngfix_predict.h"
#include "utils.h"
#include "datamap.h"
#include "sigs.h"
#include "published.h"
#include "game_layout.h"
#include "speedo.h"
#include "engine_trace.h"
#include "fastdl.h"
#include "server_movement.h"
#include "settings.h"
#include "momsurffix_predict.h"
#include "jumpstats.h"
#include "version.h"

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <unordered_map>
#include <vector>
#include <climits>
#include <cstdarg>
#include <ctime>

#ifdef _WIN32
#include "Detours/src/detours.h"
#endif

namespace tp {
namespace {

const int kSnapWatchCommands = 200;
// How far the server's bhoptools_tsnap command may be from the one we predicted the snap on.
const int kSnapMatchCommands = 8;

const float kAngleEpsilon = 0.05f;

struct Vector3 { float x, y, z; };

typedef void  (*ProcessMovementFn)(void* thisptr, void* pPlayer, void* pMove);
typedef void  (*ViewAnglesFn)(void*, Vector3*);

ProcessMovementFn oProcessMovement = nullptr;
ViewAnglesFn      oGetViewAngles   = nullptr;
ViewAnglesFn      oSetViewAngles   = nullptr;

void* g_pGameMovement = nullptr;
void* g_pEngine       = nullptr;
void* g_stringTables  = nullptr;
bool  g_netChannelOk  = false;

bool g_enabled = true;

PredRuntime g_rt;
RngPredict* g_rngp = nullptr;
// Created in Init: on Windows the launcher's image is copied in with its globals
// already constructed, and MSVC's unordered_map allocates in its constructor.
std::unordered_map<int, PredCheckpointState>* g_checkpoints = nullptr;
std::string g_currentLevel;
bool g_levelFailed = false;
const int kMapDiffFormat = 1;
struct {
    bool wanted = false;
    int  lumpCrc = 0;
    int  hash = 0;
    std::string text;
} g_mapDiff;
#ifndef _WIN32
std::string g_modDir;
#endif

struct GlobalVarsX {
    float realtime;
    int   framecount;
    float absoluteframetime;
    float curtime;
    float frametime;
    int   maxClients;
    int   tickcount;
    float interval_per_tick;
};
static_assert(offsetof(GlobalVarsX, interval_per_tick) == 0x1C, "CGlobalVarsBase layout");

GlobalVarsX** g_ppGlobals = nullptr;

// Measured over 1.5 s when the globals signature is gone.
float g_tickInterval  = 0.015f;
bool  g_intervalKnown = false;
int   g_intervalTick0 = 0;
unsigned long long g_intervalMs0 = 0;

bool    g_snapWatch = false;
bool    g_snapLeft  = false;
Vector3 g_snapAngle = { 0, 0, 0 };
Vector3 g_ourAngle  = { 0, 0, 0 };
int     g_snapTicks = 0;
bool    g_inOurSetCall = false;

// Snaps we predicted, for bhoptools_tsnap. cmd is -1 when the command number wasn't known.
struct PredictedSnap { bool used; int cmd; Vector3 angle; };
PredictedSnap g_predictedSnaps[8];
int g_predictedSnapNext = 0;

// The view each command was made with, so a late bhoptools_tsnap keeps the turn made since.
const int kCmdViews = 128;
struct CmdView { int cmd; Vector3 angle; };
CmdView g_cmdViews[kCmdViews];

// bhoptools_log: each correction from the server and what the command did.
FILE* g_predLog = nullptr;
const int kPredCmds = 128;
struct PredCmd {
    int key;
    Vector3 start, startVel;
    int flags;
    uint32_t ground;
    float baseVel;
    bool teleported, velTeleported, snapped;
};
PredCmd g_predCmds[kPredCmds];
int g_predLastRunKey = INT_MIN;
int g_predNewestKey = INT_MIN;
int g_predCommands = 0;
int g_predErrors = 0;

PredCmd& PredCmdFor(int key) { return g_predCmds[((key % kPredCmds) + kPredCmds) % kPredCmds]; }

void PredLog(const char* fmt, ...)
{
    if (!g_predLog) return;
    char ts[16];
    const time_t now = time(nullptr);
    strftime(ts, sizeof(ts), "%H:%M:%S", localtime(&now));
    fprintf(g_predLog, "%s ", ts);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_predLog, fmt, ap);
    va_end(ap);
    fputc('\n', g_predLog);
    fflush(g_predLog);
}

const char* EngineGetLevelName()
{
    if (!g_pEngine) return nullptr;
    typedef const char* (*Fn)(void*);
    return VFunc<Fn>(g_pEngine, VT_IVEngineClient_GetLevelName)(g_pEngine);
}

const char* EngineGetGameDirectory()
{
    if (!g_pEngine) return nullptr;
    typedef const char* (*Fn)(void*);
    return VFunc<Fn>(g_pEngine, VT_IVEngineClient_GetGameDirectory)(g_pEngine);
}

bool EngineIsInGame()
{
    if (!g_pEngine) return false;
    typedef bool (*Fn)(void*);
    return VFunc<Fn>(g_pEngine, VT_IVEngineClient_IsInGame)(g_pEngine);
}

void EngineSetViewAngles(const Vector3& ang)
{
    if (!g_pEngine || !oSetViewAngles) return;
    Vector3 a = ang;
    g_inOurSetCall = true;
    oSetViewAngles(g_pEngine, &a);
    g_inOurSetCall = false;
}

float Len(const Vector3& v) { return sqrtf(v.x * v.x + v.y * v.y + v.z * v.z); }

float AngleDiff(float a, float b)
{
    float d = a - b;
    while (d > 180.0f)  d -= 360.0f;
    while (d < -180.0f) d += 360.0f;
    return d < 0.0f ? -d : d;
}

bool AnglesClose(const Vector3& a, const Vector3& b)
{
    return AngleDiff(a.x, b.x) < kAngleEpsilon
        && AngleDiff(a.y, b.y) < kAngleEpsilon
        && AngleDiff(a.z, b.z) < kAngleEpsilon;
}

bool IsServerSnap(const Vector3& v)
{
    return g_snapWatch && g_snapLeft && AnglesClose(v, g_snapAngle);
}

struct RecvTableX;
struct RecvPropX {
    const char* m_pVarName;
    int         m_RecvType;
    int         m_Flags;
    int         m_StringBufferSize;
    bool        m_bInsideArray;
    const void* m_pExtraData;
    void*       m_pArrayProp;
    void*       m_ArrayLengthProxy;
    void*       m_ProxyFn;
    void*       m_DataTableProxyFn;
    RecvTableX* m_pDataTable;
    int         m_Offset;
    int         m_ElementStride;
    int         m_nElements;
    const char* m_pParentArrayPropName;
};
static_assert(sizeof(RecvPropX) == RecvProp_SIZE, "RecvProp layout");

const int kDPT_Int    = 0;
const int kDPT_Float  = 1;

// On Linux written by the installer thread, read by the main-thread hook.
int g_baseVelOffset = -1;
int g_velOffset     = -1;
int g_flagsOffset   = -1;
int g_gravityOffset = -1;
int g_tickBaseOffset= -1;
int g_lifeStateOffset = -1;
int g_observerOffset  = -1;
int g_moveTypeOffset  = -1;
int g_duckingOffset    = -1;
int g_stepSizeOffset   = -1;
int g_waterLevelOffset = -1;
bool g_moveTeleported;
bool g_moveVelocityTeleported;
int g_laggedOffset     = -1;
int g_groundOffset    = -1;
int g_waterJumpOffset = -1;
int g_frictionOffset  = -1;
int g_staminaOffset   = -1;
int g_viewOffsetZ     = -1;
std::atomic<bool> g_netvarsReady(false);
bool g_offsetsChecked = false;
bool g_offsetsGood    = false;

MoveSettings g_move;
// By the command they took effect on, so re-predicting an older command uses what applied then.
struct MoveChange { int fromCmd; float runspeed; int landfix; bool easybhop; };
std::vector<MoveChange> g_moveHistory;
MoveChange g_moveNow = { INT_MIN, 0.0f, LANDFIX_OFF, false };

MoveChange MoveFor(int cmd)
{
    for (size_t i = g_moveHistory.size(); i-- > 0; )
        if (cmd == INT_MIN || g_moveHistory[i].fromCmd <= cmd) return g_moveHistory[i];
    return g_moveHistory.empty() ? MoveChange{ INT_MIN, g_move.runspeed, g_move.landfix, g_move.easybhop }
                                 : g_moveHistory.front();
}
GroundLog    g_groundLog;
int  g_runningKey = INT_MIN;
int  g_runningCmdNumber = -1;
bool g_untouchEachCommand;
bool g_mpbhopsLoaded;
void (*g_printCheckpoints)();

// shavit-misc post-hooks CCSPlayer::GetPlayerMaxSpeed through the vtable, and its only virtual
// caller is ProcessMovement's m_flMaxSpeed store, so the client overrides the same slot.
typedef float (*GetPlayerMaxSpeedFn)(void*);
GetPlayerMaxSpeedFn oGetPlayerMaxSpeed = nullptr;
uintptr_t g_maxSpeedScanFrom = 0;
int       g_maxSpeedIndex    = -1;
void**    g_maxSpeedVtable   = nullptr;
void*     g_maxSpeedPlayer   = nullptr;  // only during our ProcessMovement
float     g_maxSpeedValue    = 0.0f;

// Olivia's landfix pre-hooks CheckJumpButton through the IGameMovement vtable.
typedef bool (*CheckJumpButtonFn)(void*);
CheckJumpButtonFn oCheckJumpButton = nullptr;
struct OliviaRun {
    char* pl;            // null outside a command Olivia applies to
    char* md;
    bool  ducked;        // FL_DUCKING at the start: the collision bounds the server traces with
    bool  clearOldJump;  // shavit's autobhop cleared IN_JUMP from the old buttons before the move
    bool  onGround;
    bool  firstRun;
};
OliviaRun g_olivia = {};

// The SourceJump plugin's landfix, from a server whose caps say so: a post hook on CheckJumpButton.
bool g_sjLandfix = false;
struct SjJump {
    char* pl;            // null outside a command it applies to
    char* md;
    bool  ducked;
    bool  onGround;
    bool  firstRun;
    bool  lifts;         // the style gets the lift and the jump would be a bhop or jumpbug
    int   startGround;
    bool  triggers;      // the sim's world is this command's, so the lift is kept for its booster touches
};
SjJump g_sjJump = {};

// While bhoptools_log is on: each jump's ground offset and how far the server's result was from ours.
bool g_jumpLog = false;
char* g_jumpPl = nullptr;
char* g_jumpMd = nullptr;
bool  g_jumpFirstRun = false;
struct JumpStart { int key; float z, vz; };
JumpStart g_jumpStarts[PRED_RING_SIZE];
struct { int key; float predOff, replayOff; bool ducked; } g_lastJump = { INT_MIN, 0.0f, 0.0f, false };
int g_lastRunKey = INT_MIN;
// A command whose jump may leave an mpbhops boost door; pl is null otherwise.
struct { char* pl; char* md; } g_mpbhopsJump = {};

int PropOffset(const RecvPropX* p, int wantType)
{
    if (p->m_pDataTable)           return -1;
    if (p->m_nElements != 1)       return -1;
    if (p->m_RecvType != wantType) return -1;
    if (p->m_Offset <= 0 || p->m_Offset > 0x8000) return -1;
    return p->m_Offset;
}

#ifdef _WIN32
bool ModuleRange(const char* name, uintptr_t& base, size_t& size)
{
    HMODULE h = GetModuleHandleA(name);
    if (!h) return false;

    const unsigned char* p = reinterpret_cast<const unsigned char*>(h);
    if (p[0] != 'M' || p[1] != 'Z') return false;

    const int lfanew = *reinterpret_cast<const int*>(p + 0x3C);
    if (lfanew < 0 || lfanew > 0x1000) return false;

    const unsigned char* nt = p + lfanew;
    if (nt[0] != 'P' || nt[1] != 'E' || nt[2] != 0 || nt[3] != 0) return false;

    const unsigned int sizeOfImage = *reinterpret_cast<const unsigned int*>(nt + 24 + 56);
    if (sizeOfImage < 0x1000 || sizeOfImage > 0x10000000) return false;

    base = reinterpret_cast<uintptr_t>(h);
    size = sizeOfImage;
    return true;
}

const char* FindStringIn(uintptr_t base, size_t size, const char* str)
{
    const size_t n = std::strlen(str) + 1;
    if (size < n) return nullptr;

    const char first = str[0];
    const char* start = reinterpret_cast<const char*>(base);

    for (size_t i = 0; i + n <= size; i++) {
        if (start[i] != first) continue;
        if (std::memcmp(start + i, str, n) == 0) return start + i;
    }
    return nullptr;
}

// The whole image, and only the name's first literal.
int FindNetvarOffset(const char* name, int wantType)
{
    uintptr_t base = 0; size_t size = 0;
    if (!ModuleRange(CLIENT_MODULE, base, size)) return -1;

    const char* str = FindStringIn(base, size, name);
    if (!str) return -1;

    const uintptr_t want = reinterpret_cast<uintptr_t>(str);

    for (size_t i = 0; i + sizeof(RecvPropX) <= size; i += 8) {
        const RecvPropX* p = reinterpret_cast<const RecvPropX*>(base + i);
        if (reinterpret_cast<uintptr_t>(p->m_pVarName) != want) continue;

        const int off = PropOffset(p, wantType);
        if (off > 0) return off;
    }
    return -1;
}
#else
std::vector<uintptr_t> FindStringLiterals(const char* str)
{
    std::vector<uintptr_t> out;
    const size_t n = std::strlen(str) + 1;
    for (const auto& seg : FindSegments(CLIENT_MODULE, 'r')) {
        const char* start = reinterpret_cast<const char*>(seg.start);
        const size_t size = seg.end - seg.start;
        for (size_t i = 0; i + n <= size; i++) {
            if (start[i] != str[0]) continue;
            if (std::memcmp(start + i, str, n) == 0) out.push_back(seg.start + i);
        }
    }
    return out;
}

// Every literal of the name in readable segments; the RecvProps are in writable ones.
int FindNetvarOffset(const char* name, int wantType)
{
    const std::vector<uintptr_t> literals = FindStringLiterals(name);
    if (literals.empty()) return -1;

    for (const auto& seg : FindSegments(CLIENT_MODULE, 'w')) {
        for (uintptr_t a = seg.start & ~(uintptr_t)7; a + sizeof(RecvPropX) <= seg.end; a += 8) {
            const RecvPropX* p = reinterpret_cast<const RecvPropX*>(a);
            bool named = false;
            for (uintptr_t lit : literals)
                if (reinterpret_cast<uintptr_t>(p->m_pVarName) == lit) named = true;
            if (!named) continue;

            const int off = PropOffset(p, wantType);
            if (off > 0) return off;
        }
    }
    return -1;
}
#endif

void ResetPerTeleportState()
{
    g_snapWatch = false;
    g_snapLeft  = false;
    for (PredictedSnap& p : g_predictedSnaps) p.used = false;
}

#ifndef _WIN32
// com_gamedir may be relative to a CWD that is not the game root inside the
// Steam Linux Runtime; anchor it on the resident client.so instead.
std::string ResolveModDir(const char* gameDir)
{
    if (gameDir[0] == '/') return gameDir;
    auto segs = FindExecSegments(CLIENT_MODULE);
    if (segs.empty()) return gameDir;
    std::string p = segs[0].path;
    for (int i = 0; i < 3; i++) {
        size_t slash = p.rfind('/');
        if (slash == std::string::npos) return gameDir;
        p.erase(slash);
    }
    return p;
}
#endif

// False when the entities are as they were.
bool UseMapDiff(PredMap& map)
{
    const bool want = g_mapDiff.wanted && g_mapDiff.lumpCrc == (int)map.entityCrc;
    if (want == map.diffed && (!want || map.diffHash == g_mapDiff.hash)) return false;
    const bool wasDiffed = map.diffed;
    if (map.diffed) {
        map.entities.swap(map.bspEntities);
        map.bspEntities.clear();
        map.diffed = false;
    }
    if (!want) return wasDiffed;

    std::vector<PredMapDiffEdit> edits;
    std::vector<PredMapEntity> ents;
    if (!PredParseMapDiff(g_mapDiff.text.data(), g_mapDiff.text.size(), edits)
            || !PredApplyMapDiff(map.entities, edits, ents)) {
        CoreLog("bhoptools: the server's entity diff doesn't fit this map; using the bsp's entities");
        return wasDiffed;
    }
    map.bspEntities.swap(map.entities);
    map.entities.swap(ents);
    map.diffed = true;
    map.diffHash = g_mapDiff.hash;
    CoreLog("bhoptools: %d entity changes from the server", (int)edits.size());
    return true;
}

void LoadLevel(const char* levelName)
{
    // Before PredReset frees the map the BSP tracer points into.
    // rngfix's surf_ check wants the server's map name, not the sha1 one a mismatched map is loaded under.
    RngPredictNewLevel(*g_rngp, fastdl::ServerMapName(levelName));
    PredReset(g_rt);
    g_checkpoints->clear();
    g_levelFailed = false;
    ResetPerTeleportState();

    const char* gameDir = EngineGetGameDirectory();
    if (!gameDir || !levelName) { g_levelFailed = true; return; }
#ifdef _WIN32
    const char* modDir = gameDir;
    char path[MAX_PATH];
#else
    if (g_modDir.empty()) g_modDir = ResolveModDir(gameDir);
    const char* modDir = g_modDir.c_str();
    char path[PATH_MAX];
#endif

    const char* prefixes[2] = { "", "download/" };

    for (int i = 0; i < 2; i++) {
        snprintf(path, sizeof(path), "%s/%s%s", modDir, prefixes[i], levelName);

        // World brushes too, for rngfix's BSP tracer when the engine trace isn't there.
        PredMap* map = new PredMap();
        if (!PredLoadBsp(path, *map, true, modDir)) { delete map; continue; }

        UseMapDiff(*map);
        PredBuildTriggers(*map);
        PredAdopt(g_rt, map);
        RngPredictSetBsp(*g_rngp, g_rt.map);
        return;
    }

    g_levelFailed = true;
}

void MaybeReloadLevel()
{
    if (!EngineIsInGame()) return;

    const char* lvl = EngineGetLevelName();
    if (!lvl || !*lvl) return;

    if (g_currentLevel == lvl) return;

    g_currentLevel = lvl;
    LoadLevel(lvl);
}

void Hooked_SetViewAngles(void* thisptr, Vector3* va)
{
    if (g_inOurSetCall || !va) {
        oSetViewAngles(thisptr, va);
        return;
    }

    if (IsServerSnap(*va)) {
        g_snapWatch = false;
        PredLog("server view snap dropped (set)");
        return;
    }

    g_ourAngle = *va;
    if (g_snapWatch && !AnglesClose(*va, g_snapAngle))
        g_snapLeft = true;

    oSetViewAngles(thisptr, va);
}

void Hooked_GetViewAngles(void* thisptr, Vector3* va)
{
    oGetViewAngles(thisptr, va);
    if (!va) return;

    if (IsServerSnap(*va)) {
        *va = g_ourAngle;
        EngineSetViewAngles(g_ourAngle);
        g_snapWatch = false;
        PredLog("server view snap dropped (get)");
        return;
    }

    g_ourAngle = *va;

    if (g_snapWatch && !AnglesClose(*va, g_snapAngle))
        g_snapLeft = true;
}

void UpdateTickInterval(int tick)
{
    if (g_intervalKnown) return;

    const unsigned long long now = MonotonicMs();
    if (g_intervalMs0 == 0) { g_intervalMs0 = now; g_intervalTick0 = tick; return; }

    const int ticks = tick - g_intervalTick0;
    const unsigned long long ms = now - g_intervalMs0;
    if (ticks < 100 || ms < 1500) {
        if (ticks < 0) { g_intervalMs0 = now; g_intervalTick0 = tick; }
        return;
    }

    const float measured = (float)ms / 1000.0f / (float)ticks;

    const float candidates[] = { 1.0f / 128.0f, 1.0f / 100.0f, 1.0f / 66.6667f,
                                 1.0f / 64.0f,  1.0f / 60.0f,  1.0f / 50.0f };
    float best = candidates[0];
    for (size_t i = 1; i < sizeof(candidates) / sizeof(candidates[0]); i++)
        if (fabsf(candidates[i] - measured) < fabsf(best - measured)) best = candidates[i];

    g_tickInterval  = best;
    g_intervalKnown = true;
}

GlobalVarsX** FindGlobals()
{
    const uintptr_t hit = FindPatternIn(CLIENT_MODULE, SIG_ProcessMovementGlobals);
    if (!hit) return nullptr;
    g_maxSpeedScanFrom = hit;
    int32_t disp;
    memcpy(&disp, reinterpret_cast<const void*>(hit + 3), sizeof disp);
    return reinterpret_cast<GlobalVarsX**>(hit + 7 + disp);
}

// Prediction sets curtime = m_nTickBase * the server's tick interval before
// running the command, so this is exact.
bool ReadTickInterval(int tick)
{
    const GlobalVarsX* g = g_ppGlobals ? *g_ppGlobals : nullptr;
    if (!g || !(g->interval_per_tick > 0.0f && g->interval_per_tick <= 0.1f)) return false;
    if ((float)tick * g->interval_per_tick != g->curtime) return false;
    g_tickInterval = g->interval_per_tick;
    return true;
}

// IsPlayerAlive: m_lifeState only.
bool LifeAlive(const char* pl)
{
    return g_lifeStateOffset <= 0 || *reinterpret_cast<const uint8_t*>(pl + g_lifeStateOffset) == 0;
}

bool Alive(const char* pl)
{
    return LifeAlive(pl) && (g_observerOffset <= 0 || *reinterpret_cast<const int*>(pl + g_observerOffset) == 0);
}

// ConCommandBase name +0x18; ConVar (secondary IConVar vptr at +0x30) m_pParent +0x38,
// m_fValue +0x54. The data layout is the same on both ABIs.
const int kCCB_Name      = 0x18;
const int kConVar_Parent = 0x38;
const int kConVar_Float  = 0x54;

// The engine keeps pointers to these for the whole process.
const int kCmdCount = 2;
ConCommandX g_cmds[kCmdCount];
bool g_registered = false;
bool g_lateInit = false;

// Replicated, so the client's copies hold the server's values.
const float* g_svGravity       = nullptr;
const float* g_svAirAccelerate = nullptr;
const float* g_svMaxVelocity   = nullptr;
const float* g_svBounce        = nullptr;

const float* FindCvarFloat(void* cvar, const char* name)
{
    char* v = static_cast<char*>(VFunc<void* (*)(void*, const char*)>(cvar, VT_Cvar_FindBase)(cvar, name));
    if (!v || VFunc<bool (*)(void*)>(v, VT_CCB_IsCommand)(v)) return nullptr;
    const char* n = *reinterpret_cast<const char* const*>(v + kCCB_Name);
    if (!n || strcmp(n, name) != 0) return nullptr;
    char* parent = *reinterpret_cast<char**>(v + kConVar_Parent);
    return reinterpret_cast<const float*>((parent ? parent : v) + kConVar_Float);
}

float CvarOr(const float* v, float def) { return v ? *v : def; }

const char* OffsetsProblem()
{
    return !g_netvarsReady.load(std::memory_order_acquire) ? "player fields not found"
        : !g_offsetsChecked ? "offsets unchecked, move over 50 u/s"
        : !g_offsetsGood ? "offsets failed their check" : nullptr;
}

void Cmd_Predict(const CCommandX& c)
{
    if (c.argc > 1) {
        settings::Get().predict = atoi(c.argv[1]) != 0;
        settings::Changed();
    }
    CoreLog("bhoptools_predict %d", g_enabled ? 1 : 0);
}

void PrintTriggers()
{
    const char* problem = OffsetsProblem();
    if (!g_enabled) CoreLog("triggers: off");
    else if (problem) CoreLog("triggers: off (%s)", problem);
    else if (g_levelFailed || !g_rt.map) CoreLog("triggers: off (no map data)");
    else if (g_mpbhopsLoaded) CoreLog("triggers: on, %d triggers, %d mpbhops doors", g_rt.map->stats.active, (int)g_rt.map->mpbhopsDoors.size());
    else CoreLog("triggers: on, %d triggers", g_rt.map->stats.active);
}

void PrintRngfix()
{
    const RngSettings& s = TeleportPredict_RngfixSettings();
    const char* problem = OffsetsProblem();
    if (!g_enabled) CoreLog("rngfix: off");
    else if (!s.known) CoreLog("rngfix: off (server has no rngfix, or its query didn't answer)");
    else if (problem) CoreLog("rngfix: off (%s)", problem);
    else CoreLog("rngfix: on (downhill %d uphill %d edge %d triggerjump %d telehop %d stairs %d)",
        s.downhill, s.uphill, s.edge, s.triggerjump, s.telehop, s.stairs);
}

void Cmd_Log(const CCommandX&)
{
    CoreLog("bhoptools_log: %s", TeleportPredict_ToggleLog() ? "on, writing cstrike/bhoptools_log.txt" : "off");
}

// Game thread only: the engine walks the command list while it runs console commands.
void LateInit()
{
    if (g_lateInit) return;
    g_lateInit = true;
#ifdef _WIN32
    // Not in Attach, which runs on the injected thread. Normally the window proc was first.
    RegisterCommands();
#endif

    void* cvar = CreateInterfaceIn(VSTDLIB_MODULE, "VEngineCvar004");
    if (cvar) {
        g_svGravity       = FindCvarFloat(cvar, "sv_gravity");
        g_svAirAccelerate = FindCvarFloat(cvar, "sv_airaccelerate");
        g_svMaxVelocity   = FindCvarFloat(cvar, "sv_maxvelocity");
        g_svBounce        = FindCvarFloat(cvar, "sv_bounce");
    }
    if (!g_svGravity)       CoreLog("rngfix: sv_gravity not found, taken as 800");
    if (!g_svAirAccelerate) CoreLog("rngfix: sv_airaccelerate not found, taken as 10");
    if (!g_svMaxVelocity)   CoreLog("rngfix: sv_maxvelocity not found, taken as 3500");
    if (g_duckingOffset <= 0)    CoreLog("rngfix: m_bDucking not found, taken as not ducking");
    if (g_stepSizeOffset <= 0)   CoreLog("rngfix: m_flStepSize not found, taken as 18");
    if (g_waterLevelOffset <= 0) CoreLog("rngfix: m_nWaterLevel not found, taken as dry");
    if (g_laggedOffset <= 0)     CoreLog("rngfix: m_flLaggedMovementValue not found, taken as 1");
    if (g_maxSpeedIndex <= 0)    CoreLog("runspeed: GetPlayerMaxSpeed call not found, shavit runspeed not predicted");
    if (g_groundOffset <= 0)     CoreLog("landfix: m_hGroundEntity not found, landfix not predicted");
    if (!oCheckJumpButton)       CoreLog("landfix: CheckJumpButton not found, Olivia landfix not predicted");
    if (!g_netChannelOk)         CoreLog("triggers: GetNetChannelInfo not recognised, touches end after every command");
}

RngEnv RngfixEnv(const char* pl)
{
    RngEnv e;
    e.gravity       = CvarOr(g_svGravity, e.gravity);
    e.airAccelerate = CvarOr(g_svAirAccelerate, e.airAccelerate);
    e.maxVelocity   = CvarOr(g_svMaxVelocity, e.maxVelocity);
    if (g_stepSizeOffset > 0) e.stepSize = *reinterpret_cast<const float*>(pl + g_stepSizeOffset);
    return e;
}

int WaterLevel(const char* pl)
{
    return g_waterLevelOffset > 0 ? *reinterpret_cast<const uint8_t*>(pl + g_waterLevelOffset) : 0;
}

RngMoveInput RngfixInput(const char* md, const char* pl)
{
    RngMoveInput in;
    in.buttons     = *reinterpret_cast<const int*>(md + MV_Buttons);
    in.oldButtons  = *reinterpret_cast<const int*>(md + MV_OldButtons);
    in.forwardMove = *reinterpret_cast<const float*>(md + MV_ForwardMove);
    in.sideMove    = *reinterpret_cast<const float*>(md + MV_SideMove);
    in.maxSpeed    = *reinterpret_cast<const float*>(md + MV_MaxSpeed);
    memcpy(in.viewAngles, md + MV_ViewAngles, sizeof(in.viewAngles));
    in.ducking     = g_duckingOffset > 0 && *reinterpret_cast<const uint8_t*>(pl + g_duckingOffset) != 0;
    in.alive       = LifeAlive(pl);
    in.waterLevel  = WaterLevel(pl);
    if (g_laggedOffset > 0) in.laggedMovement = *reinterpret_cast<const float*>(pl + g_laggedOffset);
    return in;
}

// A choked command's touches end with a later command's.
const int kPacketRing = 128;
int g_chokedCmd[kPacketRing];

// CEngineClient::GetNetChannelInfo returns cl.m_NetChannel: a rip-relative mov or lea into rax
// and a ret within a few bytes. Anything else in the slot isn't called.
bool NetChannelSlotOk()
{
    const uint8_t* f = static_cast<const uint8_t*>((*reinterpret_cast<void***>(g_pEngine))[VT_IVEngineClient_GetNetChannelInfo]);
    if (!f || f[0] != 0x48 || (f[1] != 0x8B && f[1] != 0x8D) || f[2] != 0x05) return false;
    for (int i = 7; i < 16; ++i)
        if (f[i] == 0xC3) return true;
    return false;
}

bool CommandEndsFrame(int cmd, bool firstRun)
{
    if (cmd < 0 || !g_netChannelOk) return true;
    int& slot = g_chokedCmd[cmd & (kPacketRing - 1)];
    if (!firstRun) return slot != cmd;
    // A first run comes after CL_Move sent or choked the command.
    void* nci = VFunc<void* (*)(void*)>(g_pEngine, VT_IVEngineClient_GetNetChannelInfo)(g_pEngine);
    bool end = true;
    if (nci) {
        // Until a later datagram covers it, a choked command's slot still holds cmd - 64, sent 64 ticks ago.
        const float age = VFunc<float (*)(void*)>(nci, VT_INetChannelInfo_GetTime)(nci)
            - VFunc<float (*)(void*, int, int)>(nci, VT_INetChannelInfo_GetPacketTime)(nci, 0, cmd);
        end = VFunc<bool (*)(void*, int, int)>(nci, VT_INetChannelInfo_IsValidPacket)(nci, 0, cmd) && age < 0.25f;
    }
    slot = end ? INT_MIN : cmd;
    return end;
}

float Hooked_GetPlayerMaxSpeed(void* self)
{
    const float speed = oGetPlayerMaxSpeed(self);
    return self == g_maxSpeedPlayer ? g_maxSpeedValue : speed;
}

// Every C_CSPlayer shares the vtable, so it's patched once, for the first player seen.
bool HookMaxSpeed(void* player)
{
    void** vt = *reinterpret_cast<void***>(player);
    if (g_maxSpeedVtable) return vt == g_maxSpeedVtable;
    oGetPlayerMaxSpeed = reinterpret_cast<GetPlayerMaxSpeedFn>(vt[g_maxSpeedIndex]);
    if (!PatchPointer(&vt[g_maxSpeedIndex], reinterpret_cast<void*>(&Hooked_GetPlayerMaxSpeed))) {
        g_maxSpeedIndex = -1;
        return false;
    }
    g_maxSpeedVtable = vt;
    return true;
}

// The server's index for m_hGroundEntity: -1 for none, the entity index otherwise.
int GroundIndex(const char* pl)
{
    const uint32_t h = *reinterpret_cast<const uint32_t*>(pl + g_groundOffset);
    return h == 0xFFFFFFFFu ? -1 : (int)(h & 0xFFF);
}

// TR_TraceHullFilter(origin, origin - distance, m_vecMins, m_vecMaxs, MASK_PLAYERSOLID, players filtered).
// PostThink sets the collision bounds from FL_DUCKING, so they're the start's.
bool TraceDown(const float origin[3], float distance, bool ducked, bool onGround, bool firstRun, RngTraceResult& tr)
{
    RngTracer* t = RngPredictTracer(*g_rngp, origin, onGround, firstRun);
    if (!t) return false;
    const float mins[3] = { -16.0f, -16.0f, 0.0f };
    const float maxs[3] = { 16.0f, 16.0f, ducked ? 45.0f : 62.0f };
    const float below[3] = { origin[0], origin[1], origin[2] - distance };
    return t->TraceHull(origin, below, mins, maxs, tr);
}

// FindUseEntity's MASK_NPCSOLID_BRUSHONLY | MASK_OPAQUE_AND_NPCS. Only the engine tracer takes a mask.
bool UseTrace(const float start[3], const float end[3], float half, float* fraction)
{
    if (!g_rngp || !g_rngp->engine || !g_rngp->engine->Ready()) return false;
    const float mins[3] = { -half, -half, -half }, maxs[3] = { half, half, half };
    RngTraceResult tr;
    if (!g_rngp->engine->TraceMask(start, end, mins, maxs, 0x202408B, tr)) return false;
    *fraction = tr.fraction;
    return true;
}

// Both methods trace 2 units down.
bool LandfixTrace(const float origin[3], bool ducked, bool onGround, bool firstRun, float* endZ, bool* hit)
{
    RngTraceResult tr;
    if (!TraceDown(origin, 2.0f, ducked, onGround, firstRun, tr)) return false;
    *hit = tr.fraction < 1.0f || tr.allSolid || tr.startSolid;
    *endZ = tr.endpos[2];
    return true;
}

int JumpKey(const char* pl)
{
    return g_runningKey != INT_MIN ? g_runningKey : *reinterpret_cast<const int*>(pl + g_tickBaseOffset);
}

void JumpLogStart(char* pl, char* md, bool firstRun)
{
    const int key = JumpKey(pl);
    const float z = reinterpret_cast<float*>(md + MV_AbsOrigin)[2];
    const float vz = reinterpret_cast<float*>(md + MV_Velocity)[2];
    JumpStart& slot = g_jumpStarts[((key % PRED_RING_SIZE) + PRED_RING_SIZE) % PRED_RING_SIZE];
    if (firstRun) {
        slot = { key, z, vz };
    } else if (key != g_lastRunKey + 1 && g_lastJump.key != INT_MIN && key > g_lastJump.key) {
        // A replay starts from the server's state after the command before it; slot has what we
        // predicted for that point before the server's answer for the jump came back.
        if (slot.key == key && key - g_lastJump.key <= 32)
            PredLog("jump: cmd %d%s offset %.4f (replayed %.4f), server z %+.4f vz %+.3f, %d ticks after",
                g_lastJump.key, g_lastJump.ducked ? " ducked" : "", g_lastJump.predOff, g_lastJump.replayOff,
                z - slot.z, vz - slot.vz, key - 1 - g_lastJump.key);
        g_lastJump.key = INT_MIN;
    }
    g_lastRunKey = key;
}

void JumpLogJump(float preZ)
{
    const int key = JumpKey(g_jumpPl);
    const bool ducked = (*reinterpret_cast<const int*>(g_jumpPl + g_flagsOffset) & FL_DUCKING) != 0;
    float origin[3];
    memcpy(origin, g_jumpMd + MV_AbsOrigin, sizeof(origin));
    origin[2] = preZ;
    float endZ;
    bool hit;
    const float off = LandfixTrace(origin, ducked, true, g_jumpFirstRun, &endZ, &hit) && hit ? preZ - endZ : NAN;
    if (g_jumpFirstRun)
        g_lastJump = { key, off, NAN, ducked };
    else if (key == g_lastJump.key)
        g_lastJump.replayOff = off;
}

// The player's hull as landfix.sp traces it; false when nothing can trace.
bool SjHull(const float start[3], const float end[3], bool ducked, bool onGround, bool firstRun, RngTraceResult& tr)
{
    RngTracer* t = g_rngp ? RngPredictTracer(*g_rngp, start, onGround, firstRun) : nullptr;
    if (!t) return false;
    const float mins[3] = { -16.0f, -16.0f, 0.0f };
    const float maxs[3] = { 16.0f, 16.0f, ducked ? 45.0f : 62.0f };
    return t->TraceHull(start, end, mins, maxs, tr);
}

// landfix.sp's Landfix_OnJump: every jump ends the last lift, and a bhop or jumpbug may start a new one.
void SjLandfixJump()
{
    const SjJump& j = g_sjJump;
    float* origin = reinterpret_cast<float*>(j.md + MV_AbsOrigin);
    float z = origin[2];
    if (j.lifts) {
        auto hull = [&](const float* a, const float* b, bool* hit, bool* startSolid, float* endZ) {
            RngTraceResult tr;
            if (!SjHull(a, b, j.ducked, j.onGround, j.firstRun, tr)) return false;
            *hit = tr.fraction < 1.0f || tr.allSolid || tr.startSolid;
            *startSolid = tr.startSolid;
            *endZ = tr.endpos[2];
            return true;
        };
        LandfixSjZ(origin, hull, &z);
    }
    const float lift = z - origin[2];
    origin[2] = z;
    if (j.triggers) g_rt.world.landfixLift = lift;
}

// Boosters are touched from where the player would be without the lift, traced so it stays out of the floor.
bool SjUnlift(const float origin[3], float lift, const float mins[3], const float maxs[3], float out[3])
{
    RngTracer* t = g_rngp ? RngPredictTracer(*g_rngp, origin, false, false) : nullptr;
    if (!t) return false;
    const float end[3] = { origin[0], origin[1], origin[2] - lift };
    RngTraceResult tr;
    if (!t->TraceHull(origin, end, mins, maxs, tr)) return false;
    memcpy(out, tr.endpos, sizeof(tr.endpos));
    return true;
}

bool Hooked_CheckJumpButton(void* self)
{
    const OliviaRun& o = g_olivia;
    const bool jumpLog = g_jumpLog && g_jumpMd;
    const float preZ = jumpLog ? reinterpret_cast<float*>(g_jumpMd + MV_AbsOrigin)[2] : 0.0f;
    int boostDoor = -1;
    if (g_mpbhopsJump.pl) {
        // Without the ground handle any ground counts; a jump needs one.
        const bool onEntity = g_groundOffset <= 0 || GroundIndex(g_mpbhopsJump.pl) > 0;
        const float* origin = reinterpret_cast<float*>(g_mpbhopsJump.md + MV_AbsOrigin);
        boostDoor = PredMpbhopsJumpDoor(g_rt, origin, onEntity);
        PredMpbhopsJumpGround(g_rt, origin, onEntity, (*reinterpret_cast<const uint8_t*>(g_mpbhopsJump.md + MV_Flags) & 2) != 0);
    }
    if (o.pl) {
        OliviaState s;
        s.alive         = true;
        s.waterJumpTime = g_waterJumpOffset > 0 ? *reinterpret_cast<const float*>(o.pl + g_waterJumpOffset) : 0.0f;
        s.waterLevel    = WaterLevel(o.pl);
        s.onGround      = GroundIndex(o.pl) != -1;
        s.oldButtons    = *reinterpret_cast<const int*>(o.md + MV_OldButtons) & ~(o.clearOldJump ? (1 << 1) : 0);
        float* origin = reinterpret_cast<float*>(o.md + MV_AbsOrigin);
        float endZ, z;
        bool hit;
        if (LandfixOliviaArmed(s) && LandfixTrace(origin, o.ducked, o.onGround, o.firstRun, &endZ, &hit)
                && hit && LandfixOliviaZ(origin[2], endZ, &z))
            origin[2] = z;
    }
    const bool jumped = oCheckJumpButton(self);
    if (jumped && jumpLog) JumpLogJump(preZ);
    if (jumped) js::OnJump();
    if (jumped && boostDoor >= 0)
        PredMpbhopsJump(g_rt, boostDoor, reinterpret_cast<float*>(g_mpbhopsJump.pl + g_baseVelOffset));
    if (jumped && g_sjJump.pl) SjLandfixJump();
    return jumped;
}

struct ZoneNow {
    bool  autobhopKnown = false, autobhop = false;
    bool  easybhopKnown = false, easybhop = false;
    bool  aaKnown = false;
    float aa = 0.0f;
};
bool g_autobhopNow;

const int kZonesFormat = 1;
const double kZonesPollSeconds = 0.25;
struct {
    bool   on = false;
    double polledAt = 0.0;
    std::string header; // of the list applied
} g_zoneTable;

// Zone edits reach the table mid-map. Nothing here is called when they do, so the header is looked at
// now and then; it differs whenever the list does.
void PollZones()
{
    if (!g_zoneTable.on) return;
    const double now = MonotonicSeconds();
    if (now - g_zoneTable.polledAt < kZonesPollSeconds) return;
    g_zoneTable.polledAt = now;
    const std::string header = PublishedHeader(g_stringTables, "bhoptools_zones");
    if (header.empty() || header == g_zoneTable.header) return;
    std::string info, text;
    if (!ReadPublished(g_stringTables, "bhoptools_zones", info, text)) return;
    g_zoneTable.header = header;
    int format = 0, seq = 0;
    std::vector<ZsZone> zones;
    if (sscanf(info.c_str(), "%d %d", &format, &seq) != 2 || format != kZonesFormat || !ZsParseList(text.data(), text.size(), zones)) {
        CoreLog("bhoptools: couldn't read the zone list (%s)", info.c_str());
        return;
    }
    PredZonesSet(g_rt, zones, true);
    CoreLog("bhoptools: %d shavit zones, list %d", (int)zones.size(), seq);
}

bool ZonesReady(const char* pl)
{
    return g_enabled && g_netvarsReady.load(std::memory_order_acquire) && !g_levelFailed && g_rt.map && g_offsetsGood
        && Alive(pl) && PredZonesOn(g_rt);
}

int ZoneKey(const char* pl, int cmdNumber)
{
    return cmdNumber >= 0 ? cmdNumber : *reinterpret_cast<const int*>(pl + g_tickBaseOffset);
}

// Read before the sim's PreMove restores the command's world.
ZoneNow ZonesNow(const char* pl)
{
    ZoneNow z;
    if (!ZonesReady(pl)) return z;
    const ZsState& st = PredWorldBefore(g_rt, ZoneKey(pl, g_runningCmdNumber)).zones;
    const ZsSettings& s = g_rt.zoneSettings.For(g_runningCmdNumber >= 0 ? g_runningCmdNumber : INT_MIN);
    z.autobhopKnown = ZsAutobhop(g_rt.zoneSet, s, st, &z.autobhop);
    z.easybhopKnown = ZsEasybhop(g_rt.zoneSet, s, st, &z.easybhop);
    // shavit replicates sv_airaccelerate to us a round trip late.
    z.aaKnown = ZsHas(g_rt.zoneSet, ZS_AIRACCEL) && g_svAirAccelerate && ZsAiraccelerate(g_rt.zoneSet, s, st, &z.aa);
    return z;
}

// SuperLandfix's OnPlayerRunCmd step, before the move; arms the NoTimeLoss hook for it.
void LandfixPreMove(char* pl, char* md, bool firstRun, bool active)
{
    g_olivia.pl = nullptr;
    g_sjJump.pl = nullptr;
    int key = g_runningKey;
    if (key == INT_MIN) key = *reinterpret_cast<const int*>(pl + g_tickBaseOffset);
    const int ground = GroundIndex(pl);
    int prev = 0;
    const bool havePrev = g_groundLog.Get(key - 1, &prev);
    g_groundLog.Put(key, ground);
    if (!active) return;

    const int flags = *reinterpret_cast<const int*>(pl + g_flagsOffset);
    // Armed for every style: a jump in one without the lift still ends the last one.
    if (g_sjLandfix) {
        if (!oCheckJumpButton) return;
        g_sjJump = { pl, md, (flags & FL_DUCKING) != 0, (flags & FL_ONGROUND) != 0, firstRun,
            g_moveNow.landfix == LANDFIX_SOURCEJUMP && LandfixSjJumpLifts(havePrev, prev, ground), ground, false };
        return;
    }

    const int moveType = g_moveTypeOffset > 0 ? *reinterpret_cast<const uint8_t*>(pl + g_moveTypeOffset) : PRED_MOVETYPE_WALK;
    const bool alive = LifeAlive(pl);
    float* origin = reinterpret_cast<float*>(md + MV_AbsOrigin);

    if (g_moveNow.landfix == LANDFIX_HAZE && LandfixHazeWanted(havePrev, prev, ground, g_move.hazeSkip)) {
        float endZ = 0.0f;
        bool hit = false;
        if (alive && moveType == PRED_MOVETYPE_WALK && WaterLevel(pl) <= 1
                && !LandfixTrace(origin, (flags & FL_DUCKING) != 0, (flags & FL_ONGROUND) != 0, firstRun, &endZ, &hit))
            return;
        const float z = LandfixHazeZ(origin[2], hit, endZ);
        // The server's SetupMove sees the moved origin and sets m_bGameCodeMovedPlayer.
        if (z != origin[2]) {
            origin[2] = z;
            *reinterpret_cast<uint8_t*>(md + MV_Flags) |= 2;
        }
    } else if (g_moveNow.landfix == LANDFIX_OLIVIA && alive && oCheckJumpButton) {
        const int buttons = *reinterpret_cast<const int*>(md + MV_Buttons);
        const bool autobhop = g_autobhopNow;
        g_olivia.pl = pl;
        g_olivia.md = md;
        g_olivia.ducked = (flags & FL_DUCKING) != 0;
        g_olivia.clearOldJump = autobhop && (buttons & (1 << 1)) && moveType == PRED_MOVETYPE_WALK && WaterLevel(pl) < 2;
        g_olivia.onGround = (flags & FL_ONGROUND) != 0;
        g_olivia.firstRun = firstRun;
    }
}



// A replay starts from the server's state after the command before it, where we had our own
// prediction of the same point.
void PredLogStart(const char* md, bool firstRun, int key)
{
    const Vector3 o = *reinterpret_cast<const Vector3*>(md + MV_AbsOrigin);
    const Vector3 v = *reinterpret_cast<const Vector3*>(md + MV_Velocity);
    PredCmd& c = PredCmdFor(key);
    if (firstRun) {
        g_predCommands++;
        g_predNewestKey = key;
    } else if (key != g_predLastRunKey + 1 && c.key == key) {
        const Vector3 dO = { o.x - c.start.x, o.y - c.start.y, o.z - c.start.z };
        const Vector3 dV = { v.x - c.startVel.x, v.y - c.startVel.y, v.z - c.startVel.z };
        if (Len(dO) > 0.01f || Len(dV) > 0.01f) {
            g_predErrors++;
            const PredCmd& p = PredCmdFor(key - 1);
            char what[160] = "";
            if (p.key == key - 1)
                snprintf(what, sizeof(what), "%s%s ground %d basevel %.1f%s%s%s",
                    (p.flags & 1) ? "onground" : "air", (p.flags & 2) ? " ducked" : "",
                    p.ground == 0xFFFFFFFFu ? -1 : (int)(p.ground & 0xFFF), p.baseVel,
                    p.teleported ? " teleported" : "", p.velTeleported ? " rngfix-teleport" : "",
                    p.snapped ? " view-snap" : "");
            PredLog("correction cmd %d at %.1f %.1f %.1f: pos %.3f (%+.3f %+.3f %+.3f) vel %.3f (%+.2f %+.2f %+.2f), %d in flight, %s",
                key - 1, o.x, o.y, o.z, Len(dO), dO.x, dO.y, dO.z, Len(dV), dV.x, dV.y, dV.z,
                g_predNewestKey - key + 1, p.key == key - 1 ? what : "no record");
        }
    }
    c = {};
    c.key = key;
    c.start = o;
    c.startVel = v;
    g_predLastRunKey = key;
}

void PredLogEnd(const char* pl, int key, bool snapped)
{
    PredCmd& c = PredCmdFor(key);
    if (c.key != key) return;
    c.flags = *reinterpret_cast<const int*>(pl + g_flagsOffset);
    c.ground = g_groundOffset > 0 ? *reinterpret_cast<const uint32_t*>(pl + g_groundOffset) : 0xFFFFFFFFu;
    c.baseVel = Len(*reinterpret_cast<const Vector3*>(pl + g_baseVelOffset));
    c.teleported = g_moveTeleported;
    c.velTeleported = g_moveVelocityTeleported;
    c.snapped = snapped;
}

void Hooked_ProcessMovement(void* thisptr, void* pPlayer, void* pMove)
{
    if (!pMove || !pPlayer) { oProcessMovement(thisptr, pPlayer, pMove); return; }

    char* md = static_cast<char*>(pMove);
    Vector3* origin   = reinterpret_cast<Vector3*>(md + MV_AbsOrigin);
    Vector3* velocity = reinterpret_cast<Vector3*>(md + MV_Velocity);
    char* pl = static_cast<char*>(pPlayer);
    g_moveTeleported = false;
    g_moveVelocityTeleported = false;

    const bool firstRun =
        (*reinterpret_cast<unsigned char*>(md + MV_Flags) & 1) != 0;

    const bool netvars = g_netvarsReady.load(std::memory_order_acquire);

    const bool predLog = g_predLog && netvars && g_flagsOffset > 0;
    const int predKey = g_runningKey != INT_MIN ? g_runningKey
        : netvars ? *reinterpret_cast<const int*>(pl + g_tickBaseOffset) : 0;
    if (predLog) PredLogStart(md, firstRun, predKey);
    if (firstRun && g_runningCmdNumber >= 0)
        g_cmdViews[g_runningCmdNumber % kCmdViews] = { g_runningCmdNumber, g_ourAngle };

    g_jumpMd = nullptr;
    if (g_jumpLog && netvars && g_flagsOffset > 0) {
        JumpLogStart(pl, md, firstRun);
        g_jumpPl = pl;
        g_jumpMd = md;
        g_jumpFirstRun = firstRun;
    }

    // A checkpoint restore has already rewritten mv this command.
    if (!g_offsetsChecked && netvars && !g_rt.cpTeleportPending) {
        const Vector3 mvVel = *velocity;
        if (Len(mvVel) > 50.0f) {
            const Vector3 entVel = *reinterpret_cast<const Vector3*>(pl + g_velOffset);
            const float err = fabsf(entVel.x - mvVel.x)
                            + fabsf(entVel.y - mvVel.y)
                            + fabsf(entVel.z - mvVel.z);

            const int flags = *reinterpret_cast<const int*>(pl + g_flagsOffset);
            const bool layoutOk = (flags & FL_CLIENT_BIT) != 0 && (flags & (FL_CLIENT_BIT >> 1)) == 0;

            g_offsetsChecked = true;
            g_offsetsGood    = (err < 1.0f) && layoutOk;
        }
    }

    // rngfix also runs when the map's triggers couldn't be loaded.
    if (netvars) LateInit();
    const bool rngWanted = g_enabled && netvars && g_offsetsGood && TeleportPredict_RngfixSettings().Active();

    if (g_enabled && netvars) MaybeReloadLevel();

    // The server's changes to stock movement, with or without trigger and rngfix prediction.
    const bool serverMove = g_enabled && (!g_offsetsChecked || g_offsetsGood);
    const int cmdNow = g_runningCmdNumber;
    g_moveNow = MoveFor(cmdNow >= 0 ? cmdNow : INT_MIN);
    const ZoneNow zn = ZonesNow(pl);
    if (zn.easybhopKnown) g_moveNow.easybhop = zn.easybhop;
    g_autobhopNow = zn.autobhopKnown ? zn.autobhop : TeleportPredict_RngfixSettings().autobhop;
    g_maxSpeedValue  = g_moveNow.runspeed;
    g_maxSpeedPlayer = serverMove && g_moveNow.runspeed > 0.0f && g_maxSpeedIndex > 0 && LifeAlive(pl)
        && HookMaxSpeed(pPlayer) ? pPlayer : nullptr;
    if (netvars && g_groundOffset > 0)
        LandfixPreMove(pl, md, firstRun, serverMove && (g_moveNow.landfix != LANDFIX_OFF || g_sjLandfix));
    // Zeroed before and after the move, as the server does and networks. Otherwise the stamina jump
    // penalty predicts every hop within a round trip of the last ~21% low.
    float* const stamina = serverMove && netvars && g_moveNow.easybhop && g_staminaOffset > 0
        ? reinterpret_cast<float*>(pl + g_staminaOffset) : nullptr;
    if (stamina) *stamina = 0.0f;
    const int msfTick = netvars ? *reinterpret_cast<const int*>(pl + g_tickBaseOffset) : 0;
    const msf::Player msfPlayer = { pl, g_groundOffset, g_moveTypeOffset, g_frictionOffset,
        g_ppGlobals && *g_ppGlobals ? &(*g_ppGlobals)->frametime : nullptr, CvarOr(g_svBounce, 0.0f),
        g_runningKey != INT_MIN ? g_runningKey : msfTick, (float)msfTick * g_tickInterval };
    msf::Arm(serverMove && netvars ? &msfPlayer : nullptr);

    const bool triggers = g_enabled && netvars && !g_levelFailed && g_rt.map && g_offsetsGood;
    g_rt.mpbhops = triggers && g_mpbhopsLoaded;
    g_rt.useTrace = UseTrace;
    g_rt.landfixUnlift = SjUnlift;
    g_sjJump.triggers = triggers;
    if (!rngWanted) RngPredictStop(*g_rngp, g_rt);
    if (!triggers) {
        g_rt.cpTeleportPending = false;
        g_rt.cpLoad = nullptr;
        g_rt.cpKz = false;
        // A synced gravity is for the next simulated command only.
        g_rt.world.gravityFix = false;
    }
    if (!triggers && !rngWanted) {
        oProcessMovement(thisptr, pPlayer, pMove);
        if (stamina) *stamina = 0.0f;
        g_maxSpeedPlayer = nullptr;
        msf::Arm(nullptr);
        g_olivia.pl = nullptr;
        g_sjJump.pl = nullptr;
        if (netvars) speedo::OnMove(Alive(pl), &velocity->x, firstRun);
        if (predLog) PredLogEnd(pl, predKey, false);
        return;
    }

    const int tick = *reinterpret_cast<const int*>(pl + g_tickBaseOffset);
    if (!ReadTickInterval(tick) && firstRun) UpdateTickInterval(tick);

    PredPlayerIO io;
    io.origin       = &origin->x;
    io.velocity     = &velocity->x;
    io.baseVelocity = reinterpret_cast<float*>(pl + g_baseVelOffset);
    io.gravity      = reinterpret_cast<float*>(pl + g_gravityOffset);
    io.flags        = reinterpret_cast<int*>(pl + g_flagsOffset);
    if (g_groundOffset > 0) io.groundEntity = reinterpret_cast<uint32_t*>(pl + g_groundOffset);
    io.moveFlags    = reinterpret_cast<uint8_t*>(md + MV_Flags);
    io.tick         = tick;
    if (g_runningCmdNumber >= 0) io.command = g_runningCmdNumber;
    io.moveType     = g_moveTypeOffset > 0 ? *reinterpret_cast<const uint8_t*>(pl + g_moveTypeOffset) : PRED_MOVETYPE_WALK;
    io.solid        = Alive(pl);
    io.curtime      = (float)tick * g_tickInterval;
    io.interval     = g_tickInterval;
    io.frameEnd     = CommandEndsFrame(g_runningCmdNumber, firstRun) || g_untouchEachCommand;
    io.buttons      = *reinterpret_cast<const int*>(md + MV_Buttons);
    io.oldButtons   = *reinterpret_cast<const int*>(md + MV_OldButtons);
    memcpy(io.viewAngles, md + MV_ViewAngles, sizeof(io.viewAngles));
    io.viewHeight   = g_viewOffsetZ > 0 ? *reinterpret_cast<const float*>(pl + g_viewOffsetZ)
        : ((*io.flags & FL_DUCKING) ? 47.0f : 64.0f);
    PredSetHull(io, (*io.flags & FL_DUCKING) != 0);

    // Without trigger prediction rngfix gets no PredRuntime: no trigger checks, no telehop.
    PredRuntime* rt = triggers ? &g_rt : nullptr;
    const int teleports = g_rt.teleports;
    const int zoneTeleports = g_rt.zoneTeleports;
    float* const aaVar = zn.aaKnown ? const_cast<float*>(g_svAirAccelerate) : nullptr;
    const float aaServer = aaVar ? *aaVar : 0.0f;
    if (aaVar) *aaVar = zn.aa;
    RngSettings rngNow = TeleportPredict_RngfixSettings();
    rngNow.autobhop = g_autobhopNow;
    const bool rng = rngWanted && RngPredictBegin(*g_rngp, rngNow, RngfixEnv(pl),
        io.origin, (*io.flags & FL_ONGROUND) != 0, firstRun, g_rt);
    RngMoveInput in;
    if (rng) in = RngfixInput(md, pl);

    const RngSettings& rs = TeleportPredict_RngfixSettings();
    g_rt.rngfixTelehop = rs.known && rs.telehop;
    if (triggers) PredPreMove(g_rt, io);
    if (rng) RngPredictPreTick(*g_rngp, io, in, rt);
    g_mpbhopsJump.pl = g_rt.mpbhops ? pl : nullptr;
    g_mpbhopsJump.md = md;
    oProcessMovement(thisptr, pPlayer, pMove);
    if (aaVar) *aaVar = aaServer;
    g_mpbhopsJump.pl = nullptr;
    if (stamina) *stamina = 0.0f;
    g_maxSpeedPlayer = nullptr;
    msf::Arm(nullptr);
    g_olivia.pl = nullptr;
    // landfix.sp ends the lift after a command that ends a second tick on the ground, after its touches.
    g_rt.landfixEndsLift = g_sjJump.pl && g_sjJump.startGround != -1 && GroundIndex(pl) != -1;
    g_sjJump.pl = nullptr;
    if (rng) RngPredictPostMove(*g_rngp, io, g_waterLevelOffset > 0 ? WaterLevel(pl) : -1,
        g_moveTypeOffset > 0 ? *reinterpret_cast<const uint8_t*>(pl + g_moveTypeOffset) : -1);
    if (triggers) PredPostMove(g_rt, io);
    if (rng) RngPredictEndTick(*g_rngp, io, rt);
    g_moveTeleported = g_rt.teleports != teleports || g_rt.zoneTeleports != zoneTeleports;
    // rngfix's stair, incline and telehop fixes set the velocity with TeleportEntity.
    g_moveVelocityTeleported = rng && (g_rngp->rng.fixes & (RNG_FIX_STAIRS | RNG_FIX_INCLINE | RNG_FIX_TELEHOP));

    if (triggers && firstRun) {
        if (g_rt.viewSnapPending) {
            const Vector3 dest = { g_rt.viewSnapAngles[0], g_rt.viewSnapAngles[1], g_rt.viewSnapAngles[2] };
            g_predictedSnaps[g_predictedSnapNext] = { true, g_runningCmdNumber, dest };
            g_predictedSnapNext = (g_predictedSnapNext + 1) % 8;
            EngineSetViewAngles(dest);

            // The server's own snap, to drop when it comes.
            g_snapWatch = true;
            g_snapLeft  = false;
            g_snapAngle = dest;
            g_ourAngle  = dest;
            g_snapTicks = 0;
        }

        if (g_snapWatch && ++g_snapTicks > kSnapWatchCommands)
            g_snapWatch = false;
    }

    speedo::OnMove(io.solid, &velocity->x, firstRun);
    if (predLog) PredLogEnd(pl, predKey, triggers && g_rt.viewSnapPending);
}

// Datamaps also cover m_MoveType, whose RecvProp is a proxy with offset 0.
bool FindNetvars()
{
    dm::FieldReq req[] = {
        { "m_vecBaseVelocity", dm::FIELD_VECTOR,    12, {}, 0, -1 },
        { "m_vecVelocity",     dm::FIELD_VECTOR,    12, {}, 0, -1 },
        { "m_fFlags",          dm::FIELD_INTEGER,   4,  {}, 0, -1 },
        { "m_flGravity",       dm::FIELD_FLOAT,     4,  {}, 0, -1 },
        { "m_nTickBase",       dm::FIELD_INTEGER,   4,  {}, 0, -1 },
        { "m_lifeState",       dm::FIELD_CHARACTER, 1,  {}, 0, -1 },
        { "m_MoveType",        dm::FIELD_CHARACTER, 1,  {}, 0, -1 },
        // rngfix's; m_bDucking and m_flStepSize are CPlayerLocalData members, relative to m_Local.
        { "m_Local",           dm::FIELD_EMBEDDED,  -1, {}, 0, -1 },
        { "m_bDucking",        dm::FIELD_BOOLEAN,   1,  {}, 0, -1 },
        { "m_flStepSize",      dm::FIELD_FLOAT,     4,  {}, 0, -1 },
        { "m_nWaterLevel",     dm::FIELD_CHARACTER, 1,  {}, 0, -1 },
        { "m_hGroundEntity",   dm::FIELD_EHANDLE,   4,  {}, 0, -1 },
        { "m_flWaterJumpTime", dm::FIELD_FLOAT,     4,  {}, 0, -1 },
        { "m_surfaceFriction", dm::FIELD_FLOAT,     4,  {}, 0, -1 },
        { "m_flStamina",       dm::FIELD_FLOAT,     4,  {}, 0, -1 },
        { "m_vecViewOffset",   dm::FIELD_VECTOR,    12, {}, 0, -1 },
    };
    dm::ResolveFields(ModuleDataRanges(CLIENT_MODULE), req, (int)(sizeof(req) / sizeof(req[0])));
    g_baseVelOffset  = req[0].offset;
    g_velOffset      = req[1].offset;
    g_flagsOffset    = req[2].offset;
    g_gravityOffset  = req[3].offset;
    g_tickBaseOffset = req[4].offset;

    // Optional: without these every player counts as alive and walking.
    g_lifeStateOffset = req[5].offset;
    g_moveTypeOffset  = req[6].offset;
    g_observerOffset  = FindNetvarOffset("m_iObserverMode", kDPT_Int);

    // rngfix falls back to defaults without these.
    const int local = req[7].offset;
    g_duckingOffset    = local >= 0 && req[8].offset > 0 ? local + req[8].offset : -1;
    g_stepSizeOffset   = local >= 0 && req[9].offset > 0 ? local + req[9].offset : -1;
    g_waterLevelOffset = req[10].offset;
    g_groundOffset     = req[11].offset;
    g_waterJumpOffset  = req[12].offset;
    g_frictionOffset   = req[13].offset;
    g_staminaOffset    = req[14].offset;
    // No datamap entry on the client, only the RecvProp.
    g_laggedOffset     = FindNetvarOffset("m_flLaggedMovementValue", kDPT_Float);
    g_viewOffsetZ      = req[15].offset > 0 ? req[15].offset + 8 : -1;

    return g_baseVelOffset > 0 && g_velOffset > 0 && g_flagsOffset > 0
        && g_gravityOffset > 0 && g_tickBaseOffset > 0;
}

#ifdef _WIN32
// A plain function: __try can't share a frame with objects that need unwinding.
bool FindNetvarsGuarded()
{
    __try {
        return FindNetvars();
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        g_baseVelOffset = g_velOffset = g_flagsOffset = -1;
        g_gravityOffset = g_tickBaseOffset = -1;
        return false;
    }
}
#else
void* SwapVtableSlot(void* inst, int index, void* hook)
{
    return PatchPointer(&(*reinterpret_cast<void***>(inst))[index], hook);
}
#endif

}

void RegisterCommands()
{
    if (g_registered || !g_rngp) return;
    g_registered = true;
    void* cvar = CreateInterfaceIn(VSTDLIB_MODULE, "VEngineCvar004");
    void* echo = cvar ? VFunc<void* (*)(void*, const char*)>(cvar, VT_Cvar_FindBase)(cvar, "echo") : nullptr;
    const char* echoName = echo ? *reinterpret_cast<const char* const*>(static_cast<char*>(echo) + kCCB_Name) : nullptr;
    if (!echo || !VFunc<bool (*)(void*)>(echo, VT_CCB_IsCommand)(echo) || !echoName || strcmp(echoName, "echo")) {
        CoreLog("bhoptools: couldn't register the prediction commands");
        return;
    }
    const struct { const char* name; const char* help; void (*fn)(const CCommandX&); } defs[kCmdCount] = {
        { "bhoptools_predict", "Client-side prediction of triggers, rngfix, checkpoints and HUD speed (0/1).", Cmd_Predict },
        { "bhoptools_log", "Start or stop writing debug info to cstrike/bhoptools_log.txt, to send with a bug report.", Cmd_Log },
    };
    for (int i = 0; i < kCmdCount; ++i) {
        ConCommandX& c = g_cmds[i];
        c.vtable = *reinterpret_cast<void**>(echo);
        c.name = defs[i].name;
        c.help = defs[i].help;
        c.callback = reinterpret_cast<void*>(defs[i].fn);
        c.bits = 0x02;  // new-style callback taking const CCommand&
        VFunc<void (*)(void*, void*)>(cvar, VT_Cvar_Register)(cvar, &c);
    }
}

bool Init()
{
    g_checkpoints = new std::unordered_map<int, PredCheckpointState>();
    for (int& c : g_chokedCmd) c = INT_MIN;

    g_pGameMovement = CreateInterfaceIn(CLIENT_MODULE, "GameMovement001");
    g_pEngine       = CreateInterfaceIn(ENGINE_MODULE, "VEngineClient014");
    g_stringTables  = CreateInterfaceIn(ENGINE_MODULE, "VEngineClientStringTable001");
    if (!g_pGameMovement || !g_pEngine) return false;
    g_netChannelOk = NetChannelSlotOk();

    void** vt = *reinterpret_cast<void***>(g_pGameMovement);
    oProcessMovement = reinterpret_cast<ProcessMovementFn>(vt[VT_IGameMovement_ProcessMovement]);
    if (!oProcessMovement) return false;

    void** evt = *reinterpret_cast<void***>(g_pEngine);
    g_ppGlobals = FindGlobals();
#ifdef _WIN32
    if (!g_ppGlobals) CoreLog("trigger prediction: gpGlobals not found, measuring the tick interval instead");
#endif
    // The globals load is near the top of CGameMovement::ProcessMovement, before the store.
    if (g_maxSpeedScanFrom)
        g_maxSpeedIndex = FindMaxSpeedVcall(reinterpret_cast<const uint8_t*>(g_maxSpeedScanFrom), 0x400);
    oCheckJumpButton = reinterpret_cast<CheckJumpButtonFn>(vt[VT_IGameMovement_CheckJumpButton]);
    msf::Init(g_pGameMovement);

    // The engine trace logs why if it's unusable; the BSP brushes stand in for it.
    g_rngp = new RngPredict();
    g_rngp->engine = etrace::CreateEngineTracer();

    oGetViewAngles = reinterpret_cast<ViewAnglesFn>(evt[VT_IVEngineClient_GetViewAngles]);
    oSetViewAngles = reinterpret_cast<ViewAnglesFn>(evt[VT_IVEngineClient_SetViewAngles]);
    return oGetViewAngles && oSetViewAngles;
}

#ifdef _WIN32
void Attach()
{
    if (oProcessMovement)
        DetourAttach(&(PVOID&)oProcessMovement, Hooked_ProcessMovement);
    if (oGetViewAngles)
        DetourAttach(&(PVOID&)oGetViewAngles, Hooked_GetViewAngles);
    if (oSetViewAngles)
        DetourAttach(&(PVOID&)oSetViewAngles, Hooked_SetViewAngles);
    void** slot = &(*reinterpret_cast<void***>(g_pGameMovement))[VT_IGameMovement_CheckJumpButton];
    if (oCheckJumpButton && !PatchPointer(slot, reinterpret_cast<void*>(&Hooked_CheckJumpButton)))
        oCheckJumpButton = nullptr;
    msf::Attach();
    js::Attach(msf::MoveHelper());
}

void Detach()
{
    if (oProcessMovement)
        DetourDetach(&(PVOID&)oProcessMovement, Hooked_ProcessMovement);
    if (oGetViewAngles)
        DetourDetach(&(PVOID&)oGetViewAngles, Hooked_GetViewAngles);
    if (oSetViewAngles)
        DetourDetach(&(PVOID&)oSetViewAngles, Hooked_SetViewAngles);
    if (oCheckJumpButton)
        PatchPointer(&(*reinterpret_cast<void***>(g_pGameMovement))[VT_IGameMovement_CheckJumpButton], reinterpret_cast<void*>(oCheckJumpButton));
    if (g_maxSpeedVtable)
        PatchPointer(&g_maxSpeedVtable[g_maxSpeedIndex], reinterpret_cast<void*>(oGetPlayerMaxSpeed));
    g_maxSpeedVtable = nullptr;
    msf::Detach();
    js::Detach();
}
#else
bool Attach()
{
    // The main thread, which also runs console commands.
    RegisterCommands();
    if (oCheckJumpButton && !SwapVtableSlot(g_pGameMovement, VT_IGameMovement_CheckJumpButton, (void*)&Hooked_CheckJumpButton))
        oCheckJumpButton = nullptr;
    msf::Attach();
    js::Attach(msf::MoveHelper());
    return SwapVtableSlot(g_pGameMovement, VT_IGameMovement_ProcessMovement, (void*)&Hooked_ProcessMovement)
        && SwapVtableSlot(g_pEngine, VT_IVEngineClient_GetViewAngles, (void*)&Hooked_GetViewAngles)
        && SwapVtableSlot(g_pEngine, VT_IVEngineClient_SetViewAngles, (void*)&Hooked_SetViewAngles);
}
#endif

bool ResolveNetvars()
{
    if (g_netvarsReady) return true;
#ifdef _WIN32
    const bool ok = FindNetvarsGuarded();
#else
    const bool ok = FindNetvars();
#endif
    if (ok) g_netvarsReady.store(true, std::memory_order_release);
    return ok;
}

bool Enabled() { return g_enabled; }

void SetEnabled(bool on)
{
    g_enabled = on;
    ResetPerTeleportState();
    speedo::SetEnabled(on);
}

void OnLevelInit()
{
    if (g_predLog) {
        PredLog("level load: %d commands, %d corrections since the last", g_predCommands, g_predErrors);
        g_predCommands = g_predErrors = 0;
        g_predLastRunKey = INT_MIN;
    }
    // String tables come with the server info, before signon state NEW loads the map, so they're
    // here already. A reconnect to the same map keeps the loaded level, so its triggers are rebuilt.
    std::string info;
    int format = 0;
    g_mapDiff.wanted = ReadPublished(g_stringTables, "bhoptools_mapdiff", info, g_mapDiff.text)
        && sscanf(info.c_str(), "%d %d", &format, &g_mapDiff.lumpCrc) == 2 && format == kMapDiffFormat;
    g_mapDiff.hash = (int)PredCrc32(g_mapDiff.text.data(), g_mapDiff.text.size());
    if (g_rt.map && UseMapDiff(*g_rt.map)) {
        g_rt.map->stats = decltype(g_rt.map->stats)();
        PredBuildTriggers(*g_rt.map);
        if (g_rngp) g_rngp->rng.otherFor = nullptr;
    }
    if (g_rt.map) PredRestart(g_rt);
    g_checkpoints->clear();
    // Command numbers start again on a new connection.
    g_groundLog.Clear();
    TeleportPredict_ClearZones();
    if (g_rngp) {
        RngReset(g_rngp->rng);
        // Also on a reconnect to the same map, which doesn't reload the level.
        if (g_rngp->engine) g_rngp->engine->Reset();
    }
}

void SetEventQueueFix(bool on)
{
    g_rt.eventQueueFix = on;
    // Pending events were scheduled on the other clock.
    if (g_rt.map) PredRestart(g_rt);
    g_checkpoints->clear();
}

void SetUntouchEachCommand(bool on) { g_untouchEachCommand = on; }

void SetMpbhopsLoaded(bool on) { g_mpbhopsLoaded = on; }

void SetRunningCommand(int key, int cmdNumber)
{
    g_runningKey = key;
    g_runningCmdNumber = cmdNumber;
}

void SetCheckpointPrinter(void (*print)()) { g_printCheckpoints = print; }


}

void TeleportPredict_OnCheckpointTeleport()
{
    tp::g_rt.cpTeleportPending = true;
}

bool TeleportPredict_SetOwnViewAngles(float pitch, float yaw, float roll)
{
    if (!tp::g_pEngine || !tp::oSetViewAngles) return false;
    const tp::Vector3 a = { pitch, yaw, roll };
    tp::EngineSetViewAngles(a);
    // A trigger teleport's server snap can still be in flight, so keep watching.
    tp::g_ourAngle = a;
    if (tp::g_snapWatch) tp::g_snapLeft = !tp::AnglesClose(a, tp::g_snapAngle);
    return true;
}

void TeleportPredict_ServerViewSnap(int cmd, float pitch, float yaw, float roll)
{
    const tp::Vector3 a = { pitch, yaw, roll };
    for (const tp::PredictedSnap& p : tp::g_predictedSnaps) {
        if (!p.used || !tp::AnglesClose(p.angle, a)) continue;
        if (p.cmd >= 0 && abs(p.cmd - cmd) > tp::kSnapMatchCommands) continue;
        // Already turned. The server cleared its snap, so stop waiting for one.
        if (tp::g_snapWatch && tp::AnglesClose(tp::g_snapAngle, a)) tp::g_snapWatch = false;
        return;
    }
    // Keep what the mouse turned since the command the server teleported on.
    tp::Vector3 ang = a;
    const tp::CmdView& v = tp::g_cmdViews[((cmd % tp::kCmdViews) + tp::kCmdViews) % tp::kCmdViews];
    if (v.cmd == cmd) {
        ang.x = pitch + (tp::g_ourAngle.x - v.angle.x);
        ang.x = ang.x < -89.0f ? -89.0f : (ang.x > 89.0f ? 89.0f : ang.x);
        ang.y = remainderf(yaw + remainderf(tp::g_ourAngle.y - v.angle.y, 360.0f), 360.0f);
    }
    tp::PredLog("teleport snap on cmd %d wasn't predicted, turned to %.1f %.1f%s", cmd, ang.x, ang.y,
        v.cmd == cmd ? "" : " (no view record, turn lost)");
    TeleportPredict_SetOwnViewAngles(ang.x, ang.y, ang.z);
}

bool TeleportPredict_ToggleLog()
{
    if (tp::g_predLog) {
        tp::PredLog("log off: %d commands, %d corrections", tp::g_predCommands, tp::g_predErrors);
        fclose(tp::g_predLog);
        tp::g_predLog = nullptr;
        tp::g_jumpLog = false;
        return false;
    }
    const char* gameDir = tp::EngineGetGameDirectory();
    if (!gameDir) return false;
#ifdef _WIN32
    const std::string path = std::string(gameDir) + "\\bhoptools_log.txt";
#else
    if (tp::g_modDir.empty()) tp::g_modDir = tp::ResolveModDir(gameDir);
    const std::string path = tp::g_modDir + "/bhoptools_log.txt";
#endif
    tp::g_predLog = fopen(path.c_str(), "a");
    if (!tp::g_predLog) return false;
    tp::g_predCommands = tp::g_predErrors = 0;
    tp::g_predLastRunKey = INT_MIN;
    tp::g_lastJump.key = INT_MIN;
    tp::g_jumpLog = true;
    const char* map = tp::EngineGetLevelName();
    const Settings& s = settings::Get();
#ifdef _WIN32
    const char* os = "windows";
#else
    const char* os = "linux";
#endif
    tp::PredLog("log on, bhoptools " APP_VERSION " %s, map %s", os, map && *map ? map : "-");
    tp::PredLog("settings: rawinput %d predict %d viewpunch %d fastdl %d speedo %d hudkeys %d",
        s.rawinput, s.predict, s.viewpunch, s.fastdl, s.speedo, s.hudkeys);
    // These go through CoreLog, which copies to the log while it's open.
    tp::PrintTriggers();
    tp::PrintRngfix();
    if (tp::g_printCheckpoints) tp::g_printCheckpoints();
    return true;
}

void TeleportPredict_LogLine(const char* line)
{
    tp::PredLog("%s", line);
}

void TeleportPredict_PredLog(const char* fmt, ...)
{
    if (!tp::g_predLog) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    tp::PredLog("%s", buf);
}

float* TeleportPredict_LaggedMovement(void* player)
{
    return tp::g_laggedOffset > 0 ? reinterpret_cast<float*>(static_cast<char*>(player) + tp::g_laggedOffset) : nullptr;
}

float TeleportPredict_TickInterval()
{
    const tp::GlobalVarsX* g = tp::g_ppGlobals ? *tp::g_ppGlobals : nullptr;
    return g && g->interval_per_tick > 0.0f && g->interval_per_tick <= 0.1f ? g->interval_per_tick : tp::g_tickInterval;
}

int TeleportPredict_WaterLevel(void* player)
{
    return tp::WaterLevel(static_cast<const char*>(player));
}

float TeleportPredict_SurfaceFriction(void* player)
{
    return tp::g_frictionOffset > 0 ? *reinterpret_cast<const float*>(static_cast<const char*>(player) + tp::g_frictionOffset) : 1.0f;
}

float TeleportPredict_Stamina(void* player)
{
    return tp::g_staminaOffset > 0 ? *reinterpret_cast<const float*>(static_cast<const char*>(player) + tp::g_staminaOffset) : 0.0f;
}

bool TeleportPredict_TraceDown(const float origin[3], float distance, bool ducked, bool onGround, bool firstRun, RngTraceResult& tr)
{
    return tp::TraceDown(origin, distance, ducked, onGround, firstRun, tr);
}

bool TeleportPredict_MoveTeleported(bool velocityOnly)
{
    return tp::g_moveTeleported || (velocityOnly && tp::g_moveVelocityTeleported);
}

void TeleportPredict_OnCheckpointSave(int cpIndex)
{
    if (!tp::g_rt.map || !tp::g_enabled || tp::g_rt.prevTick == INT_MIN) return;
    PredSaveCheckpoint(tp::g_rt, (*tp::g_checkpoints)[cpIndex]);
}

void TeleportPredict_OnCheckpointLoad(int cpIndex, bool kz)
{
    auto it = tp::g_checkpoints->find(cpIndex);
    tp::g_rt.cpLoad = it == tp::g_checkpoints->end() ? nullptr : &it->second;
    tp::g_rt.cpKz = kz;
}

void TeleportPredict_SetStateSync(bool on)
{
    tp::g_rt.stateSync = on;
}

int TeleportPredict_SyncEntity(int cmd, int hammerId, int flags, float value, int wait)
{
    return PredSyncEntity(tp::g_rt, cmd, hammerId, flags, value, wait);
}

int TeleportPredict_SyncPlayer(int cmd, float gravity, bool baseVelFlag, const char* name, const char* cls)
{
    return PredSyncPlayer(tp::g_rt, cmd, gravity, baseVelFlag, name, cls);
}

namespace {
RngSettings g_rngfixSettings;
}

void TeleportPredict_SetRngfixSettings(const RngSettings& s)
{
	g_rngfixSettings = s;
}

const RngSettings& TeleportPredict_RngfixSettings()
{
	return g_rngfixSettings;
}

void TeleportPredict_SetMoveSettings(float runspeed, int landfix, bool easybhop, int fromCmd)
{
	tp::g_move.runspeed = runspeed > 0.0f ? runspeed : 0.0f;
	tp::g_move.landfix = landfix;
	tp::g_move.easybhop = easybhop;
	auto& h = tp::g_moveHistory;
	if (fromCmd == INT_MIN) h.clear();
	const tp::MoveChange c = { fromCmd, tp::g_move.runspeed, landfix, easybhop };
	size_t at = h.size();
	while (at > 0 && h[at - 1].fromCmd > fromCmd) --at;
	h.insert(h.begin() + at, c);
	if (h.size() > 16) h.erase(h.begin());
}

void TeleportPredict_WatchZones(bool on)
{
	tp::g_zoneTable.on = on;
	tp::g_zoneTable.polledAt = 0.0;
}

void TeleportPredict_ClearZones()
{
	PredZonesSet(tp::g_rt, std::vector<ZsZone>(), false);
	tp::g_rt.zoneSettings.Clear();
	tp::g_zoneTable.on = false;
	tp::g_zoneTable.header.clear();
}

void TeleportPredict_SetZoneSettings(int fromCmd, const ZsSettings& s)
{
	tp::g_rt.zoneSettings.Add(fromCmd, s);
}

bool TeleportPredict_ZonesPreCmd(void* player, int cmdNumber, ZsCmd& c, int* inStart, int* freestyle)
{
	*inStart = *freestyle = -1;
	const char* pl = static_cast<const char*>(player);
	tp::PollZones();
	if (!tp::ZonesReady(pl)) return false;
	const PredRuntime& rt = tp::g_rt;
	const int key = tp::ZoneKey(pl, cmdNumber);
	const ZsState& st = PredWorldBefore(rt, key).zones;
	const ZsSettings& s = rt.zoneSettings.For(cmdNumber >= 0 ? cmdNumber : INT_MIN);
	if (ZsKnown(rt.zoneSet, ZS_START)) *inStart = ZsInside(rt.zoneSet, st, ZS_START, s.track);
	if (ZsKnown(rt.zoneSet, ZS_FREESTYLE)) *freestyle = ZsInside(rt.zoneSet, st, ZS_FREESTYLE, -1);
	ZsPreCmd(rt.zoneSet, s, st, c);
	tp::g_rt.zonePreGroundKey = key;
	tp::g_rt.zonePreGround = c.onGround;
	return true;
}

void TeleportPredict_SetSourceJumpLandfix(bool on)
{
	tp::g_sjLandfix = on;
	if (!on) tp::g_rt.world.landfixLift = 0.0f;
}

void TeleportPredict_SetHazeSkip(const std::vector<int>& entities)
{
	tp::g_move.hazeSkip = entities;
}

const MoveSettings& TeleportPredict_MoveSettings()
{
	return tp::g_move;
}
