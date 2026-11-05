#pragma once

#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "bsp_lzma.h"
#include "vscript_sim.h"

#ifdef _MSC_VER
#define strcasecmp  _stricmp
#define strncasecmp _strnicmp
#endif

#define BSP_LUMP_ENTITIES    0
#define BSP_LUMP_PLANES      1
#define BSP_LUMP_NODES       5
#define BSP_LUMP_LEAFS       10
#define BSP_LUMP_LEAFBRUSHES 17
#define BSP_LUMP_BRUSHES     18
#define BSP_LUMP_BRUSHSIDES  19
#define BSP_LUMP_MODELS      14
#define BSP_LUMP_PAKFILE     40

struct BspLump {
	int32_t fileofs;
	int32_t filelen;
	int32_t version;
	char    fourCC[4];
};

struct BspHeader {
	int32_t ident;
	int32_t version;
	BspLump lumps[64];
	int32_t mapRevision;
};

struct BspPlane {
	float   normal[3];
	float   dist;
	int32_t type;
};

struct BspNode {
	int32_t  planenum;
	int32_t  children[2];
	int16_t  mins[3];
	int16_t  maxs[3];
	uint16_t firstface;
	uint16_t numfaces;
	int16_t  area;
	int16_t  padding;
};

struct BspBrush {
	int32_t firstside;
	int32_t numsides;
	int32_t contents;
};

struct BspBrushSide {
	uint16_t planenum;
	int16_t  texinfo;
	int16_t  dispinfo;
	int16_t  bevel;
};

struct BspModel {
	float   mins[3];
	float   maxs[3];
	float   origin[3];
	int32_t headnode;
	int32_t firstface;
	int32_t numfaces;
};

struct BspLeafCore {
	int32_t  contents;
	int16_t  cluster;
	int16_t  areaFlags;
	int16_t  mins[3];
	int16_t  maxs[3];
	uint16_t firstleafface;
	uint16_t numleaffaces;
	uint16_t firstleafbrush;
	uint16_t numleafbrushes;
};

static_assert(sizeof(BspLump)      == 16, "BspLump layout");
static_assert(sizeof(BspHeader)    == 1036, "BspHeader layout");
static_assert(sizeof(BspPlane)     == 20, "BspPlane layout");
static_assert(sizeof(BspNode)      == 32, "BspNode layout");
static_assert(sizeof(BspBrush)     == 12, "BspBrush layout");
static_assert(sizeof(BspBrushSide) == 8,  "BspBrushSide layout");
static_assert(sizeof(BspModel)     == 48, "BspModel layout");
static_assert(sizeof(BspLeafCore)  == 28, "BspLeafCore layout");

struct PredKv {
	std::string key;
	std::string value;
};

struct PredMapEntity {
	std::vector<PredKv> kv;

	const char* Get(const char* key, const char* def = "") const
	{
		for (const auto& p : kv)
			if (strcasecmp(p.key.c_str(), key) == 0) return p.value.c_str();
		return def;
	}
};

struct PredBrushSpan {
	int   firstPlane;
	int   planeCount;
	int   contents;
	bool  isBox; // six axial sides: the engine tests these as boxes
	float boxMins[3];
	float boxMaxs[3];
};

struct PredXform {
	float origin[3];
	bool  rotated;
	float m[3][4];
};

// SinCos is fsincos on POSIX (mathlib.h); double sin/cos rounds the same way.
static void PredSinCos(float degrees, float* s, float* c)
{
	const float rad = degrees * (float)((float)M_PI / 180.f);
	*s = (float)sin((double)rad);
	*c = (float)cos((double)rad);
}

static void PredAngleMatrix(const float angles[3], const float origin[3], PredXform& x)
{
	float sr, sp, sy, cr, cp, cy;
	PredSinCos(angles[1], &sy, &cy);
	PredSinCos(angles[0], &sp, &cp);
	PredSinCos(angles[2], &sr, &cr);
	x.m[0][0] = cp * cy;
	x.m[1][0] = cp * sy;
	x.m[2][0] = -sp;
	const float crcy = cr * cy, crsy = cr * sy, srcy = sr * cy, srsy = sr * sy;
	x.m[0][1] = sp * srcy - crsy;
	x.m[1][1] = sp * srsy + crcy;
	x.m[2][1] = sr * cp;
	x.m[0][2] = sp * crcy + srsy;
	x.m[1][2] = sp * crsy - srcy;
	x.m[2][2] = cr * cp;
	for (int i = 0; i < 3; ++i) { x.m[i][3] = origin[i]; x.origin[i] = origin[i]; }
	x.rotated = angles[0] != 0.0f || angles[1] != 0.0f || angles[2] != 0.0f;
}

