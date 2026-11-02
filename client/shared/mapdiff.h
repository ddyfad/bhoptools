#pragma once

// Ordinals must count entities as PredParseEntityLump does.

#include "prediction.h"

struct PredMapDiffEdit {
	char op;
	int  ordinal;
	int  hammerId;
	PredMapEntity ent;
};

static bool PredParseMapDiff(const char* text, size_t len, std::vector<PredMapDiffEdit>& out)
{
	out.clear();
	const char* s = text;
	const char* end = text + len;
	while (s < end) {
		const char* eol = (const char*)memchr(s, '\n', (size_t)(end - s));
		if (!eol) eol = end;
		const std::string line(s, eol);
		s = eol + 1;
		if (line.empty()) continue;

		PredMapDiffEdit e;
		e.op = line[0];
		e.hammerId = 0;
		int used = 0;
		if (e.op == 'r' || e.op == 'm') {
			if (sscanf(line.c_str() + 1, " %d %d%n", &e.ordinal, &e.hammerId, &used) != 2) return false;
		} else if (e.op == 'a') {
			if (sscanf(line.c_str() + 1, " %d%n", &e.ordinal, &used) != 1) return false;
		} else {
			return false;
		}
		if (e.op != 'r') {
			const char* block = line.c_str() + 1 + used;
			std::vector<PredMapEntity> ents;
			PredParseEntityLump(block, strlen(block), ents);
			if (ents.size() != 1) return false;
			e.ent = std::move(ents[0]);
		}
		out.push_back(std::move(e));
	}
	return true;
}

// False, leaving `out` unspecified, when the diff names an entity the BSP doesn't have at that
// ordinal or edits one twice: it was made against another lump.
static bool PredApplyMapDiff(const std::vector<PredMapEntity>& bsp, const std::vector<PredMapDiffEdit>& edits,
	std::vector<PredMapEntity>& out)
{
	const int n = (int)bsp.size();
	std::vector<const PredMapEntity*> slot(bsp.size());
	std::vector<uint8_t> edited(bsp.size(), 0);
	std::vector<std::vector<const PredMapEntity*>> after(bsp.size() + 1);
	for (int i = 0; i < n; ++i) slot[i] = &bsp[i];
	for (const PredMapDiffEdit& e : edits) {
		if (e.op == 'a') {
			if (e.ordinal < -1 || e.ordinal >= n) return false;
			after[e.ordinal + 1].push_back(&e.ent);
			continue;
		}
		if (e.ordinal < 0 || e.ordinal >= n || edited[e.ordinal]
				|| atoi(bsp[e.ordinal].Get("hammerid", "0")) != e.hammerId)
			return false;
		edited[e.ordinal] = 1;
		slot[e.ordinal] = e.op == 'm' ? &e.ent : nullptr;
	}
	out.clear();
	out.reserve(bsp.size() + edits.size());
	for (const PredMapEntity* p : after[0]) out.push_back(*p);
	for (int i = 0; i < n; ++i) {
		if (slot[i]) out.push_back(*slot[i]);
		for (const PredMapEntity* p : after[i + 1]) out.push_back(*p);
	}
	return true;
}
