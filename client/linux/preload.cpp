#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cwchar>
#include <unistd.h>
#include <sys/mman.h>
#include <pthread.h>
#include <dlfcn.h>
#include <link.h>
#include <climits>
#include <sys/stat.h>

#include "utils.h"
#include "mouse_split.h"
#include "sigs.h"
#include "teleport_predict.h"
#include "checkpoint_predict.h"
#include "speedo.h"
#include "chat_glyphs.h"
#include "download_progress.h"
#include "fastdl.h"
#include "settings.h"
#include "version.h"
#include "update_json.h"
#include "options_tab.h"
#include "spectate.h"
#include "singleletter_fix.h"

static bool InstallHook(uintptr_t target, uintptr_t hook, size_t copy_size, void** original);

typedef double (*Plat_FloatTime_t)();
static Plat_FloatTime_t g_PlatFloatTime = nullptr;

static double plat_now()
{
	if (g_PlatFloatTime) return g_PlatFloatTime();
	// Different epoch, but only deltas of recent samples matter.
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

typedef void (*ConMsg_t)(const char* fmt, ...);
static ConMsg_t g_ConMsg = nullptr;

static void vconmsg(const char* fmt, va_list ap)
{
	char buf[256];
	vsnprintf(buf, sizeof(buf), fmt, ap);
	if (g_ConMsg) g_ConMsg("[bhoptools] %s\n", buf);
	fprintf(stderr, "[bhoptools] %s\n", buf);
	TeleportPredict_LogLine(buf);
}

static void conmsg(const char* fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vconmsg(fmt, ap);
	va_end(ap);
}

typedef void* (*CHud_FindElement_t)(void* hud, const char* name);
typedef void (*ChatPrintf_t)(void* chat, int client, int filter, const char* fmt, ...);
static void* g_hud = nullptr;
static CHud_FindElement_t g_hudFindElement = nullptr;

static uintptr_t Rel32Target(uintptr_t at)
{
	int32_t rel;
	memcpy(&rel, (const void*)at, sizeof rel);
	return at + 4 + rel;
}

static void LocateHudChat()
{
	uintptr_t init = FindPatternIn("client.so", SIG_ClientModeShared_Init);
	if (!init) {
		conmsg("chat: ClientModeShared::Init not found, messages go to the console");
		return;
	}
	g_hud = (void*)Rel32Target(init + OFF_ClientModeShared_Init_gHUD);
	g_hudFindElement = (CHud_FindElement_t)Rel32Target(init + OFF_ClientModeShared_Init_FindElement);
}

// Game thread only: the HUD isn't thread-safe.
static void hudmsg(const char* fmt, ...)
{
	char buf[256];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	void* chat = g_hudFindElement ? g_hudFindElement(g_hud, "CHudChat") : nullptr;
	if (chat)
		(*reinterpret_cast<ChatPrintf_t**>(chat))[VT_CBaseHudChat_ChatPrintf](chat, 0, 0, "%s", buf);
	else
		conmsg("%s", buf);
}

void CoreLog(const char* fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vconmsg(fmt, ap);
	va_end(ap);
}

#define SDL_KEYDOWN      0x300
#define SDL_MOUSEMOTION  0x400
#define SDL_TOUCH_MOUSEID 0xFFFFFFFFu
#define SDL_SCANCODE_F8  65

struct SDL_KeyboardEvent {
	uint32_t type;
	uint32_t timestamp;
	uint32_t windowID;
	uint8_t  state;
	uint8_t  repeat;
	uint8_t  padding2;
	uint8_t  padding3;
	int32_t  scancode;
	int32_t  sym;
	uint16_t mod;
	uint32_t unused;
};

struct SDL_MouseMotionEvent {
	uint32_t type;
	uint32_t timestamp;
	uint32_t windowID;
	uint32_t which;
	uint32_t state;
	int32_t  x, y;
	int32_t  xrel, yrel;
};

union SDL_Event {
	uint32_t type;
	SDL_MouseMotionEvent motion;
	SDL_KeyboardEvent    key;
	uint8_t  padding[56]; // SDL2's size; SDL_PeepEvents fills arrays of these
};

typedef int  (*SDL_EventFilter)(void* userdata, SDL_Event* event);
typedef void (*SDL_AddEventWatch_t)(SDL_EventFilter, void*);
typedef void (*SDL_PumpEvents_t)(void);
typedef int  (*SDL_GetRelativeMouseMode_t)(void);
typedef uint32_t (*SDL_GetMouseState_t)(int*, int*);
typedef int  (*SDL_PollEvent_t)(SDL_Event*);
typedef int  (*SDL_PeepEvents_t)(SDL_Event*, int, int action, uint32_t minType, uint32_t maxType);

static SDL_PumpEvents_t           g_SDLPumpEvents = nullptr;
static SDL_GetRelativeMouseMode_t g_SDLGetRelativeMouseMode = nullptr;
static SDL_GetMouseState_t        g_SDLGetMouseState = nullptr;
static SDL_PollEvent_t            g_SDLPollEvent = nullptr;
static SDL_PeepEvents_t           g_SDLPeepEvents = nullptr;

bool LeftMouseDown()
{
	return g_SDLGetMouseState && (g_SDLGetMouseState(nullptr, nullptr) & 1) != 0;
}

// CSDLMgr only uses SDL relative mode with m_rawinput 1 and the cursor hidden. Otherwise it
// warps the cursor back to centre, so summing xrel would cancel each move; use the engine's deltas.
static bool RelativeMouseMode()
{
	return !g_SDLGetRelativeMouseMode || g_SDLGetRelativeMouseMode();
}

// Written by the SDL watch, read by the CInput hook: both main thread.
static MouseSplit g_mouse;
static double     g_lastHookCallTime = 0.0;

// Walked down from host_frametime by each consumer's frametime; `frameStart - it` is its boundary.
static float  g_mouseSampleTimeLeft = 0.0f; // momentum's m_flMouseSampleTime
static float  g_mouseMoveFrameTime  = 0.0f;
static double g_frameStart          = 0.0;

// The remover jumps over the predicted punch store and zeroes the server's punch via its RecvProp proxy.
// Game memory is only written on the main thread, so the toggle never races the code it patches.
static uint8_t* g_punchPatchSite     = nullptr;
static uint8_t  g_punchPatchOriginal[2] = {0};
static uint8_t  g_punchPatchNew[2]      = {0};
static void**   g_punchProxySlot     = nullptr;
static void*    g_punchProxyOriginal = nullptr;
static bool     g_punchPending       = false;
static bool     g_viewpunchRemoved   = true;

static void ZeroVectorRecvProxy(const void* /*pData*/, void* /*pStruct*/, void* pOut)
{
	float* v = (float*)pOut;
	v[0] = v[1] = v[2] = 0.0f;
}

static bool LocatePunchPatchSite()
{
	uintptr_t hit = FindPatternIn("client.so", SIG_RoughLanding_PunchStore);
	if (!hit) return false;
	uint8_t* site = (uint8_t*)(hit + OFF_RoughLanding_PatchSite);
	uintptr_t limit = (uintptr_t)site + 0x100;
	for (const auto& s : FindExecSegments("client.so"))
		if ((uintptr_t)site >= s.start && (uintptr_t)site < s.end && limit > s.end) limit = s.end;
	uintptr_t epilogue = ScanRange((uintptr_t)site, limit, ParseSig(SIG_RoughLanding_Epilogue));
	if (!epilogue) return false;
	epilogue += OFF_RoughLanding_Epilogue;
	const intptr_t rel = (intptr_t)epilogue - (intptr_t)(site + 2);
	if (rel < 0 || rel > 127) return false;
	memcpy(g_punchPatchOriginal, site, 2);
	g_punchPatchNew[0] = 0xEB;
	g_punchPatchNew[1] = (uint8_t)rel;
	__atomic_store_n(&g_punchPatchSite, site, __ATOMIC_RELEASE);
	return true;
}

static uintptr_t LocatePunchAngleLiteral()
{
	const char needle[] = "m_vecPunchAngle";
	for (const auto& seg : FindSegments("client.so", 'r')) {
		const uint8_t* b = (const uint8_t*)seg.start;
		const size_t n = sizeof(needle);
		for (size_t i = 0; i + n <= seg.end - seg.start; ++i) {
			if (memcmp(b + i, needle, n) == 0) return seg.start + i;
		}
	}
	return 0;
}

static bool LocatePunchRecvProp(uintptr_t str)
{
	auto exec = FindExecSegments("client.so");
	for (const auto& seg : FindSegments("client.so", 'w')) {
		for (uintptr_t a = seg.start & ~(uintptr_t)7; a + RecvProp_SIZE <= seg.end; a += 8) {
			if (*(uintptr_t*)a != str) continue;
			if (*(int*)(a + OFF_RecvProp_m_RecvType) != DPT_Vector) continue;
			if (*(int*)(a + OFF_RecvProp_m_nElements) != 1) continue;
			uintptr_t proxy = *(uintptr_t*)(a + OFF_RecvProp_m_ProxyFn);
			bool in_text = false;
			for (const auto& x : exec) if (proxy >= x.start && proxy < x.end) in_text = true;
			if (!in_text) continue;
			g_punchProxyOriginal = (void*)proxy;
			__atomic_store_n(&g_punchProxySlot, (void**)(a + OFF_RecvProp_m_ProxyFn), __ATOMIC_RELEASE);
			return true;
		}
	}
	return false;
}

// Main thread only.
static void ApplyViewpunch()
{
	uint8_t* site = __atomic_load_n(&g_punchPatchSite, __ATOMIC_ACQUIRE);
	if (site) {
		const uint8_t* bytes = g_viewpunchRemoved ? g_punchPatchNew : g_punchPatchOriginal;
		if (memcmp(site, bytes, 2) != 0) {
			if (mprotect_range((uintptr_t)site, 2, PROT_READ | PROT_WRITE | PROT_EXEC)) {
				memcpy(site, bytes, 2);
				__builtin___clear_cache((char*)site, (char*)site + 2);
				mprotect_range((uintptr_t)site, 2, PROT_READ | PROT_EXEC);
			} else {
				conmsg("Viewpunch: code patch failed (mprotect)");
			}
		}
	}
	void** slot = __atomic_load_n(&g_punchProxySlot, __ATOMIC_ACQUIRE);
	if (slot) *slot = g_viewpunchRemoved ? (void*)&ZeroVectorRecvProxy : g_punchProxyOriginal;
}

// The RecvProp table is a guarded static built on the first ClientClass walk, so poll for it.
static void InstallViewpunchRemover()
{
	if (LocatePunchPatchSite()) __atomic_store_n(&g_punchPending, true, __ATOMIC_RELEASE);

	const uintptr_t literal = LocatePunchAngleLiteral();
	if (!literal) return;
	for (int i = 0; i < 120 * 2; ++i) {
		if (LocatePunchRecvProp(literal)) {
			__atomic_store_n(&g_punchPending, true, __ATOMIC_RELEASE);
			return;
		}
		usleep(500 * 1000);
	}
}

typedef void (*CEngineVGui_UpdateCustomProgressBar_t)(const wchar_t* desc, float progress);
typedef void (*CEngineVGui_UpdateCustomProgressBarThunk_t)(void* self, const wchar_t* desc, float progress);

static CEngineVGui_UpdateCustomProgressBar_t  g_originalUpdateCustomProgressBar = nullptr;
static CEngineVGui_UpdateCustomProgressBarThunk_t g_UpdateCustomProgressBarThunk = nullptr;

// Via the vtable thunk so its GameUI null check still guards the body.
void dlprogress::Draw(const wchar_t* desc, float progress)
{
	g_UpdateCustomProgressBarThunk(nullptr, desc, progress);
}

static void Hooked_UpdateCustomProgressBar(const wchar_t* desc, float progress)
{
	wchar_t buf[256];
	desc = dlprogress::Describe(desc, progress, buf);
	g_originalUpdateCustomProgressBar(desc, progress);
}

static void InstallDownloadProgress()
{
	struct { const char* sig; size_t copy; void* hook; void** original; } hooks[] = {
		{ SIG_CEngineVGui_UpdateCustomProgressBar,
		  HOOK_COPY_CEngineVGui_UpdateCustomProgressBar, (void*)&Hooked_UpdateCustomProgressBar,
		  (void**)&g_originalUpdateCustomProgressBar },
		{ SIG_CDownloadManager_UpdateProgressBar,
		  HOOK_COPY_CDownloadManager_UpdateProgressBar, (void*)&dlprogress::Hooked_UpdateProgressBar,
		  (void**)&dlprogress::oUpdateProgressBar },
		{ SIG_DownloadCache_PersistToDisk,
		  HOOK_COPY_DownloadCache_PersistToDisk, (void*)&dlprogress::Hooked_PersistToDisk,
		  (void**)&dlprogress::oPersistToDisk },
		{ SIG_DecompressBZipToDisk,
		  HOOK_COPY_DecompressBZipToDisk, (void*)&dlprogress::Hooked_DecompressBZipToDisk,
		  (void**)&dlprogress::oDecompressBZipToDisk },
		{ SIG_BZ2_bzread,
		  HOOK_COPY_BZ2_bzread, (void*)&dlprogress::Hooked_BZ2_bzread,
		  (void**)&dlprogress::oBZ2_bzread },
	};

	uintptr_t thunk = FindPatternIn("engine.so", SIG_CEngineVGui_UpdateCustomProgressBar_Thunk);
	if (!thunk) return;
	g_UpdateCustomProgressBarThunk = (CEngineVGui_UpdateCustomProgressBarThunk_t)thunk;

	const size_t hook_count = sizeof(hooks) / sizeof(hooks[0]);
	uintptr_t targets[hook_count];
	for (size_t i = 0; i < hook_count; ++i) {
		targets[i] = FindPatternIn("engine.so", hooks[i].sig);
		if (!targets[i]) return;
	}
	// Progress-bar hook first: every other hook calls into it.
	for (size_t i = 0; i < hook_count; ++i) {
		if (!InstallHook(targets[i], (uintptr_t)hooks[i].hook, hooks[i].copy, hooks[i].original)) return;
	}
}

static void* ModuleHandle(const char* name_substr);

typedef void*  (*curl_easy_init_t)(void);
typedef int    (*curl_easy_setopt_t)(void*, int, ...);
typedef int    (*curl_easy_perform_t)(void*);
typedef void   (*curl_easy_cleanup_t)(void*);
typedef int    (*curl_global_init_t)(long);
#define CURL_GLOBAL_DEFAULT    3
#define CURLOPT_TIMEOUT        13
#define CURLOPT_CONNECTTIMEOUT 78
#define CURLOPT_NOSIGNAL       99
#define CURLOPT_WRITEDATA      10001
#define CURLOPT_ERRORBUFFER    10010
#define CURLOPT_URL            10002
#define CURLOPT_USERAGENT      10018
#define CURLOPT_WRITEFUNCTION  20011
#define CURLOPT_FAILONERROR    45
#define CURLOPT_FOLLOWLOCATION 52

static size_t CurlWriteToFile(char* data, size_t size, size_t n, void* f)
{
	return fwrite(data, size, n, (FILE*)f);
}

// `err`, when given, holds at least 256 bytes and gets the reason on failure.
static bool CurlDownload(const char* url, const char* path, char* err = nullptr)
{
	void* h = ModuleHandle("libcurl-gnutls");
	if (!h) {
		if (err) snprintf(err, 256, "libcurl-gnutls isn't loaded");
		return false;
	}
	auto init    = (curl_easy_init_t)dlsym(h, "curl_easy_init");
	auto setopt  = (curl_easy_setopt_t)dlsym(h, "curl_easy_setopt");
	auto perform = (curl_easy_perform_t)dlsym(h, "curl_easy_perform");
	auto cleanup = (curl_easy_cleanup_t)dlsym(h, "curl_easy_cleanup");
	dlclose(h);
	if (!init || !setopt || !perform || !cleanup) return false;

	char tmp[PATH_MAX + 64];
	snprintf(tmp, sizeof(tmp), "%s.part", path);
	FILE* f = fopen(tmp, "wb");
	if (!f) {
		if (err) snprintf(err, 256, "can't create the download file: %s", strerror(errno));
		return false;
	}
	void* c = init();
	bool ok = false;
	if (c) {
		setopt(c, CURLOPT_URL, url);
		setopt(c, CURLOPT_WRITEFUNCTION, (void*)&CurlWriteToFile);
		setopt(c, CURLOPT_WRITEDATA, (void*)f);
		setopt(c, CURLOPT_FAILONERROR, 1L);
		setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
		setopt(c, CURLOPT_USERAGENT, "bhoptools");
		setopt(c, CURLOPT_NOSIGNAL, 1L);
		setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
		setopt(c, CURLOPT_TIMEOUT, 180L);
		if (err) {
			err[0] = 0;
			setopt(c, CURLOPT_ERRORBUFFER, err);
		}
		const int rc = perform(c);
		ok = rc == 0;
		if (!ok && err && !err[0]) snprintf(err, 256, "curl error %d", rc);
		cleanup(c);
	}
	fclose(f);
	if (ok && rename(tmp, path) != 0) {
		ok = false;
		if (err) snprintf(err, 256, "can't rename the download: %s", strerror(errno));
	}
	if (!ok) unlink(tmp);
	return ok;
}

// curl_global_init isn't thread-safe: call it before the fetch thread or any engine download.
static void CurlGlobalInit()
{
	void* h = ModuleHandle("libcurl-gnutls");
	if (!h) return;
	auto global = (curl_global_init_t)dlsym(h, "curl_global_init");
	if (global) global(CURL_GLOBAL_DEFAULT);
	dlclose(h);
}

FILE* fastdl::OpenLumpChecksums()
{
	const char* home = getenv("HOME");
	if (!home) return nullptr;
	char dir[PATH_MAX], path[PATH_MAX + 32];
	snprintf(dir, sizeof(dir), "%s/.cache/bhoptools", home);
	snprintf(path, sizeof(path), "%s/lump_checksums.csv", dir);
	mkdir(dir, 0755);

	struct stat st;
	bool fresh = stat(path, &st) == 0 && labs((long)(time(nullptr) - st.st_mtime)) < 36 * 60 * 60;
	if (!fresh)
		CurlDownload("https://venus.fastdl.me/lump_checksums.csv", path);
	return fopen(path, "rb");
}

static void* LumpChecksumsThread(void*)
{
	fastdl::LoadLumpChecksums();
	return nullptr;
}

typedef void (*CDownloadManager_Queue_t)(void* self, const char* baseURL, const char* urlPath, const char* gamePath);
typedef void (*CHLClient_LevelInitPreEntity_t)(void* self, const char* mapname);
typedef void* (*CreateInterface_t)(const char* name, int* ret);

static CDownloadManager_Queue_t         g_originalQueue                  = nullptr;
static CHLClient_LevelInitPreEntity_t   g_originalLevelInitPreEntity     = nullptr;

static bool g_checkpointPredictReady = false;
static bool g_teleportPredictReady = false;

// Queue itself tries the .bz2 and then the file for an https base URL.
void fastdl::QueueFromFastdl(void* dlman, const char* url, const char* gamePath, bool /*bz2*/, bool /*plain*/)
{
	g_originalQueue(dlman, fastdl::BASE, url, gamePath);
}

static void Hooked_Queue(void* self, const char* baseURL, const char* urlPath, const char* gamePath)
{
	if (fastdl::Queue(self, gamePath, true, true)) return;
	g_originalQueue(self, baseURL, urlPath, gamePath);
}

static void Hooked_LevelInitPreEntity(void* self, const char* mapname)
{
	settings::ApplyRawInput(true);
	if (g_checkpointPredictReady) cp::OnLevelInit();
	// A same-map reconnect keeps the level name, so the sim can't see it itself.
	if (g_teleportPredictReady) tp::OnLevelInit();
	mapname = fastdl::ServerMapName(mapname);
	g_originalLevelInitPreEntity(self, mapname);
}

// Checkpoint prediction needs level init even without the fastdl hooks.
static void InstallClientHooks()
{
	void* client = ModuleHandle("client.so");
	CreateInterface_t create = client ? (CreateInterface_t)dlsym(client, "CreateInterface") : nullptr;
	void* hlclient = create ? create("VClient017", nullptr) : nullptr;
	if (client) dlclose(client);
	if (!hlclient) return;

	void** vt = *(void***)hlclient;
	g_originalLevelInitPreEntity = (CHLClient_LevelInitPreEntity_t)vt[VT_CHLClient_LevelInitPreEntity];
	if (!PatchPointer(&vt[VT_CHLClient_LevelInitPreEntity], (void*)&Hooked_LevelInitPreEntity)) {
		g_originalLevelInitPreEntity = nullptr;
		return;
	}
	speedo::Attach(hlclient);
}

// The level name swap in LevelInitPreEntity is part of it: the game must see the server's map name.
static void InstallFastdl()
{
	uintptr_t psi   = FindPatternIn("engine.so", SIG_CClientState_ProcessServerInfo);
	uintptr_t md5   = FindPatternIn("engine.so", SIG_MD5_MapFile);
	uintptr_t queue = FindPatternIn("engine.so", SIG_CDownloadManager_Queue);
	uintptr_t valid = FindPatternIn("engine.so", SIG_IsValidFileForTransfer);
	if (!g_originalLevelInitPreEntity || !psi || !md5 || !queue || !valid) return;
	fastdl::MD5_MapFile = (fastdl::MD5_MapFileFn)md5;

	InstallHook(valid, (uintptr_t)&fastdl::Hooked_IsValidFileForTransfer,
		HOOK_COPY_IsValidFileForTransfer, (void**)&fastdl::oIsValidFileForTransfer);
	if (InstallHook(queue, (uintptr_t)&Hooked_Queue,
			HOOK_COPY_CDownloadManager_Queue, (void**)&g_originalQueue))
		InstallHook(psi, (uintptr_t)&fastdl::Hooked_ProcessServerInfo,
			HOOK_COPY_CClientState_ProcessServerInfo, (void**)&fastdl::oProcessServerInfo);

	CurlGlobalInit();
	pthread_t th;
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	pthread_create(&th, &attr, LumpChecksumsThread, nullptr);
	pthread_attr_destroy(&attr);
}

typedef void (*GetAccumulatedMouseDeltas_t)(void* self, float* mx, float* my);
static GetAccumulatedMouseDeltas_t g_originalGetAccumulatedMouseDeltas = nullptr;
static void Hooked_GetAccumulatedMouseDeltas(void* self, float* mx, float* my);
static uintptr_t g_clientTarget  = 0;
static bool      g_installPending = false;

static void InstallVtableHooksEarly();

// Host_FilterTime raises every frame to at least 1 ms, so above 1000 fps the client's clock runs
// ahead of real time: it makes more commands than the server expects and moves too fast between
// corrections. Dropping the floor keeps the real frame time; MAX_FRAMETIME still caps it.
static void RemoveFrametimeFloor()
{
	uint8_t* site = (uint8_t*)FindPatternIn("engine.so", SIG_HostFilterTime_MinFrametime);
	if (!site) {
		conmsg("frametime floor: sig Host_FilterTime NOT FOUND");
		return;
	}
	double floor;
	memcpy(&floor, site + 8 + *(int32_t*)(site + 4), sizeof(floor));
	if (floor != 0.001) {
		conmsg("frametime floor: the clamp isn't 0.001, leaving it");
		return;
	}
	static const uint8_t nop8[8] = { 0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00 };
	if (!mprotect_range((uintptr_t)site, sizeof(nop8), PROT_READ | PROT_WRITE | PROT_EXEC)) {
		conmsg("frametime floor: code patch failed (mprotect)");
		return;
	}
	memcpy(site, nop8, sizeof(nop8));
	__builtin___clear_cache((char*)site, (char*)site + sizeof(nop8));
	mprotect_range((uintptr_t)site, sizeof(nop8), PROT_READ | PROT_EXEC);
}

// The main thread is never inside a patched function while it pumps SDL events.
static void InstallCodeHooks()
{
	if (!InstallHook(g_clientTarget, (uintptr_t)&Hooked_GetAccumulatedMouseDeltas,
			HOOK_COPY_GetAccumulatedMouseDeltas, (void**)&g_originalGetAccumulatedMouseDeltas)) {
		conmsg("mouse hook FAILED to install");
		return;
	}
	LocateHudChat();
	InstallVtableHooksEarly();
	InstallDownloadProgress();
	InstallClientHooks();
	InstallFastdl();
	RemoveFrametimeFloor();
	glyphs::Install();
	speedo::RegisterCommands();
	tp::SetCheckpointPrinter(cp::PrintState);
	singleletter::Install();
	g_teleportPredictReady = tp::Init() && tp::Attach();
	if (!g_teleportPredictReady) conmsg("trigger prediction: unavailable (init failed)");
	// After tp::Attach, so the checkpoint hook wraps the trigger sim's.
	g_checkpointPredictReady = cp::Attach();
	if (g_checkpointPredictReady) cp::SetTriggerPrediction(g_teleportPredictReady);
	if (!g_checkpointPredictReady) conmsg("checkpoint prediction: unavailable");

	settings::SetApply([] {
		const Settings& s = settings::Get();
		if (tp::Enabled() != s.predict) tp::SetEnabled(s.predict);
		if (g_viewpunchRemoved != s.viewpunch) {
			g_viewpunchRemoved = s.viewpunch;
			ApplyViewpunch();
		}
	});
	if (tp::Enabled() != settings::Get().predict) tp::SetEnabled(settings::Get().predict);
}

static pthread_t g_mainThread;

// SDL_PushEvent also calls watches on the pushing thread; only the main thread may patch.
static int Bhoptools_SDLWatch(void* /*userdata*/, SDL_Event* ev)
{
	if (!ev || !pthread_equal(pthread_self(), g_mainThread)) return 1;
	if (__atomic_load_n(&g_installPending, __ATOMIC_RELAXED)
			&& __atomic_exchange_n(&g_installPending, false, __ATOMIC_ACQ_REL)) InstallCodeHooks();
	if (__atomic_load_n(&g_punchPending, __ATOMIC_RELAXED)
			&& __atomic_exchange_n(&g_punchPending, false, __ATOMIC_ACQ_REL)) ApplyViewpunch();
	// GameUI and the client may load after us; a failed look is a few ms of scanning, so once a second.
	static bool optionsDone = false;
	static double optionsNext = 0.0;
	static bool spectateDone = false;
	if ((!optionsDone || !spectateDone) && g_originalGetAccumulatedMouseDeltas && plat_now() >= optionsNext) {
		if (!optionsDone) optionsDone = options::Install();
		if (!spectateDone) spectateDone = spectate::Install();
		optionsNext = plat_now() + 1.0;
	}
	if (ev->type == SDL_KEYDOWN) {
		if (ev->key.repeat) return 1;
		Settings& s = settings::Get();
		if (ev->key.scancode == SDL_SCANCODE_F8) {
			s.predict = !s.predict;
			settings::Changed();
			hudmsg("prediction %d", s.predict ? 1 : 0);
		}
		return 1;
	}
	if (ev->type != SDL_MOUSEMOTION || ev->motion.which == SDL_TOUCH_MOUSEID || !RelativeMouseMode()) return 1;

	const double now = plat_now();
	// Long unconsumed: drop the backlog rather than dump it into one frame when input resumes.
	if (g_lastHookCallTime <= 0.0 || (now - g_lastHookCallTime) > 0.5) MouseSplitClear(g_mouse);

	MouseSplitAdd(g_mouse, ev->motion.xrel, ev->motion.yrel, now);
	return 1;
}

// The engine reaches these through CHLClient forwarding to CInput's vtable, so no code patching.
typedef void (*CInput_CreateMove_t)(void* self, int sequence_number, bool active, float input_sample_frametime);
typedef void (*CInput_ExtraMouseSample_t)(void* self, bool active, float frametime);
typedef void (*CInput_IN_SetSampleTime_t)(void* self, float frametime);

static CInput_CreateMove_t       g_originalCreateMove       = nullptr;
static CInput_ExtraMouseSample_t g_originalExtraMouseSample = nullptr;
static CInput_IN_SetSampleTime_t g_originalIN_SetSampleTime = nullptr;
static bool                      g_vtableHooked             = false;
static bool                      g_vtableHookOk             = false;

static void Hooked_CreateMove(void* self, int sequence_number, bool active, float input_sample_frametime)
{
	g_mouseMoveFrameTime = input_sample_frametime;
	g_originalCreateMove(self, sequence_number, active, input_sample_frametime);
	g_mouseMoveFrameTime = 0.0f;
	speedo::OnCommand(self, sequence_number);
	cp::OnCreateMove(sequence_number);
}

static void Hooked_ExtraMouseSample(void* self, bool active, float frametime)
{
	g_mouseMoveFrameTime = frametime;
	g_originalExtraMouseSample(self, active, frametime);
	g_mouseMoveFrameTime = 0.0f;
}

static void Hooked_IN_SetSampleTime(void* self, float frametime)
{
	g_mouseSampleTimeLeft = frametime;
	g_frameStart = plat_now();
	g_originalIN_SetSampleTime(self, frametime);
}

static void InstallVtableHooks(void* self)
{
	g_vtableHooked = true;
	void** vt = *(void***)self;
	g_originalCreateMove       = (CInput_CreateMove_t)vt[VT_CInput_CreateMove];
	g_originalExtraMouseSample = (CInput_ExtraMouseSample_t)vt[VT_CInput_ExtraMouseSample];
	g_originalIN_SetSampleTime = (CInput_IN_SetSampleTime_t)vt[VT_CInput_IN_SetSampleTime];
	// All three or none: the split needs both the frame start and every consumer.
	if (!PatchPointer(&vt[VT_CInput_IN_SetSampleTime], (void*)&Hooked_IN_SetSampleTime)) return;
	PatchPointer(&vt[VT_CInput_CreateMove], (void*)&Hooked_CreateMove);
	PatchPointer(&vt[VT_CInput_ExtraMouseSample], (void*)&Hooked_ExtraMouseSample);
	g_vtableHookOk = true;
}

// The mouse hook installs these on the first mouse read, which a client without focus never
// does, so CInput is also found through CHLClient's forwarder.
static void InstallVtableHooksEarly()
{
	void* client = ModuleHandle("client.so");
	CreateInterface_t create = client ? (CreateInterface_t)dlsym(client, "CreateInterface") : nullptr;
	void* hlclient = create ? create("VClient017", nullptr) : nullptr;
	if (client) dlclose(client);
	if (!hlclient || g_vtableHooked) return;
	const uint8_t* fn = (*(const uint8_t* const**)hlclient)[VT_Client_EncodeUserCmdToBuffer];
	static const uint8_t kTail[] = { 0x48, 0x8B, 0x38, 0x48, 0x8B, 0x07, 0xFF, 0x60, 0x30 };
	if (fn[0] != 0x48 || fn[1] != 0x8D || fn[2] != 0x05 || memcmp(fn + 7, kTail, sizeof kTail) != 0) {
		conmsg("CInput not found through CHLClient; its hooks wait for the first mouse read");
		return;
	}
	int32_t rel;
	memcpy(&rel, fn + 3, sizeof rel);
	void* input = *(void* const*)(fn + 7 + rel);
	if (input) InstallVtableHooks(input);
}

static void Hooked_GetAccumulatedMouseDeltas(void* self, float* mx, float* my)
{
	if (!g_vtableHooked && self) InstallVtableHooks(self);

	// Result unused, but the call zeros CInput's cooked accumulators and drains CSDLMgr's raw one.
	float engineX = 0.0f, engineY = 0.0f;
	g_originalGetAccumulatedMouseDeltas(self, &engineX, &engineY);

	const float frametime = g_mouseMoveFrameTime;
	g_mouseMoveFrameTime = 0.0f;

	if (!g_vtableHookOk || !RelativeMouseMode() || !settings::Get().rawinput) {
		if (mx) *mx = engineX;
		if (my) *my = engineY;
		cp::AddInput(engineX, engineY);
		return;
	}

	// The engine pumps SDL after the ticks, so otherwise all the frame's motion lands in ExtraMouseSample.
	if (g_SDLPumpEvents) g_SDLPumpEvents();
	// Tick boundaries are offsets from the frame start: IN_SetSampleTime runs before the tick loop.
	const double base = g_frameStart;
	g_lastHookCallTime = base;

	int rawX = 0, rawY = 0;
	if (g_mouseSampleTimeLeft > 0.0f) {
		if (frametime > 0.0f && frametime < g_mouseSampleTimeLeft) {
			g_mouseSampleTimeLeft -= frametime;
			MouseSplitTake(g_mouse, rawX, rawY, base - (double)g_mouseSampleTimeLeft);
		} else {
			// Last consumer of the frame (ExtraMouseSample) takes everything, late motion included.
			MouseSplitTake(g_mouse, rawX, rawY, 0.0);
			g_mouseSampleTimeLeft = 0.0f;
		}
	}

	if (mx) *mx = (float)rawX;
	if (my) *my = (float)rawY;
	cp::AddInput((float)rawX, (float)rawY);
}

static long g_pageSize = 0;

static const size_t JMP_PATCH_SIZE = 5;

// jmp rel32 only reaches +-2GB, so the stub has to live near the target.
static uint8_t* AllocNearPage(uintptr_t target)
{
	if (g_pageSize == 0) g_pageSize = sysconf(_SC_PAGESIZE);
#ifdef MAP_FIXED_NOREPLACE
	const int hint_flag = MAP_FIXED_NOREPLACE;
#else
	const int hint_flag = 0;
#endif
	const uintptr_t step  = 16 * 1024 * 1024;
	const uintptr_t reach = 0x60000000;
	for (uintptr_t delta = step; delta < reach; delta += step) {
		for (int below = 0; below < 2; ++below) {
			if (below && delta > target) continue;
			uintptr_t hint = below ? (target - delta) : (target + delta);
			hint &= ~(uintptr_t)(g_pageSize - 1);
			void* p = mmap((void*)hint, g_pageSize,
				PROT_READ | PROT_WRITE | PROT_EXEC,
				MAP_PRIVATE | MAP_ANONYMOUS | hint_flag, -1, 0);
			if (p == MAP_FAILED) continue;
			const int64_t d = (int64_t)(uintptr_t)p - (int64_t)target;
			if (d > -0x70000000LL && d < 0x70000000LL) return (uint8_t*)p;
			munmap(p, g_pageSize);
		}
	}
	return nullptr;
}

// copy_size must be >= JMP_PATCH_SIZE and end on an instruction boundary unless original is null.
static bool InstallHook(uintptr_t target, uintptr_t hook, size_t copy_size, void** original)
{
	if (original && (copy_size < JMP_PATCH_SIZE || copy_size > 64)) return false;

	uint8_t* page = AllocNearPage(target);
	if (!page) return false;

	uint8_t* stub = page;
	stub[0] = 0x48; stub[1] = 0xB8;
	memcpy(stub + 2, &hook, 8);
	stub[10] = 0xFF; stub[11] = 0xE0;

	uint8_t* tramp = nullptr;
	if (original) {
		tramp = page + 16;
		memcpy(tramp, (void*)target, copy_size);
		uint8_t* p = tramp + copy_size;
		uint64_t resume = (uint64_t)target + copy_size;
		*p++ = 0x48; *p++ = 0xB8;
		memcpy(p, &resume, 8); p += 8;
		*p++ = 0xFF; *p++ = 0xE0;
	}
	__builtin___clear_cache((char*)page, (char*)page + g_pageSize);

	uint8_t jmp5[JMP_PATCH_SIZE];
	const int32_t rel32 = (int32_t)((int64_t)(uintptr_t)stub - (int64_t)(target + JMP_PATCH_SIZE));
	jmp5[0] = 0xE9;
	memcpy(jmp5 + 1, &rel32, 4);

	if (!mprotect_range(target, 8, PROT_READ | PROT_WRITE | PROT_EXEC)) {
		munmap(page, g_pageSize);
		return false;
	}

	if (original) __atomic_store_n(original, (void*)tramp, __ATOMIC_SEQ_CST);

	// One aligned 8-byte atomic store, so a thread at the entry sees the old code or the whole jmp.
	if ((target & 7) == 0) {
		uint64_t word;
		memcpy(&word, (void*)target, 8);
		memcpy(&word, jmp5, JMP_PATCH_SIZE);
		__atomic_store_n((uint64_t*)target, word, __ATOMIC_SEQ_CST);
	} else {
		// Unaligned entry, so the write can't be atomic.
		memcpy((void*)target, jmp5, JMP_PATCH_SIZE);
	}
	__builtin___clear_cache((char*)target, (char*)target + 8);
	mprotect_range(target, 8, PROT_READ | PROT_EXEC);

	// Patch is live either way; unwinding `original` would strand the hook.
	return memcmp((void*)target, jmp5, JMP_PATCH_SIZE) == 0;
}

// Source dlopens RTLD_LOCAL, so get a handle with RTLD_NOLOAD; callers must dlclose it.
static void* ModuleHandle(const char* name_substr)
{
	auto segs = FindExecSegments(name_substr);
	if (segs.empty()) return nullptr;
	void* h = dlopen(segs[0].path.c_str(), RTLD_LAZY | RTLD_NOLOAD);
	if (!h) h = dlopen(segs[0].path.c_str(), RTLD_LAZY);
	return h;
}

static void ResolvePlatFloatTime()
{
	void* h = ModuleHandle("libtier0.so");
	if (!h) return;
	void* sym = dlsym(h, "Plat_FloatTime");
	if (!sym) sym = dlsym(h, "_Z13Plat_FloatTimev");
	g_ConMsg = (ConMsg_t)dlsym(h, "_Z6ConMsgPKcz");
	dlclose(h);
	g_PlatFloatTime = (Plat_FloatTime_t)sym;
}

static SDL_AddEventWatch_t ResolveSDLAddEventWatch()
{
	// Steam Linux Runtime ships it as libSDL2-2.0.so.0; match on the stem.
	void* h = ModuleHandle("libSDL2");
	if (!h) return nullptr;
	if (!g_SDLPumpEvents) g_SDLPumpEvents = (SDL_PumpEvents_t)dlsym(h, "SDL_PumpEvents");
	if (!g_SDLGetRelativeMouseMode)
		g_SDLGetRelativeMouseMode = (SDL_GetRelativeMouseMode_t)dlsym(h, "SDL_GetRelativeMouseMode");
	if (!g_SDLGetMouseState) g_SDLGetMouseState = (SDL_GetMouseState_t)dlsym(h, "SDL_GetMouseState");
	if (!g_SDLPollEvent) g_SDLPollEvent = (SDL_PollEvent_t)dlsym(h, "SDL_PollEvent");
	if (!g_SDLPeepEvents) g_SDLPeepEvents = (SDL_PeepEvents_t)dlsym(h, "SDL_PeepEvents");
	SDL_AddEventWatch_t fn = (SDL_AddEventWatch_t)dlsym(h, "SDL_AddEventWatch");
	dlclose(h);
	return fn;
}

// CSDLMgr handles at most 100 events a frame and posts a mutex-locked cursor event for every motion
// event, so a 8kHz mouse costs fps and below ~80 fps the queue falls behind. It only sums xrel/yrel
// and keeps the last position, so merging back-to-back motion events gives it the same input.
static int Hooked_SDLPollEvent(SDL_Event* ev)
{
	const int r = g_SDLPollEvent(ev);
	if (!r || ev->type != SDL_MOUSEMOTION || !RelativeMouseMode()) return r;

	const int kBatch = 64;
	SDL_Event next[kBatch];
	for (;;) {
		const int n = g_SDLPeepEvents(next, kBatch, 1 /* SDL_PEEKEVENT */, 0, 0xFFFF);
		int k = 0;
		while (k < n && next[k].type == SDL_MOUSEMOTION && next[k].motion.which == ev->motion.which) k++;
		if (k == 0) break;
		// They're at the head of the queue, so this takes the same k.
		g_SDLPeepEvents(next, k, 2 /* SDL_GETEVENT */, SDL_MOUSEMOTION, SDL_MOUSEMOTION);
		for (int i = 0; i < k; i++) {
			ev->motion.xrel += next[i].motion.xrel;
			ev->motion.yrel += next[i].motion.yrel;
		}
		const SDL_MouseMotionEvent& last = next[k - 1].motion;
		ev->motion.timestamp = last.timestamp;
		ev->motion.state = last.state;
		ev->motion.x = last.x;
		ev->motion.y = last.y;
		if (k < kBatch) break;
	}
	return r;
}

// Redirects one module's PLT import, so other callers are untouched.
static bool HookPltImport(const char* module, const char* symbol, void* hook)
{
	// dl_iterate_phdr rather than dlinfo, which needs glibc 2.34.
	struct Found { const char* name; ElfW(Addr) base; const ElfW(Dyn)* dyn; } found{ module, 0, nullptr };
	dl_iterate_phdr([](dl_phdr_info* info, size_t, void* data) {
		Found* f = (Found*)data;
		if (!info->dlpi_name || !strstr(info->dlpi_name, f->name)) return 0;
		for (int i = 0; i < info->dlpi_phnum; i++)
			if (info->dlpi_phdr[i].p_type == PT_DYNAMIC)
				f->dyn = (const ElfW(Dyn)*)(info->dlpi_addr + info->dlpi_phdr[i].p_vaddr);
		f->base = info->dlpi_addr;
		return 1;
	}, &found);
	if (!found.dyn) return false;
	const ElfW(Addr) base = found.base;

	// glibc relocates these in place on x86-64, but don't depend on it.
	auto abs = [base](ElfW(Addr) p) { return p < base ? p + base : p; };
	const ElfW(Rela)* rela = nullptr;
	size_t relaSize = 0;
	const ElfW(Sym)* syms = nullptr;
	const char* strs = nullptr;
	for (const ElfW(Dyn)* d = found.dyn; d->d_tag != DT_NULL; ++d) {
		if (d->d_tag == DT_JMPREL) rela = (const ElfW(Rela)*)abs(d->d_un.d_ptr);
		else if (d->d_tag == DT_PLTRELSZ) relaSize = d->d_un.d_val;
		else if (d->d_tag == DT_SYMTAB) syms = (const ElfW(Sym)*)abs(d->d_un.d_ptr);
		else if (d->d_tag == DT_STRTAB) strs = (const char*)abs(d->d_un.d_ptr);
	}
	if (!rela || !syms || !strs) return false;

	for (size_t i = 0; i < relaSize / sizeof(ElfW(Rela)); ++i) {
		if (ELF64_R_TYPE(rela[i].r_info) != R_X86_64_JUMP_SLOT) continue;
		if (strcmp(strs + syms[ELF64_R_SYM(rela[i].r_info)].st_name, symbol) != 0) continue;
		const uintptr_t slot = base + rela[i].r_offset;
		if (!mprotect_range(slot, sizeof(void*), PROT_READ | PROT_WRITE)) return false;
		__atomic_store_n((void**)slot, hook, __ATOMIC_RELEASE);
		return true;
	}
	return false;
}

// Same guarded-static timing as the punch RecvProp above.
static void InstallTriggerPredictionNetvars()
{
	for (int i = 0; i < 120 * 2; ++i) {
		if (tp::ResolveNetvars()) return;
		usleep(500 * 1000);
	}
}

static void* UpdateThread(void*);

static void* InstallerThread(void* /*arg*/)
{
	uintptr_t client_target = 0;
	SDL_AddEventWatch_t addEventWatch = nullptr;
	const int max_secs = 300;
	const int iters_per_sec = 10;
	for (int i = 0; i < max_secs * iters_per_sec; ++i) {
		if (!g_PlatFloatTime) ResolvePlatFloatTime();
		if (!addEventWatch) addEventWatch = ResolveSDLAddEventWatch();
		if (!client_target) client_target = FindPatternIn("client.so", SIG_GetAccumulatedMouseDeltas);
		if (g_PlatFloatTime && addEventWatch && client_target) break;
		usleep(100 * 1000);
	}

	if (!g_PlatFloatTime || !addEventWatch || !client_target) {
		conmsg("install FAILED: tier0=%d sdl=%d client_sig=%d",
			g_PlatFloatTime != nullptr, addEventWatch != nullptr, client_target != 0);
		return nullptr;
	}

	g_clientTarget = client_target;
	// The sig can match before client.so is relocated; dlopen takes the loader lock, so this waits.
	auto clientSegs = FindExecSegments("client.so");
	if (!clientSegs.empty())
		if (void* h = dlopen(clientSegs[0].path.c_str(), RTLD_LAZY | RTLD_NOLOAD)) dlclose(h);
	// Static tables only, so no polling. The flag below publishes these and the settings.
	cp::ResolveOffsets();
	settings::Load();
	g_viewpunchRemoved = settings::Get().viewpunch;
	__atomic_store_n(&g_installPending, true, __ATOMIC_RELEASE);
	addEventWatch(Bhoptools_SDLWatch, nullptr);
	if (g_SDLPollEvent && g_SDLPeepEvents && !HookPltImport("launcher.so", "SDL_PollEvent", (void*)&Hooked_SDLPollEvent))
		conmsg("8k mouse merge: launcher.so SDL_PollEvent import not found");

	pthread_t updateTh;
	pthread_attr_t updateAttr;
	pthread_attr_init(&updateAttr);
	pthread_attr_setdetachstate(&updateAttr, PTHREAD_CREATE_DETACHED);
	pthread_create(&updateTh, &updateAttr, UpdateThread, nullptr);
	pthread_attr_destroy(&updateAttr);

	InstallViewpunchRemover();
	InstallTriggerPredictionNetvars();
	return nullptr;
}

struct SDL_MessageBoxButtonData { uint32_t flags; int buttonid; const char* text; };
struct SDL_MessageBoxData {
	uint32_t flags;
	void* window;
	const char* title;
	const char* message;
	int numbuttons;
	const SDL_MessageBoxButtonData* buttons;
	const void* colorScheme;
};
typedef int (*SDL_ShowMessageBox_t)(const SDL_MessageBoxData*, int*);

static int ShowSDLMessageBox(const char* message, const SDL_MessageBoxButtonData* buttons, int count)
{
	// SDL wants at least one button.
	static const SDL_MessageBoxButtonData ok = { 1, 0, "OK" };
	if (!count) { buttons = &ok; count = 1; }
	void* h = ModuleHandle("libSDL2");
	if (!h) return -1;
	auto show = (SDL_ShowMessageBox_t)dlsym(h, "SDL_ShowMessageBox");
	auto focus = (void* (*)())dlsym(h, "SDL_GetKeyboardFocus");
	dlclose(h);
	if (!show) return -1;
	// Parented to the game window so a fullscreen game doesn't cover it, which also means
	// waiting until that window is up.
	void* window = nullptr;
	for (int i = 0; focus && i < 600 && !(window = focus()); i++) usleep(100000);
	SDL_MessageBoxData data = { 0x40, window, "bhoptools update", message, count, buttons, nullptr };
	int id = -1;
	return show(&data, &id) == 0 ? id : -1;
}

static bool DownloadElf(const char* url, const char* path, char* err)
{
	if (!CurlDownload(url, path, err)) return false;
	char magic[4] = {};
	FILE* f = fopen(path, "rb");
	if (f) { if (fread(magic, 1, 4, f) != 4) magic[0] = 0; fclose(f); }
	if (!memcmp(magic, "\x7f" "ELF", 4)) return true;
	snprintf(err, 256, "the download isn't an ELF file");
	unlink(path);
	return false;
}

// Exit 42 makes cstrike.sh start the game again; _restart reloads the engine in-process and crashes.
static void* UpdateThread(void*)
{
	if (!strcmp(APP_VERSION, "dev")) return nullptr;
	const char* home = getenv("HOME");
	if (!home) return nullptr;
	const char* xdg = getenv("XDG_CONFIG_HOME");
	char dir[PATH_MAX], ignoreFile[PATH_MAX + 32], jsonFile[PATH_MAX + 32];
	if (xdg && *xdg) snprintf(dir, sizeof(dir), "%s/bhoptools", xdg);
	else snprintf(dir, sizeof(dir), "%s/.config/bhoptools", home);
	snprintf(ignoreFile, sizeof(ignoreFile), "%s/no_update_check", dir);
	if (access(ignoreFile, F_OK) == 0) return nullptr;

	snprintf(jsonFile, sizeof(jsonFile), "%s/.cache", home);
	mkdir(jsonFile, 0755);
	strcat(jsonFile, "/bhoptools");
	mkdir(jsonFile, 0755);
	strcat(jsonFile, "/release.json");
	if (!CurlDownload("https://api.github.com/repos/ddyfad/bhoptools/releases?per_page=20", jsonFile))
		return nullptr;
	std::string json;
	if (FILE* f = fopen(jsonFile, "rb")) {
		char chunk[4096];
		size_t n;
		while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) json.append(chunk, n);
		fclose(f);
	}
	unlink(jsonFile);
	std::string changelog;
	const std::string tag = Changelog(json, APP_VERSION, changelog);
	if (tag.empty()) return nullptr;

	const SDL_MessageBoxButtonData buttons[] = {
		{ 1, 0, "Update" },
		{ 2, 1, "Ignore" },
		{ 0, 2, "Ignore forever" },
	};
	const int answer = ShowSDLMessageBox((tag + " is out, you have " APP_VERSION ".\n\nChangelog:\n\n" + changelog).c_str(), buttons, 3);
	if (answer == 2) {
		mkdir(dir, 0755);
		if (FILE* f = fopen(ignoreFile, "w")) fclose(f);
		return nullptr;
	}
	if (answer != 0) return nullptr;

	Dl_info info;
	if (!dladdr((void*)&UpdateThread, &info) || !info.dli_fname) return nullptr;
	char soPath[PATH_MAX], newSo[PATH_MAX + 8], url[256];
	snprintf(soPath, sizeof(soPath), "%s", info.dli_fname);
	snprintf(newSo, sizeof(newSo), "%s.new", soPath);
	snprintf(url, sizeof(url), "https://github.com/ddyfad/bhoptools/releases/download/%s/bhoptools.so", tag.c_str());
	char err[256];
	if (!DownloadElf(url, newSo, err)) {
		ShowSDLMessageBox(("Update failed, the current version is unchanged.\n\n" + std::string(err)).c_str(), nullptr, 0);
		return nullptr;
	}

	// The launcher only prints launch options, so it's best effort.
	char launcher[PATH_MAX + 32], newLauncher[PATH_MAX + 40];
	snprintf(launcher, sizeof(launcher), "%s", soPath);
	char* slash = strrchr(launcher, '/');
	if (slash) {
		strcpy(slash + 1, "bhoptools");
		snprintf(newLauncher, sizeof(newLauncher), "%s.new", launcher);
		snprintf(url, sizeof(url), "https://github.com/ddyfad/bhoptools/releases/download/%s/bhoptools", tag.c_str());
		if (access(launcher, F_OK) == 0 && DownloadElf(url, newLauncher, err)) {
			chmod(newLauncher, 0755);
			rename(newLauncher, launcher);
		}
	}
	_exit(42);
}