struct PredSimEnt;
struct PredAction;
struct PredFilter;

struct PredGrid {
	float cell = 512.0f;
	int   x0 = 0, y0 = 0, nx = 0, ny = 0;
	std::vector<int> cellStart;
	std::vector<int> items;
	std::vector<int> always;
};

struct PredMpbhopsDoor {
	int       modelIndex = -1;
	PredXform xf = {};
	float     speed = 0.0f;
	bool      boost = false;
	int       tele = -1;
};

struct PredScriptFile {
	enum State : uint8_t { MISSING, LOADED, UNREADABLE };
	std::string name;
	std::string text;
	uint8_t     state = MISSING;
};

struct PredMap {
	std::vector<PredMapEntity> entities;
	// bhoptools_mapdiff: the BSP's own entities while the server's diff replaces them (mapdiff.h).
	std::vector<PredMapEntity> bspEntities;
	uint32_t entityCrc = 0; // of the entity lump as stored, before any decompression
	bool     diffed = false;
	int      diffHash = 0;
	std::vector<PredScriptFile> scriptFiles;
	std::vector<vs::Program>   vsPrograms;
	std::vector<BspPlane>      brushPlanes;
	std::vector<PredBrushSpan> brushes;
	std::vector<BspModel>      models;

	std::vector<std::pair<int, int>> modelBrushes;
	std::vector<PredSimEnt>    sim;
	std::unordered_map<int, int> simByHammerId; // -1 where two entities share one
	std::vector<PredAction>    actions;
	std::vector<PredFilter>    filters;
	std::vector<PredMpbhopsDoor> mpbhopsDoors;
	std::vector<int>             doors; // sim indices of func_door and func_door_rotating
	std::vector<int>             buttons; // sim indices of damage-activated func_button
	PredGrid                   grid;
	struct {
		int active = 0;
		int vsScripts = 0, vsMissing = 0, vsUnreadable = 0, vsFunctions = 0, vsUnsupported = 0;
		int vsPredicted = 0, vsSkipped = 0;
	} stats;
	bool loaded = false;
};

// zlib's CRC-32, as the bhoptools plugin takes it of the entity lump and of its map diff.
static uint32_t PredCrc32(const void* data, size_t len)
{
	const uint8_t* p = (const uint8_t*)data;
	uint32_t crc = 0xFFFFFFFFu;
	for (size_t i = 0; i < len; ++i) {
		crc ^= p[i];
		for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
	}
	return ~crc;
}

static bool PredReadLump(FILE* f, long fileSize, const BspHeader& h, int lump, std::vector<uint8_t>& out,
	uint32_t* crc = nullptr)
{
	const BspLump& l = h.lumps[lump];
	if (l.fileofs <= 0 || l.filelen <= 0) return false;
	// Otherwise a bogus header has us zero-fill up to 2GB.
	if ((int64_t)l.fileofs + l.filelen > fileSize) return false;
	out.resize((size_t)l.filelen);
	if (fseek(f, l.fileofs, SEEK_SET) != 0) return false;
	if (fread(out.data(), 1, (size_t)l.filelen, f) != (size_t)l.filelen) return false;
	if (crc) *crc = PredCrc32(out.data(), out.size());
	if (out.size() >= 17 && memcmp(out.data(), "LZMA", 4) == 0) {
		std::vector<uint8_t> plain;
		if (!bsplzma::DecodeLump(out.data(), out.size(), plain)) return false;
		out.swap(plain);
	}
	return true;
}

