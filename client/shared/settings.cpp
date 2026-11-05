#include "settings.h"
#include "utils.h"
#include "sigs.h"
#include "game_layout.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#endif

namespace settings {
namespace {

Settings g_settings;
std::function<void()> g_apply;
bool g_rawinputSaved;
bool g_lastRawinput = true;

struct Field {
	const char* key;
	bool Settings::* value;
};
const Field kFields[] = {
	{ "rawinput",  &Settings::rawinput },
	{ "predict",   &Settings::predict },
	{ "pingfix",   &Settings::predict },  // read from files saved before the rename
	{ "viewpunch", &Settings::viewpunch },
	{ "fastdl",    &Settings::fastdl },
	{ "speedo",    &Settings::speedo },
	{ "hudkeys",   &Settings::hudkeys },
	{ "jsframe",   &Settings::jsframe },
	{ "chatglyphs", &Settings::chatglyphs },
	{ "pollfix",   &Settings::pollfix },
};

}

std::string Dir()
{
#ifdef _WIN32
	const char* base = getenv("APPDATA");
	return base ? std::string(base) + "\\bhoptools" : std::string();
#else
	const char* xdg = getenv("XDG_CONFIG_HOME");
	const char* home = getenv("HOME");
	if (xdg && *xdg) return std::string(xdg) + "/bhoptools";
	return home ? std::string(home) + "/.config/bhoptools" : std::string();
#endif
}

namespace {

std::string Path()
{
	const std::string dir = Dir();
	return dir.empty() ? dir : dir + "/settings.cfg";
}

void Save()
{
	const std::string dir = Dir();
	if (dir.empty()) return;
#ifdef _WIN32
	_mkdir(dir.c_str());
#else
	mkdir(dir.c_str(), 0755);
#endif
	// Written aside and renamed over, so a crash mid-write keeps the old file.
	const std::string path = Path(), tmp = path + ".tmp";
	FILE* f = fopen(tmp.c_str(), "w");
	if (!f) return;
	for (const Field& fl : kFields)
		if (strcmp(fl.key, "pingfix") && (fl.value != &Settings::rawinput || g_rawinputSaved))
			fprintf(f, "%s=%d\n", fl.key, g_settings.*fl.value ? 1 : 0);
	const bool ok = fclose(f) == 0;
#ifdef _WIN32
	if (ok) remove(path.c_str());
#endif
	if (!ok || rename(tmp.c_str(), path.c_str()) != 0) {
		remove(tmp.c_str());
		CoreLog("settings: couldn't write %s", path.c_str());
	}
}

}

Settings& Get() { return g_settings; }

void Load()
{
	const std::string path = Path();
	FILE* f = path.empty() ? nullptr : fopen(path.c_str(), "r");
	if (!f) return;
	char line[128];
	while (fgets(line, sizeof(line), f)) {
		char* eq = strchr(line, '=');
		if (!eq) continue;
		*eq = 0;
		for (const Field& fl : kFields)
			if (!strcmp(line, fl.key)) {
				g_settings.*fl.value = atoi(eq + 1) != 0;
				if (fl.value == &Settings::rawinput) g_rawinputSaved = true;
			}
	}
	fclose(f);
	g_lastRawinput = g_settings.rawinput;
}

void Changed()
{
	if (g_settings.rawinput != g_lastRawinput) g_rawinputSaved = true;
	g_lastRawinput = g_settings.rawinput;
	if (g_apply) g_apply();
	ApplyRawInput(false);
	Save();
}

void ApplyRawInput(bool levelLoad)
{
	void* cvar = CreateInterfaceIn(VSTDLIB_MODULE, "VEngineCvar004");
	void* engine = CreateInterfaceIn(ENGINE_MODULE, "VEngineClient014");
	char* var = cvar ? VFunc<char* (*)(void*, const char*)>(cvar, VT_Cvar_FindBase)(cvar, "m_rawinput") : nullptr;
	if (!var || !engine || strcmp(*reinterpret_cast<const char* const*>(var + 0x18), "m_rawinput")) return;  // m_pszName
	char* parent = *reinterpret_cast<char**>(var + OFF_ConVar_Parent);
	const int value = *reinterpret_cast<const int*>((parent ? parent : var) + OFF_ConVar_IntValue);
	if (!g_rawinputSaved) {
		if (!levelLoad) return;
		g_rawinputSaved = true;
		g_settings.rawinput = g_lastRawinput = value != 0;
		Save();
	}
	if ((value != 0) == g_settings.rawinput) return;
	void* fn = (*reinterpret_cast<void***>(engine))[VT_IVEngineClient_ClientCmdUnrestricted];
	if (!InModuleCode(ENGINE_MODULE, fn)) return;
	reinterpret_cast<void (*)(void*, const char*)>(fn)(engine, g_settings.rawinput ? "m_rawinput 1\n" : "m_rawinput 0\n");
}

void SetApply(std::function<void()> fn) { g_apply = fn; }

}
