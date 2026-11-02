#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <ctime>
#include <unistd.h>
#include <dlfcn.h>
#include <sys/mman.h>

#define CLIENT_MODULE  "client.so"
#define ENGINE_MODULE  "engine.so"
#define VSTDLIB_MODULE "libvstdlib.so"
#define SURFACE_MODULE "vguimatsurface.so"
#define GAMEUI_MODULE  "GameUI.so"
#define VGUI_MODULE    "vgui2.so"

void CoreLog(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

typedef void* (*CreateInterfaceFn)(const char* name, int* ret);

struct MemRange { uintptr_t start, end; };

struct ModuleSegment {
	uintptr_t start;
	uintptr_t end;
	std::string path;
};

// "client.so" must not match steamclient.so; "libSDL2" matches libSDL2-2.0.so.0, not libSDL2_image.
inline bool ModuleNameMatches(const char* path, size_t plen, const char* name)
{
	const char* bn = path;
	for (size_t i = 0; i < plen; ++i) if (path[i] == '/') bn = path + i + 1;
	if (*name == '/') ++name;
	const size_t k = strlen(name);
	if ((size_t)(path + plen - bn) < k || memcmp(bn, name, k) != 0) return false;
	const char c = bn + k < path + plen ? bn[k] : 0;
	return c == 0 || c == '.' || c == '-';
}

// Hand-rolled instead of sscanf/strtoul: glibc 2.38 headers redirect those to
// __isoc23_* symbols that older runtimes don't have.
inline const char* ParseHex(const char* s, uintptr_t* out)
{
	uintptr_t v = 0;
	for (;; ++s) {
		const char c = *s;
		int d;
		if (c >= '0' && c <= '9') d = c - '0';
		else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
		else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
		else break;
		v = (v << 4) | (uintptr_t)d;
	}
	*out = v;
	return s;
}

inline const char* SkipField(const char* s)
{
	while (*s && *s != ' ' && *s != '\t' && *s != '\n') ++s;
	while (*s == ' ' || *s == '\t') ++s;
	return s;
}

inline bool ParseMapsLine(const char* line, uintptr_t* s, uintptr_t* e, char perms[4], const char** path, size_t* plen)
{
	const char* p = ParseHex(line, s);
	if (*p != '-') return false;
	p = ParseHex(p + 1, e);
	if (*p != ' ') return false;
	++p;
	for (int i = 0; i < 4; ++i) {
		if (!p[i]) return false;
		perms[i] = p[i];
	}
	for (int i = 0; i < 4; ++i) p = SkipField(p);
	size_t n = strlen(p);
	while (n && (p[n-1] == '\n' || p[n-1] == '\r' || p[n-1] == ' ')) --n;
	*path = p;
	*plen = n;
	return true;
}

// The anonymous rw-p mapping directly after a module's last file-backed
// segment is its .bss tail; report it under the module's path.
inline std::vector<ModuleSegment> FindSegments(const char* name, char need_perm)
{
	std::vector<ModuleSegment> out;
	FILE* f = fopen("/proc/self/maps", "r");
	if (!f) return out;

	char line[1024];
	uintptr_t module_end = 0;
	std::string module_path;
	while (fgets(line, sizeof(line), f)) {
		uintptr_t s, e;
		char perms[4];
		const char* path;
		size_t plen;
		if (!ParseMapsLine(line, &s, &e, perms, &path, &plen)) continue;

		bool is_module = plen && ModuleNameMatches(path, plen, name);
		bool is_bss_tail = !plen && module_end && s == module_end && perms[1] == 'w'
			&& (e - s) <= (16u << 20);
		std::string p;
		if (is_module) {
			module_end = e;
			module_path.assign(path, plen);
			p = module_path;
		} else if (is_bss_tail) {
			p = module_path;
			module_end = 0;
		} else {
			module_end = 0;
			continue;
		}

		if (perms[0] != 'r') continue;
		if (need_perm == 'x' && perms[2] != 'x') continue;
		if (need_perm == 'w' && perms[1] != 'w') continue;
		out.push_back({s, e, std::move(p)});
	}
	fclose(f);
	return out;
}

inline std::vector<ModuleSegment> FindExecSegments(const char* name)
{
	return FindSegments(name, 'x');
}

struct ParsedSig {
	std::vector<uint8_t> bytes;
	std::vector<uint8_t> mask; // 1 = compare, 0 = wildcard
};

inline ParsedSig ParseSig(const char* sig)
{
	ParsedSig p;
	const char* s = sig;
	while (*s) {
		while (*s == ' ' || *s == '\t') ++s;
		if (!*s) break;
		if (*s == '?') {
			p.bytes.push_back(0);
			p.mask.push_back(0);
			++s;
			if (*s == '?') ++s;
		} else {
			char buf[3] = { s[0], s[1], 0 };
			uintptr_t v;
			ParseHex(buf, &v);
			p.bytes.push_back((uint8_t)v);
			p.mask.push_back(1);
			s += s[1] ? 2 : 1;
		}
	}
	return p;
}

inline uintptr_t ScanRange(uintptr_t start, uintptr_t end, const ParsedSig& p)
{
	const size_t n = p.bytes.size();
	if (!n || end <= start || (end - start) < n) return 0;

	size_t k = 0;
	while (k < n && !p.mask[k]) ++k;
	if (k == n) return start;

	const uint8_t* base = (const uint8_t*)start;
	const uint8_t* last = base + (end - start) - n;
	const uint8_t* cur = base;
	while (cur <= last) {
		const uint8_t* hit = (const uint8_t*)memchr(cur + k, p.bytes[k], (size_t)(last - cur) + 1);
		if (!hit) return 0;
		cur = hit - k;
		size_t j = 0;
		for (; j < n; ++j) {
			if (p.mask[j] && cur[j] != p.bytes[j]) break;
		}
		if (j == n) return (uintptr_t)cur;
		++cur;
	}
	return 0;
}

inline uintptr_t FindPatternIn(const char* module, const char* sig)
{
	auto segs = FindExecSegments(module);
	if (segs.empty()) return 0;
	ParsedSig p = ParseSig(sig);
	for (const auto& s : segs) {
		uintptr_t hit = ScanRange(s.start, s.end, p);
		if (hit) return hit;
	}
	return 0;
}

// Source dlopens modules RTLD_LOCAL, so go through a NOLOAD handle.
inline CreateInterfaceFn InterfaceFactory(const char* module)
{
	auto segs = FindExecSegments(module);
	void* h = segs.empty() ? nullptr : dlopen(segs[0].path.c_str(), RTLD_LAZY | RTLD_NOLOAD);
	CreateInterfaceFn create = h ? (CreateInterfaceFn)dlsym(h, "CreateInterface") : nullptr;
	if (h) dlclose(h);
	return create;
}

inline void* CreateInterfaceIn(const char* module, const char* name)
{
	CreateInterfaceFn create = InterfaceFactory(module);
	return create ? create(name, nullptr) : nullptr;
}

inline std::vector<MemRange> ModuleDataRanges(const char* module)
{
	const std::vector<ModuleSegment> exec = FindExecSegments(module);
	std::vector<MemRange> out;
	for (auto& s : FindSegments(module, 'r')) {
		bool isExec = false;
		for (auto& x : exec) isExec |= x.start == s.start;
		if (!isExec) out.push_back({ s.start, s.end });
	}
	return out;
}

// A class's primary vtable through its RTTI: the type name string, the typeinfo pointing at it, and
// the vtable whose typeinfo slot points at that with offset-to-top 0. `name` is the mangled
// name, "22COptionsSubMultiplayer" or "N4vgui13PropertySheetE".
inline void** FindVTable(const char* module, const char* name)
{
	const std::vector<MemRange> data = ModuleDataRanges(module);
	const size_t len = strlen(name) + 1;
	uintptr_t str = 0;
	for (auto& r : data)
		for (uintptr_t p = r.start; !str && p + len <= r.end; ++p)
			if (*(const char*)p == name[0] && !memcmp((const void*)p, name, len) && *(const char*)(p - 1) == 0) str = p;
	if (!str) return nullptr;
	uintptr_t info = 0;
	for (auto& r : data)
		for (uintptr_t p = r.start + 8; !info && p + 8 <= r.end; p += 8)
			if (*(const uintptr_t*)p == str) info = p - 8;
	if (!info) return nullptr;
	for (auto& r : data)
		for (uintptr_t p = r.start + 8; p + 16 <= r.end; p += 8)
			if (*(const uintptr_t*)p == info && *(const intptr_t*)(p - 8) == 0) return (void**)(p + 8);
	return nullptr;
}

// Whether a code pointer lies in the module, to tell a slot someone else has hooked.
inline bool InModuleCode(const char* module, const void* p)
{
	for (auto& s : FindExecSegments(module))
		if ((uintptr_t)p >= s.start && (uintptr_t)p < s.end) return true;
	return false;
}

inline unsigned long long MonotonicMs()
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (unsigned long long)ts.tv_sec * 1000ULL + (unsigned long long)ts.tv_nsec / 1000000ULL;
}