static void PredParseEntityLump(const char* text, size_t len, std::vector<PredMapEntity>& out)
{
	const char* s = text;
	const char* end = text + len;
	PredMapEntity ent;
	bool in_ent = false;
	while (s < end && *s) {
		if (*s == '{') { in_ent = true; ent.kv.clear(); ++s; continue; }
		if (*s == '}') {
			if (in_ent && !ent.kv.empty()) out.push_back(ent);
			in_ent = false;
			++s;
			continue;
		}
		if (*s == '"' && in_ent) {
			const char* k0 = ++s;
			while (s < end && *s != '"') ++s;
			if (s >= end) break;
			std::string key(k0, s - k0);
			++s;
			while (s < end && *s != '"' && *s != '}' && *s != '{') ++s;
			if (s >= end || *s != '"') continue;
			const char* v0 = ++s;
			while (s < end && *s != '"') ++s;
			if (s >= end) break;
			ent.kv.push_back({std::move(key), std::string(v0, s - v0)});
			++s;
			continue;
		}
		++s;
	}
}

static const size_t PRED_MAX_SCRIPT_FILES = 32;

static uint16_t PredRd16(const uint8_t* p) { uint16_t v; memcpy(&v, p, 2); return v; }
static uint32_t PredRd32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }

static bool PredReadAt(FILE* f, int64_t ofs, size_t len, std::vector<uint8_t>& out)
{
	out.resize(len);
	if (len == 0) return true;
	if (ofs < 0 || ofs > 0x7fffffff || fseek(f, (long)ofs, SEEK_SET) != 0) return false;
	return fread(out.data(), 1, len, f) == len;
}

static int PredZipReadEntry(FILE* f, const BspLump& l, uint16_t method, uint32_t csize, uint32_t usize,
	uint32_t lho, std::string& text)
{
	if (usize == 0) return PredScriptFile::MISSING; // the game skips an empty script
	if (usize > vs::kMaxSource || csize > vs::kMaxSource + 64) return PredScriptFile::UNREADABLE;
	std::vector<uint8_t> hdr, data;
	if ((uint64_t)lho + 30 > (uint64_t)l.filelen || !PredReadAt(f, (int64_t)l.fileofs + lho, 30, hdr)
			|| PredRd32(hdr.data()) != 0x04034b50)
		return PredScriptFile::UNREADABLE;
	const uint64_t dataOfs = (uint64_t)lho + 30 + PredRd16(&hdr[26]) + PredRd16(&hdr[28]);
	if (dataOfs + csize > (uint64_t)l.filelen || !PredReadAt(f, (int64_t)l.fileofs + (int64_t)dataOfs, csize, data))
		return PredScriptFile::UNREADABLE;
	std::vector<uint8_t> plain;
	if (method == 0) {
		if (csize != usize) return PredScriptFile::UNREADABLE;
		plain.swap(data);
	} else if (method == 14) {
		// LZMA SDK version (2), properties size (2), properties (5), then the stream.
		if (csize < 9 || PredRd16(&data[2]) != 5) return PredScriptFile::UNREADABLE;
		plain.assign(usize, 0);
		bsplzma::LzmaDecoder dec;
		if (!dec.Decode(&data[4], &data[9], csize - 9, plain.data(), usize)) return PredScriptFile::UNREADABLE;
	} else {
		return PredScriptFile::UNREADABLE;
	}
	size_t n = 0;
	while (n < plain.size() && plain[n]) ++n; // the game reads it as a C string
	text.assign((const char*)plain.data(), n);
	return n ? PredScriptFile::LOADED : PredScriptFile::MISSING;
}

static bool PredPathEq(const std::string& zipName, const std::string& want)
{
	if (zipName.size() != want.size()) return false;
	for (size_t i = 0; i < want.size(); ++i) {
		char a = zipName[i] == '\\' ? '/' : zipName[i], b = want[i];
		if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
		if (b >= 'A' && b <= 'Z') b += 'a' - 'A';
		if (a != b) return false;
	}
	return true;
}

