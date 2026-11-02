#include "fastdl.h"
#include "sigs.h"
#include "utils.h"
#include "lump_checksums.h"
#include "settings.h"

#include <atomic>
#include <cctype>
#include <cstdlib>
#include <cstring>

#ifdef _MSC_VER
#define strcasecmp _stricmp
#endif

namespace fastdl {

ProcessServerInfoFn      oProcessServerInfo      = nullptr;
MD5_MapFileFn            MD5_MapFile             = nullptr;
IsValidFileForTransferFn oIsValidFileForTransfer = nullptr;

typedef void (*SetupURLPathFn)(void* self, char* rc, const char* urlPath);
typedef void (*OnDownloadErrorFn)(void* self, char* rc);
static OnDownloadErrorFn g_originalOnDownloadError = nullptr;
static SetupURLPathFn    g_originalSetupURLPath    = nullptr;
static void**            g_downloadManagerVtable   = nullptr;

static std::atomic<char*> g_lumpChecksums{ nullptr };

static char g_serverMap[260]      = {0};
static char g_matchingMapSha1[41] = {0};
static bool g_hijackMap           = false;
static bool g_requeuedAfterError  = false;

static bool IsBspPath(const char* path)
{
	size_t n = strlen(path);
	return n > 9 && strncmp(path, "maps", 4) == 0 && (path[4] == '/' || path[4] == '\\')
		&& strcasecmp(path + n - 4, ".bsp") == 0;
}

static const char* BaseName(const char* path)
{
	const char* a = strrchr(path, '/');
	const char* b = strrchr(path, '\\');
	const char* end = a > b ? a : b;
	return end ? end + 1 : path;
}

bool Hooked_ProcessServerInfo(void* self, char* msg)
{
	g_matchingMapSha1[0] = 0;
	g_hijackMap = false;
	g_requeuedAfterError = false;
	char* mapName = *(char**)(msg + OFF_SVC_ServerInfo_m_szMapName);
	snprintf(g_serverMap, sizeof(g_serverMap), "%s", mapName);

	const char* csv = g_lumpChecksums.load();
	if (csv && settings::Get().fastdl && LumpChecksumsFindSha1(csv, (const unsigned char*)(msg + OFF_SVC_ServerInfo_m_nMapMD5), g_matchingMapSha1)) {
		char map[260];
		unsigned char mine[16];
		snprintf(map, sizeof(map), "maps/%s.bsp", mapName);
		if (MD5_MapFile(mine, map) && memcmp(mine, msg + OFF_SVC_ServerInfo_m_nMapMD5, 16) != 0) {
			g_hijackMap = true;
			strcpy(mapName, g_matchingMapSha1); // m_szMapNameBuffer[260]
		}
	}
	return oProcessServerInfo(self, msg);
}

static void Hooked_SetupURLPath(void* /*self*/, char* rc, const char* urlPath)
{
	char* dst = rc + OFF_RequestContext_urlPath;
	snprintf(dst, 256, "%s%s", urlPath ? urlPath : rc + OFF_RequestContext_gamePath,
		urlPath && rc[OFF_RequestContext_bIsBZ2] ? ".bz2" : "");
}

static void Hooked_OnDownloadError(void* self, char* rc)
{
	g_originalOnDownloadError(self, rc);
	if (g_requeuedAfterError || !g_matchingMapSha1[0]) return;
	if (!rc[OFF_RequestContext_bAsHTTP] || rc[OFF_RequestContext_bIsBZ2]) return;
	const char* gamePath = rc + OFF_RequestContext_gamePath;
	if (!IsBspPath(gamePath)) return;
	g_requeuedAfterError = true;
	char url[256];
	snprintf(url, sizeof(url), "hashed/%s.bsp", g_matchingMapSha1);
	QueueFromFastdl(self, url, gamePath, true, true);
}

static void InstallDownloadVtableHooks(void* dlman)
{
	void** vt = *(void***)dlman;
	g_originalOnDownloadError = (OnDownloadErrorFn)vt[VT_CDownloadManager_OnDownloadError];
	g_originalSetupURLPath = (SetupURLPathFn)vt[VT_CDownloadManager_SetupURLPath];
	if (!PatchPointer(&vt[VT_CDownloadManager_OnDownloadError], (void*)&Hooked_OnDownloadError))
		return;
	PatchPointer(&vt[VT_CDownloadManager_SetupURLPath], (void*)&Hooked_SetupURLPath);
	g_downloadManagerVtable = vt;
}

#ifdef _WIN32
void RemoveDownloadVtableHooks()
{
	if (!g_downloadManagerVtable) return;
	PatchPointer(&g_downloadManagerVtable[VT_CDownloadManager_OnDownloadError], (void*)g_originalOnDownloadError);
	PatchPointer(&g_downloadManagerVtable[VT_CDownloadManager_SetupURLPath], (void*)g_originalSetupURLPath);
	g_downloadManagerVtable = nullptr;
}
#endif

bool Queue(void* dlman, const char* gamePath, bool bz2, bool plain)
{
	if (!g_downloadManagerVtable) InstallDownloadVtableHooks(dlman);
	if (!g_downloadManagerVtable || !g_hijackMap || !gamePath || !IsBspPath(gamePath)) return false;
	char url[256], file[256];
	snprintf(url, sizeof(url), "hashed/%s.bsp", g_matchingMapSha1);
	snprintf(file, sizeof(file), "maps/%s.bsp", g_matchingMapSha1);
	QueueFromFastdl(dlman, url, file, bz2, plain);
	if (plain) g_hijackMap = false;
	return true;
}

// The engine wants a 3-4 char extension after the first '.', which rejects "bhop_x.v2.bsp".
bool Hooked_IsValidFileForTransfer(const char* filename)
{
	if (oIsValidFileForTransfer(filename)) return true;
	size_t n = strlen(filename);
	if (n < 5 || n > 259 || strcasecmp(filename + n - 4, ".bsp") != 0) return false;
	for (const char* c = BaseName(filename); *c; ++c)
		if (!(isalnum((unsigned char)*c) || *c == '.' || *c == '_' || *c == '-')) return false;
	// The engine rejects these anywhere in the name; masking the dots below would hide them.
	static const char* const blocked[] = { ".cfg", ".lst", ".exe", ".vbs", ".com", ".bat", ".cmd",
		".dll", ".so", ".dylib", ".ini", ".log", ".lua", ".nut", ".vdf", ".smx", ".flt", ".mix", ".asi",
		".gcf", ".lmp", ".sys", ".vpk", ".zip", ".scr", ".pop" };
	char probe[260];
	for (size_t i = 0; i <= n; ++i) probe[i] = (char)tolower((unsigned char)filename[i]);
	for (const char* b : blocked)
		if (strstr(probe, b)) return false;
	memcpy(probe, filename, n + 1);
	for (char* c = (char*)BaseName(probe); c < probe + n - 4; ++c) if (*c == '.') *c = '_';
	return oIsValidFileForTransfer(probe);
}

const char* ServerMapName(const char* levelName)
{
	if (g_matchingMapSha1[0] && g_serverMap[0] && levelName
			&& strncmp(BaseName(levelName), g_matchingMapSha1, 40) == 0)
		return BaseName(g_serverMap);
	return levelName;
}

void LoadLumpChecksums()
{
	FILE* f = OpenLumpChecksums();
	if (!f) {
		CoreLog("fastdl.me map fix: couldn't get lump_checksums.csv");
		return;
	}
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	char* buf = size > 0 ? (char*)calloc(size + 1, 1) : nullptr;
	if (buf && fread(buf, 1, size, f) == (size_t)size)
		g_lumpChecksums.store(buf);
	else
		free(buf);
	fclose(f);
}

}