// The options slider polls it while it's dragged.
bool LeftMouseDown();

inline double MonotonicSeconds()
{
	timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

// RELRO can end mid-page, so restore the page's old protection rather than assuming read-only.
inline int PageProt(uintptr_t addr)
{
	int prot = PROT_READ;
	FILE* f = fopen("/proc/self/maps", "r");
	if (!f) return prot;
	char line[1024];
	while (fgets(line, sizeof(line), f)) {
		uintptr_t s, e;
		char perms[4];
		const char* path;
		size_t plen;
		if (!ParseMapsLine(line, &s, &e, perms, &path, &plen)) continue;
		if (addr < s || addr >= e) continue;
		prot = (perms[0] == 'r' ? PROT_READ : 0) | (perms[1] == 'w' ? PROT_WRITE : 0) | (perms[2] == 'x' ? PROT_EXEC : 0);
		break;
	}
	fclose(f);
	return prot;
}

inline bool mprotect_range(uintptr_t start, size_t len, int prot)
{
	const long page_size = sysconf(_SC_PAGESIZE);
	uintptr_t page_start = start & ~(uintptr_t)(page_size - 1);
	size_t total = (start + len) - page_start;
	size_t pad = total % page_size;
	if (pad) total += (page_size - pad);
	return mprotect((void*)page_start, total, prot) == 0;
}

// Returns the old slot value, or nullptr if the page couldn't be made writable.
inline void* PatchPointer(void** slot, void* value)
{
	const uintptr_t addr = (uintptr_t)slot;
	const int prot = PageProt(addr);
	if (!(prot & PROT_WRITE) && !mprotect_range(addr, sizeof(void*), prot | PROT_WRITE)) return nullptr;
	void* old = __atomic_exchange_n(slot, value, __ATOMIC_ACQ_REL);
	if (!(prot & PROT_WRITE)) mprotect_range(addr, sizeof(void*), prot);
	return old;
}