static void PredZipLoadScripts(FILE* f, long fileSize, const BspLump& l, std::vector<PredScriptFile>& files)
{
	if (l.fileofs <= 0 || l.filelen < 22 || (int64_t)l.fileofs + l.filelen > fileSize) return;
	const size_t tailLen = (size_t)l.filelen < 22 + 65535 ? (size_t)l.filelen : 22 + 65535;
	std::vector<uint8_t> tail;
	if (!PredReadAt(f, (int64_t)l.fileofs + l.filelen - (int64_t)tailLen, tailLen, tail)) return;
	size_t eocd = (size_t)-1;
	for (size_t i = tailLen - 22 + 1; i-- > 0; )
		if (PredRd32(&tail[i]) == 0x06054b50) { eocd = i; break; }
	if (eocd == (size_t)-1) return;
	const uint32_t cdCount = PredRd16(&tail[eocd + 10]);
	const uint32_t cdSize = PredRd32(&tail[eocd + 12]);
	const uint32_t cdOfs = PredRd32(&tail[eocd + 16]);
	if (cdSize > (16u << 20) || (uint64_t)cdOfs + cdSize > (uint64_t)l.filelen) return;
	std::vector<uint8_t> cd;
	if (!PredReadAt(f, (int64_t)l.fileofs + cdOfs, cdSize, cd)) return;

	std::vector<std::string> want(files.size());
	for (size_t k = 0; k < files.size(); ++k) want[k] = "scripts/vscripts/" + files[k].name;
	size_t p = 0;
	for (uint32_t n = 0; n < cdCount && p + 46 <= cd.size(); ++n) {
		if (PredRd32(&cd[p]) != 0x02014b50) break;
		const uint16_t flags = PredRd16(&cd[p + 8]);
		const uint16_t method = PredRd16(&cd[p + 10]);
		const uint32_t csize = PredRd32(&cd[p + 20]);
		const uint32_t usize = PredRd32(&cd[p + 24]);
		const size_t nameLen = PredRd16(&cd[p + 28]);
		const size_t extraLen = PredRd16(&cd[p + 30]);
		const size_t commentLen = PredRd16(&cd[p + 32]);
		const uint32_t lho = PredRd32(&cd[p + 42]);
		if (p + 46 + nameLen > cd.size()) break;
		const std::string name((const char*)&cd[p + 46], nameLen);
		p += 46 + nameLen + extraLen + commentLen;
		for (size_t k = 0; k < files.size(); ++k) {
			if (files[k].state != PredScriptFile::MISSING || !PredPathEq(name, want[k])) continue;
			files[k].state = (flags & 1) ? (uint8_t)PredScriptFile::UNREADABLE // encrypted
				: (uint8_t)PredZipReadEntry(f, l, method, csize, usize, lho, files[k].text);
		}
	}
}

static void PredLooseLoadScript(const char* gameDir, PredScriptFile& sf)
{
	static const char* const dirs[2] = { "/scripts/vscripts/", "/download/scripts/vscripts/" };
	for (const char* d : dirs) {
		const std::string path = std::string(gameDir) + d + sf.name;
		FILE* f = fopen(path.c_str(), "rb");
		if (!f) continue;
		long size = -1;
		if (fseek(f, 0, SEEK_END) == 0) size = ftell(f);
		std::vector<uint8_t> buf;
		const bool ok = size >= 0 && (size_t)size <= vs::kMaxSource && PredReadAt(f, 0, (size_t)size, buf);
		fclose(f);
		if (!ok) { sf.state = PredScriptFile::UNREADABLE; return; }
		size_t n = 0;
		while (n < buf.size() && buf[n]) ++n;
		sf.text.assign((const char*)buf.data(), n);
		sf.state = n ? PredScriptFile::LOADED : PredScriptFile::MISSING;
		return;
	}
}

// "vscripts" as CBaseEntity::RunVScripts splits it: a 254-char copy split on spaces,
// ".nut" added when there's no extension, anything else refused.
static void PredSplitVScripts(const char* list, std::vector<std::string>& out)
{
	out.clear();
	std::string s(list);
	if (s.size() > 254) s.resize(254);
	size_t p = 0;
	while (p < s.size()) {
		size_t e = s.find(' ', p);
		if (e == std::string::npos) e = s.size();
		std::string name = s.substr(p, e - p);
		p = e + 1;
		if (name.empty()) continue;
		const size_t dot = name.rfind('.');
		if (dot == std::string::npos) name += ".nut";
		else if (name.compare(dot, std::string::npos, ".nut") != 0) name.clear(); // the game refuses it
		out.push_back(name);
	}
}

static bool PredScriptNameSafe(const std::string& n)
{
	if (n.empty() || n[0] == '/' || n[0] == '\\' || n.find("..") != std::string::npos || n.find(':') != std::string::npos)
		return false;
	for (char c : n)
		if ((unsigned char)c < 0x20) return false;
	return true;
}

