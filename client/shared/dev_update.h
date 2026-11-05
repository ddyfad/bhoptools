// bhoptools-dev (BHOPTOOLS_DEV, set by the staging workflow's dev build): updates come from the
// dev-<n> pre-releases of the private ddyfad/bhoptools-staging, fetched with a token the player
// saves in the dev settings folder. See STAGING.md.
#pragma once
#ifdef BHOPTOOLS_DEV

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#endif
#include "utils.h"
#include "sigs.h"
#include "game_layout.h"
#include "release_sig.h"
#include "settings.h"
#include "update_json.h"
#include "version.h"

#define DEV_API "https://api.github.com/repos/ddyfad/bhoptools-staging/releases"

namespace dev {

// GETs an api.github.com URL with the token into `file`. With `asset` it asks for the release
// asset itself, and follows the redirect to its storage host without the token. Per platform.
bool Download(const std::string& url, bool asset, const std::string& file);

// "dev-48" from "dev-48 (abc1234)"; a local build is "dev".
inline std::string CurrentTag()
{
	const std::string v = APP_VERSION;
	return v.substr(0, v.find(' '));
}

inline std::string TokenPath()
{
	const std::string dir = settings::Dir();
	return dir.empty() ? dir : dir + "/github_token.txt";
}

// "" when there's no token file. Only token characters are kept, so a newline or BOM doesn't matter.
inline std::string Token()
{
	const std::string path = TokenPath();
	FILE* f = path.empty() ? nullptr : fopen(path.c_str(), "rb");
	if (!f) return "";
	char buf[256];
	const size_t n = fread(buf, 1, sizeof(buf), f);
	fclose(f);
	std::string token;
	for (size_t i = 0; i < n; i++)
		if ((buf[i] >= '0' && buf[i] <= '9') || (buf[i] >= 'a' && buf[i] <= 'z') || (buf[i] >= 'A' && buf[i] <= 'Z') || buf[i] == '_')
			token += buf[i];
	return token;
}

inline void MakeDir()
{
#ifdef _WIN32
	_mkdir(settings::Dir().c_str());
#else
	mkdir(settings::Dir().c_str(), 0755);
#endif
}

// A one-line file in the dev settings folder: "" when it's missing, removed when written empty.
inline std::string ReadState(const char* name)
{
	std::string text;
	if (FILE* f = fopen((settings::Dir() + "/" + name).c_str(), "r")) {
		char line[64] = {};
		if (fgets(line, sizeof(line), f)) text = line;
		fclose(f);
	}
	text.erase(text.find_last_not_of(" \r\n") + 1);
	return text;
}

inline bool WriteState(const char* name, const std::string& text)
{
	const std::string path = settings::Dir() + "/" + name;
	if (text.empty()) return remove(path.c_str()) == 0;
	MakeDir();
	FILE* f = fopen(path.c_str(), "w");
	if (!f) return false;
	const bool ok = fputs(text.c_str(), f) >= 0;
	return fclose(f) == 0 && ok;
}

// What bhoptools_install leaves for the launcher's next start (Windows).
const char kInstallFile[] = "install_tag.txt";
// The build bhoptools_install put in: while it's the one running, newer builds aren't offered.
const char kHeldFile[] = "held_tag.txt";

inline bool Held() { return ReadState(kHeldFile) == CurrentTag(); }

inline std::string ReadAll(const std::string& path)
{
	std::string data;
	if (FILE* f = fopen(path.c_str(), "rb")) {
		char chunk[4096];
		size_t n;
		while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) data.append(chunk, n);
		fclose(f);
	}
	return data;
}

// Downloads `url` (a release list or one release) and reads it. Empty on failure.
inline std::string FetchJson(const std::string& url, const char* name)
{
	const std::string dir = settings::Dir();
	if (dir.empty()) return "";
	MakeDir();
	const std::string file = dir + "/" + name;
	if (!Download(url, false, file)) return "";
	const std::string json = ReadAll(file);
	remove(file.c_str());
	return json;
}

struct Build {
	std::string tag, notes;
	size_t from, to;  // its part of the JSON
};

// The dev-<n> releases in a /releases list (newest first) or a single release.
inline std::vector<Build> Builds(const std::string& json)
{
	std::vector<Build> out;
	size_t pos = 0;
	for (;;) {
		Build b;
		b.tag = JsonString(json, "tag_name", &pos);
		if (pos == std::string::npos) break;
		b.from = pos;
		b.to = json.find("\"tag_name\"", pos);
		if (b.to == std::string::npos) b.to = json.size();
		size_t body = pos;
		b.notes = JsonString(json, "body", &body);
		if (body == std::string::npos || body >= b.to) b.notes.clear();
		b.notes.erase(b.notes.find_last_not_of(" \n") + 1);
		if (b.tag.compare(0, 4, "dev-") == 0) out.push_back(b);
		pos = b.to;
	}
	return out;
}

inline const Build* Find(const std::vector<Build>& builds, const std::string& tag)
{
	for (const Build& b : builds)
		if (b.tag == tag) return &b;
	return nullptr;
}

struct Asset {
	std::string url;     // the API URL: a private repo's assets only come through it
	long long size = -1;
	std::string sha256;  // GitHub's digest, empty for assets uploaded before it kept them
};