// The launch goes through bash and reaper helpers; only the game process gets the installer.
static bool is_game_process()
{
	char comm[128] = {0};
	FILE* f = fopen("/proc/self/comm", "r");
	if (!f) return true;
	if (!fgets(comm, sizeof(comm), f)) { fclose(f); return true; }
	fclose(f);
	return strstr(comm, "cstrike_linux64") || strstr(comm, "hl2_linux");
}

// Patched code in a VAC-secured client is a ban risk, so do nothing unless it runs insecure.
// CCommandLine::FindParm is case-insensitive and skips argv[0].
static bool has_insecure_flag()
{
	char cmd[16384];
	size_t n = 0;
	if (FILE* f = fopen("/proc/self/cmdline", "rb")) {
		n = fread(cmd, 1, sizeof(cmd) - 1, f);
		fclose(f);
	}
	cmd[n] = 0;
	for (size_t i = strlen(cmd) + 1; i < n; i += strlen(cmd + i) + 1)
		if (strcasecmp(cmd + i, "-insecure") == 0) return true;
	return false;
}

static void ReexecWith(const char* self, const char* so)
{
	if (getenv("BHOPTOOLS_REEXEC")) return;
	std::string cmdline;
	if (FILE* f = fopen("/proc/self/cmdline", "rb")) {
		char chunk[4096];
		size_t n;
		while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) cmdline.append(chunk, n);
		fclose(f);
	}
	std::vector<char*> argv;
	for (size_t i = 0; i < cmdline.size(); i += strlen(&cmdline[i]) + 1) argv.push_back(&cmdline[i]);
	argv.push_back(nullptr);

	std::string preload = getenv("LD_PRELOAD") ? getenv("LD_PRELOAD") : "";
	const size_t at = preload.find(self);
	if (at == std::string::npos) return;
	preload = "LD_PRELOAD=" + preload.replace(at, strlen(self), so);
	std::vector<char*> env;
	for (char** e = environ; *e; e++)
		if (strncmp(*e, "LD_PRELOAD=", 11)) env.push_back(*e);
	env.push_back(&preload[0]);
	env.push_back((char*)"BHOPTOOLS_REEXEC=1");
	env.push_back(nullptr);
	execve("/proc/self/exe", argv.data(), env.data());
}

__attribute__((constructor))
static void bhoptools_ctor()
{
	// Installs a pending update. Inside the Steam container the rename fails with EBUSY, and the
	// restarted game still has the old .so mapped, so it re-execs with the new one.
	Dl_info info;
	if (dladdr((void*)&bhoptools_ctor, &info) && info.dli_fname) {
		char newSo[PATH_MAX + 8];
		snprintf(newSo, sizeof(newSo), "%s.new", info.dli_fname);
		const bool installed = rename(newSo, info.dli_fname) == 0;
		if (is_game_process() && (installed || access(newSo, F_OK) == 0))
			ReexecWith(info.dli_fname, installed ? info.dli_fname : newSo);
	}

	if (!is_game_process()) return;
	if (!has_insecure_flag()) {
		fprintf(stderr, "[bhoptools] -insecure is not on the command line, not installing\n");
		return;
	}
	// LD_PRELOAD constructors run on the exec'd process's initial thread.
	g_mainThread = pthread_self();

	pthread_t th;
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	pthread_create(&th, &attr, InstallerThread, nullptr);
	pthread_attr_destroy(&attr);
}