static void PredLoadScripts(FILE* f, long fileSize, const BspHeader& h, PredMap& map, const char* gameDir)
{
	std::vector<std::string> names;
	for (const PredMapEntity& e : map.entities) {
		if (strcmp(e.Get("classname"), "logic_script") != 0) continue;
		PredSplitVScripts(e.Get("vscripts"), names);
		for (const std::string& n : names) {
			if (n.empty()) continue;
			bool have = false;
			for (const PredScriptFile& sf : map.scriptFiles) have = have || sf.name == n;
			if (have || map.scriptFiles.size() >= PRED_MAX_SCRIPT_FILES) continue;
			PredScriptFile sf;
			sf.name = n;
			// A name we won't look up can't be told apart from a missing one: treat it as unreadable.
			if (!PredScriptNameSafe(n)) sf.state = PredScriptFile::UNREADABLE;
			map.scriptFiles.push_back(sf);
		}
	}
	if (map.scriptFiles.empty()) return;
	PredZipLoadScripts(f, fileSize, h.lumps[BSP_LUMP_PAKFILE], map.scriptFiles);
	if (gameDir && gameDir[0])
		for (PredScriptFile& sf : map.scriptFiles)
			if (sf.state == PredScriptFile::MISSING && PredScriptNameSafe(sf.name)) PredLooseLoadScript(gameDir, sf);
}

// Iterative with each node visited once: a node that names itself or an ancestor
// as a child would otherwise recurse until the stack runs out.
static void PredCollectBrushes(const std::vector<BspNode>& nodes, const uint8_t* leafs, size_t leafSize,
	size_t leafCount, const std::vector<uint16_t>& leafBrushes, int headnode, std::vector<int>& out,
	std::vector<uint8_t>& seen, std::vector<uint8_t>& nodeSeen)
{
	std::vector<int> stack;
	stack.push_back(headnode);

	while (!stack.empty()) {
		const int nodeIdx = stack.back();
		stack.pop_back();

		if (nodeIdx < 0) {
			size_t leaf = (size_t)(-1 - nodeIdx);
			if (leaf >= leafCount) continue;
			const BspLeafCore* l = (const BspLeafCore*)(leafs + leaf * leafSize);
			for (int i = 0; i < l->numleafbrushes; ++i) {
				size_t lb = (size_t)l->firstleafbrush + i;
				if (lb >= leafBrushes.size()) continue;
				int b = leafBrushes[lb];
				if ((size_t)b >= seen.size() || seen[b]) continue;
				seen[b] = 1;
				out.push_back(b);
			}
			continue;
		}

		if ((size_t)nodeIdx >= nodes.size() || nodeSeen[nodeIdx]) continue;
		nodeSeen[nodeIdx] = 1;

		stack.push_back(nodes[nodeIdx].children[1]);
		stack.push_back(nodes[nodeIdx].children[0]);
	}
}