// Asset `name` of the build; false if it has none. An asset's url comes just before its name,
// its size and digest after it and before its browser_download_url.
inline bool FindAsset(const std::string& json, const Build& b, const char* name, Asset& out)
{
	for (size_t pos = b.from;;) {
		const std::string n = JsonString(json, "name", &pos);
		if (pos == std::string::npos || pos >= b.to) return false;
		if (n != name) continue;
		size_t url = json.rfind("\"url\"", pos);
		if (url == std::string::npos || url < b.from) return false;
		out = Asset();
		out.url = JsonString(json, "url", &url);
		size_t end = json.find("\"browser_download_url\"", pos);
		if (end == std::string::npos || end > b.to) end = b.to;
		const size_t size = json.find("\"size\"", pos);
		if (size < end) {
			const size_t colon = json.find(':', size);
			if (colon < end) out.size = strtoll(json.c_str() + colon + 1, nullptr, 10);
		}
		size_t at = pos;
		const std::string digest = JsonString(json, "digest", &at);
		if (at < end && digest.compare(0, 7, "sha256:") == 0) out.sha256 = digest.substr(7);
		return !out.url.empty();
	}
}

// Checks a downloaded asset against the size and digest the API listed. Dev builds aren't
// signed: this catches a broken or swapped download, not someone who can publish dev releases.
inline bool CheckAsset(const Asset& a, const std::string& data, std::string& err)
{
	if (a.size >= 0 && (long long)data.size() != a.size) {
		err = "the download is " + std::to_string(data.size()) + " bytes, the release lists " + std::to_string(a.size);
		return false;
	}
	if (!a.sha256.empty() && relsig::Sha256Hex(data) != a.sha256) {
		err = "the download's sha256 isn't the one the release lists";
		return false;
	}
	return true;
}

// The newest build, or "" when it's the one running; `log` gets the notes of every newer one.
inline std::string Changelog(const std::vector<Build>& builds, std::string& log)
{
	const std::string current = CurrentTag();
	std::string latest;
	for (const Build& b : builds) {
		if (b.tag == current) break;
		if (latest.empty()) latest = b.tag;
		log += b.tag + ":\n" + (b.notes.empty() ? "(no notes)" : b.notes) + "\n\n";
	}
	log.erase(log.find_last_not_of('\n') + 1);
	if (log.size() > 1500)
		log = log.substr(0, 1500) + "...";
	return latest;
}

// Game side: CoreLog goes to the console.
inline bool CheckToken()
{
	if (!Token().empty()) return true;
	CoreLog("no GitHub token: save a read-only token for ddyfad/bhoptools-staging in %s (STAGING.md says how)", TokenPath().c_str());
	return false;
}

inline void PrintBuilds()
{
	if (!CheckToken()) return;
	const std::string json = FetchJson(DEV_API "?per_page=20", "builds.json");
	const std::vector<Build> builds = Builds(json);
	if (builds.empty()) {
		CoreLog("%s", json.empty() ? "couldn't fetch the dev builds (check the token)" : "no dev builds");
		return;
	}
	const std::string current = CurrentTag();
	for (const Build& b : builds) {
		CoreLog("%s%s", b.tag.c_str(), b.tag == current ? "  (running)" : "");
		for (size_t at = 0; at < b.notes.size();) {
			size_t end = b.notes.find('\n', at);
			if (end == std::string::npos) end = b.notes.size();
			if (end > at) CoreLog("    %s", b.notes.substr(at, end - at).c_str());
			at = end + 1;
		}
	}
	CoreLog("bhoptools_install <tag> installs one of these");
}

inline void RegisterCommands(void (*install)(const CCommandX&), void (*builds)(const CCommandX&))
{
	static bool done;
	if (done) return;
	done = true;
	static ConCommandX cmds[2];
	void* cvar = CreateInterfaceIn(VSTDLIB_MODULE, "VEngineCvar004");
	void* echo = cvar ? VFunc<void* (*)(void*, const char*)>(cvar, VT_Cvar_FindBase)(cvar, "echo") : nullptr;
	const char* echoName = echo ? static_cast<ConCommandX*>(echo)->name : nullptr;
	if (!echo || !VFunc<bool (*)(void*)>(echo, VT_CCB_IsCommand)(echo) || !echoName || strcmp(echoName, "echo")) {
		CoreLog("dev: couldn't register bhoptools_install and bhoptools_builds");
		return;
	}
	const struct { const char* name; const char* help; void (*fn)(const CCommandX&); } defs[2] = {
		{ "bhoptools_install", "bhoptools_install <dev-n>: install that dev build, e.g. to roll back.", install },
		{ "bhoptools_builds", "List the last 20 dev builds and their notes.", builds },
	};
	for (int i = 0; i < 2; ++i) {
		ConCommandX& c = cmds[i];
		c.vtable = *reinterpret_cast<void**>(echo);
		c.name = defs[i].name;
		c.help = defs[i].help;
		c.callback = reinterpret_cast<void*>(defs[i].fn);
		c.bits = 0x02;  // new-style callback taking const CCommand&
		VFunc<void (*)(void*, void*)>(cvar, VT_Cvar_Register)(cvar, &c);
	}
}

}

#endif
