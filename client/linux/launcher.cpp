// Prints the Steam launch options for the preload; Steam owns launching the game on Linux.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <climits>
#include <unistd.h>
#include <libgen.h>
#include <sys/stat.h>
#include <pwd.h>

#ifdef BHOPTOOLS_DEV
static const char* PRELOAD_NAME = "bhoptools-dev.so";
#else
static const char* PRELOAD_NAME = "bhoptools.so";
#endif

static bool path_exists(const char* p)
{
	struct stat st;
	return stat(p, &st) == 0;
}

static const char* home_dir()
{
	const char* h = getenv("HOME");
	if (h && *h) return h;
	struct passwd* pw = getpwuid(getuid());
	return pw ? pw->pw_dir : nullptr;
}

// Only used to sanity-check the install before printing the launch string.
static bool find_css_install(char* out, size_t out_len)
{
	const char* h = home_dir();
	if (!h) return false;

	char vdf[PATH_MAX];
	snprintf(vdf, sizeof(vdf), "%s/.local/share/Steam/steamapps/libraryfolders.vdf", h);
	if (!path_exists(vdf)) {
		snprintf(vdf, sizeof(vdf), "%s/.steam/steam/steamapps/libraryfolders.vdf", h);
		if (!path_exists(vdf)) return false;
	}

	FILE* f = fopen(vdf, "r");
	if (!f) return false;

	char line[1024];
	while (fgets(line, sizeof(line), f)) {
		const char* k = strstr(line, "\"path\"");
		if (!k) continue;
		const char* q1 = strchr(k + 6, '"');
		if (!q1) continue;
		const char* q2 = strchr(q1 + 1, '"');
		if (!q2) continue;
		size_t n = (size_t)(q2 - q1 - 1);
		if (n == 0 || n >= PATH_MAX - 64) continue;
		char libpath[PATH_MAX];
		memcpy(libpath, q1 + 1, n);
		libpath[n] = 0;

		char candidate[PATH_MAX];
		const int len = snprintf(candidate, sizeof(candidate), "%s/steamapps/common/Counter-Strike Source", libpath);
		if (len > 0 && (size_t)len < out_len && path_exists(candidate)) {
			memcpy(out, candidate, (size_t)len + 1);
			fclose(f);
			return true;
		}
	}
	fclose(f);
	return false;
}

int main()
{
	char exepath[PATH_MAX];
	ssize_t n = readlink("/proc/self/exe", exepath, sizeof(exepath) - 1);
	if (n <= 0) {
		fprintf(stderr, "could not resolve own path via /proc/self/exe\n");
		return 1;
	}
	exepath[n] = 0;

	char dir_buf[PATH_MAX];
	snprintf(dir_buf, sizeof(dir_buf), "%s", exepath);
	const char* dir = dirname(dir_buf);

	char so_path[PATH_MAX];
	snprintf(so_path, sizeof(so_path), "%s/%s", dir, PRELOAD_NAME);

#ifdef BHOPTOOLS_DEV
	printf("bhoptools dev -- Linux\n");
	printf("=========================\n\n");
	// Same folder as settings::Dir().
	char token[PATH_MAX];
	const char* xdg = getenv("XDG_CONFIG_HOME");
	const char* h = home_dir();
	if (xdg && *xdg) snprintf(token, sizeof(token), "%s/bhoptools-dev/github_token.txt", xdg);
	else snprintf(token, sizeof(token), "%s/.config/bhoptools-dev/github_token.txt", h ? h : "~");
	if (!path_exists(token)) {
		printf("No GitHub token, so no dev build updates. Save a read-only token for\n");
		printf("ddyfad/bhoptools-staging in\n  %s\n(STAGING.md says how).\n\n", token);
	}
#else
	printf("bhoptools -- Linux\n");
	printf("=========================\n\n");
#endif
	printf("m_rawinput 2 style mouse interpolation (Options > Fixes, sets m_rawinput)\n");
	printf("F8 toggles all client-side prediction (on by default)\n");
	printf("checkpoints and noclip predicted on servers running the bhoptools plugin\n");
	printf("download progress shown in the loading bar\n");
	printf("missing / differing maps fetched from fastdl.me\n\n");

	if (!path_exists(so_path)) {
		printf("WARNING: preload .so not found at:\n  %s\n\n", so_path);
		printf("Build it with `make` first.\n\n");
	}

	char css[PATH_MAX];
	if (find_css_install(css, sizeof(css))) {
		printf("Detected CS:S install:\n  %s\n\n", css);
	} else {
		printf("Could not locate Counter-Strike: Source via Steam libraryfolders.vdf.\n");
		printf("(Not fatal -- you just need CS:S installed somewhere Steam knows about.)\n\n");
	}

	// LD_PRELOAD splits on spaces and colons, so such a path can't be preloaded.
	if (strpbrk(so_path, " :")) {
		printf("WARNING: the path contains a space or ':', which LD_PRELOAD can't handle.\n");
		printf("Move the .so to a path without them (e.g. ~/.local/lib/) and use that.\n\n");
	}

	printf("In Steam, right-click Counter-Strike: Source -> Properties -> Launch Options,\n");
	printf("and paste the following (all one line):\n\n");
	// Appending $LD_PRELOAD keeps the Steam overlay, which Steam preloads the same way.
	printf("    LD_PRELOAD=\"%s:$LD_PRELOAD\" %%command%% -insecure\n\n", so_path);

	printf("VAC WARNING:\n");
	printf("  The preload does nothing unless the game runs with -insecure, so keep\n");
	printf("  it in the launch options. Only join servers the in-game browser shows\n");
	printf("  as insecure.\n\n");

	printf("Notes:\n");
	printf("  * Mouse interpolation in Options > bhoptools sets m_rawinput 1 when on and 0 when off;\n");
	printf("    off, the engine's own mouse path is used unchanged.\n");
	printf("    Remove `LD_PRELOAD=...` from your launch options to disable.\n");
	printf("  * Messages start with [bhoptools], in the game console and on stderr.\n");
	printf("  * Checkpoint binds: bind mouse4 sm_save; bind mouse5 sm_tele.\n");
	printf("  * Having problems? bhoptools_log in the console, play until it happens, bhoptools_log again,\n");
	printf("    and send cstrike/bhoptools_log.txt.\n");

	return 0;
}