static bool PredLoadBsp(const char* path, PredMap& map, bool worldBrushes = false, const char* gameDir = nullptr)
{
	FILE* f = fopen(path, "rb");
	if (!f) return false;

	long fileSize = -1;
	if (fseek(f, 0, SEEK_END) == 0) fileSize = ftell(f);

	BspHeader h;
	bool ok = fileSize > 0 && fseek(f, 0, SEEK_SET) == 0
		&& fread(&h, 1, sizeof(h), f) == sizeof(h) && h.ident == 0x50534256
		&& h.version >= 19 && h.version <= 21;

	std::vector<uint8_t> entText, planes, nodes, leafs, leafBrushes16, brushes, sides, models;
	ok = ok && PredReadLump(f, fileSize, h, BSP_LUMP_ENTITIES, entText, &map.entityCrc)
		&& PredReadLump(f, fileSize, h, BSP_LUMP_PLANES, planes)
		&& PredReadLump(f, fileSize, h, BSP_LUMP_NODES, nodes)
		&& PredReadLump(f, fileSize, h, BSP_LUMP_LEAFS, leafs)
		&& PredReadLump(f, fileSize, h, BSP_LUMP_LEAFBRUSHES, leafBrushes16)
		&& PredReadLump(f, fileSize, h, BSP_LUMP_BRUSHES, brushes)
		&& PredReadLump(f, fileSize, h, BSP_LUMP_BRUSHSIDES, sides)
		&& PredReadLump(f, fileSize, h, BSP_LUMP_MODELS, models);
	if (!ok) { fclose(f); return false; }

	PredParseEntityLump((const char*)entText.data(), entText.size(), map.entities);
	PredLoadScripts(f, fileSize, h, map, gameDir);
	fclose(f);

	const BspPlane* plane = (const BspPlane*)planes.data();
	const size_t planeCount = planes.size() / sizeof(BspPlane);
	const BspBrush* brush = (const BspBrush*)brushes.data();
	const size_t brushCount = brushes.size() / sizeof(BspBrush);
	const BspBrushSide* side = (const BspBrushSide*)sides.data();
	const size_t sideCount = sides.size() / sizeof(BspBrushSide);
	const BspModel* model = (const BspModel*)models.data();
	const size_t modelCount = models.size() / sizeof(BspModel);
	// Lump version 0 leafs still carry the 24-byte ambient cube.
	const size_t leafSize = h.lumps[BSP_LUMP_LEAFS].version == 0 ? 56 : 32;
	const size_t leafCount = leafs.size() / leafSize;

	const BspNode* node = (const BspNode*)nodes.data();
	const uint16_t* leafBrush = (const uint16_t*)leafBrushes16.data();
	std::vector<BspNode> nodeVec(node, node + nodes.size() / sizeof(BspNode));
	std::vector<uint16_t> leafBrushVec(leafBrush, leafBrush + leafBrushes16.size() / 2);

	map.models.assign(model, model + modelCount);
	map.modelBrushes.resize(modelCount, {0, 0});
	std::vector<uint8_t> seen, nodeSeen;
	// Model 0 is the world: no trigger uses it and it holds most of the brushes.
	const int maskPlayerSolid = 0x1 | 0x2 | 0x8 | 0x4000 | 0x10000 | 0x2000000;
	for (size_t m = worldBrushes ? 0 : 1; m < modelCount; ++m) {
		std::vector<int> ids;
		seen.assign(brushCount, 0);
		nodeSeen.assign(nodeVec.size(), 0);
		PredCollectBrushes(nodeVec, leafs.data(), leafSize, leafCount, leafBrushVec, model[m].headnode, ids, seen, nodeSeen);
		int first = (int)map.brushes.size();
		for (int b : ids) {
			if ((size_t)b >= brushCount) continue;
			const BspBrush& br = brush[b];
			// Keeping only the sides that exist would make a broken brush bigger, and a
			// huge numsides would spin here for billions of iterations.
			if (br.firstside < 0 || br.numsides <= 0 || (size_t)br.firstside + (size_t)br.numsides > sideCount)
				continue;
			if (m == 0 && !(br.contents & maskPlayerSolid)) continue;
			PredBrushSpan span = {};
			span.firstPlane = (int)map.brushPlanes.size();
			span.contents = br.contents;
			int axial = 0;
			for (int s = 0; s < br.numsides; ++s) {
				size_t si = (size_t)br.firstside + s;
				if (side[si].planenum >= planeCount) continue;
				const BspPlane& pl = plane[side[si].planenum];
				map.brushPlanes.push_back(pl);
				++span.planeCount;
				if (pl.type >= 0 && pl.type <= 2 && (pl.normal[pl.type] == 1.0f || pl.normal[pl.type] == -1.0f)) {
					++axial;
					if (pl.normal[pl.type] == 1.0f) span.boxMaxs[pl.type] = pl.dist;
					else span.boxMins[pl.type] = -pl.dist;
				}
			}
			span.isBox = span.planeCount == 6 && br.numsides == 6 && axial == 6;
			if (span.planeCount) map.brushes.push_back(span);
		}
		map.modelBrushes[m] = {first, (int)map.brushes.size() - first};
	}

	map.loaded = true;
	return true;
}

// p is the box centre in model space, ext its half size.
static bool PredBoxInBrush(const PredMap& map, const PredBrushSpan& b, const float p[3], const float ext[3])
{
	if (b.isBox) {
		for (int c = 0; c < 3; ++c) {
			const float lo = fmaxf(b.boxMins[c], -ext[c] + p[c]);
			const float hi = fminf(b.boxMaxs[c], ext[c] + p[c]);
			if (lo > hi) return false;
		}
		return true;
	}
	for (int i = 0; i < b.planeCount; ++i) {
		const BspPlane& pl = map.brushPlanes[(size_t)b.firstPlane + i];
		const float* n = pl.normal;
		float ofs[3];
		for (int c = 0; c < 3; ++c) ofs[c] = n[c] < 0.0f ? ext[c] : -ext[c];
		const float dist = pl.dist - (ofs[0] * n[0] + ofs[1] * n[1] + ofs[2] * n[2]);
		const float d1 = (p[0] * n[0] + p[1] * n[1] + p[2] * n[2]) - dist;
		if (d1 > 0.0f) return false;
	}
	return b.planeCount > 0;
}

// The engine's trigger touch trace. A rotated model gets the feet point rotated and
// the centre offset added back unrotated, so the box stays axis-aligned. The default
// mask is the touch trace's MASK_SOLID; rngfix's trigger enumeration passes MASK_ALL.
static bool PredBoxInModel(const PredMap& map, int modelIdx, const PredXform& xf, const float feet[3],
	const float mins[3], const float maxs[3], int mask = 0x1 | 0x2 | 0x8 | 0x4000 | 0x2000000)
{
	if (modelIdx < 0 || (size_t)modelIdx >= map.modelBrushes.size()) return false;
	float ext[3], centre[3], start[3], p[3];
	for (int c = 0; c < 3; ++c) {
		ext[c] = (maxs[c] - mins[c]) * 0.5f;
		centre[c] = (mins[c] + maxs[c]) * 0.5f;
		start[c] = feet[c] + centre[c];
	}
	if (!xf.rotated) {
		for (int c = 0; c < 3; ++c) p[c] = start[c] - xf.origin[c];
	} else {
		float w[3];
		for (int c = 0; c < 3; ++c) w[c] = (start[c] + -centre[c]) - xf.m[c][3];
		for (int c = 0; c < 3; ++c) p[c] = (w[0] * xf.m[0][c] + w[1] * xf.m[1][c] + w[2] * xf.m[2][c]) - -centre[c];
	}
	// The partition's bounds for a brush model are a unit wider.
	const BspModel& m = map.models[modelIdx];
	for (int c = 0; c < 3; ++c)
		if (p[c] + ext[c] < m.mins[c] - 1.0f || p[c] - ext[c] > m.maxs[c] + 1.0f) return false;
	auto span = map.modelBrushes[modelIdx];
	for (int i = 0; i < span.second; ++i) {
		const PredBrushSpan& b = map.brushes[(size_t)span.first + i];
		if ((b.contents & mask) && PredBoxInBrush(map, b, p, ext)) return true;
	}
	return false;
}

struct PredPlayerIO {
	float* origin;
	float* velocity;
	float* baseVelocity;
	float* gravity;
	int*   flags;
	uint32_t* groundEntity = nullptr;
	uint8_t* moveFlags;  // CMoveData byte 0: bit 0 first run, bit 1 m_bGameCodeMovedPlayer
	int    tick;
	// The usercmd's command_number, INT_MIN if unknown. The server moves our tickbase now and
	// then, so snapshots of the state after a command are keyed by this when it's known.
	int    command = INT_MIN;
	int    moveType;
	bool   solid;        // alive and not observing: only then does ProcessImpacts touch triggers
	float  curtime;
	float  interval;
	float  mins[3];
	float  maxs[3];
	// The server runs none of our commands after this one before its frame-end untouch:
	// the command ended its packet, or which one did is unknown.
	bool   frameEnd = true;
	// For +use: the command's buttons and view, and the eye's height above the origin.
	int    buttons = 0;
	int    oldButtons = 0;
	float  viewAngles[3] = {0, 0, 0};
	float  viewHeight = 64.0f;
};

static int PredSnapshotKey(const PredPlayerIO& io)
{
	return io.command != INT_MIN ? io.command : io.tick;
}

// eventqueuefix's clock for player events and trigger_multiple waits: it counts the player's
// commands (one step per OnPlayerRunCmd), which the command number follows, while tickbase
// jumps whenever the server corrects it.
static int PredEventClock(const PredPlayerIO& io)
{
	return PredSnapshotKey(io);
}

#include "zones_sim.h"
#include "trigger_sim.h"
