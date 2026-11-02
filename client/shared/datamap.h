#pragma once

#include "utils.h"
#include "sigs.h"

#include <cstdint>
#include <cstring>
#include <vector>

namespace dm {

// typedescription_t: fieldName +8, fieldOffset[0] +0x10, fieldSizeInBytes per platform (sigs.h).
// Constant-initialised, so valid as soon as the client module is mapped, unlike RecvTables.
struct FieldReq {
	const char* name;
	int type;
	int bytes;     // fieldSizeInBytes, -1 = don't check
	uintptr_t lit[4];
	int nlit;
	int offset;    // -1 unresolved, -2 conflicting
};

const int FIELD_FLOAT = 1, FIELD_VECTOR = 3, FIELD_INTEGER = 5, FIELD_BOOLEAN = 6, FIELD_CHARACTER = 8, FIELD_EMBEDDED = 10,
	FIELD_EHANDLE = 13;

inline void ResolveFields(const std::vector<MemRange>& segs, FieldReq* req, int nreq)
{
	const int nsegs = (int)segs.size();
	for (int r = 0; r < nreq; ++r) { req[r].nlit = 0; req[r].offset = -1; }

	for (int s = 0; s < nsegs; ++s) {
		const char* base = reinterpret_cast<const char*>(segs[s].start);
		const size_t size = segs[s].end - segs[s].start;
		for (const char* p = base; (p = static_cast<const char*>(memchr(p, 'm', size - (p - base)))); ++p) {
			if (p + 1 >= base + size || p[1] != '_') continue;
			for (int r = 0; r < nreq; ++r) {
				const size_t n = strlen(req[r].name) + 1;
				if ((size_t)(base + size - p) >= n && memcmp(p, req[r].name, n) == 0 && req[r].nlit < 4)
					req[r].lit[req[r].nlit++] = reinterpret_cast<uintptr_t>(p);
			}
		}
	}

	uintptr_t lo = ~(uintptr_t)0, hi = 0;
	for (int r = 0; r < nreq; ++r)
		for (int i = 0; i < req[r].nlit; ++i) {
			if (req[r].lit[i] < lo) lo = req[r].lit[i];
			if (req[r].lit[i] > hi) hi = req[r].lit[i];
		}

	for (int s = 0; s < nsegs; ++s) {
		const uintptr_t first = (segs[s].start + 7) & ~(uintptr_t)7;
		for (uintptr_t a = first + 8; a + OFF_typedescription_fieldSizeInBytes + 0x10 <= segs[s].end; a += 8) {
			const uintptr_t v = *reinterpret_cast<const uintptr_t*>(a);
			if (v < lo || v > hi) continue;
			const char* td = reinterpret_cast<const char*>(a - 8);
			for (int r = 0; r < nreq; ++r) {
				bool named = false;
				for (int i = 0; i < req[r].nlit; ++i) named |= req[r].lit[i] == v;
				if (!named) continue;
				if (*reinterpret_cast<const int*>(td) != req[r].type) continue;
				if (req[r].bytes >= 0 && *reinterpret_cast<const int*>(td + OFF_typedescription_fieldSizeInBytes) != req[r].bytes) continue;
				const int off = *reinterpret_cast<const int*>(td + 0x10);
				if (off < 0 || off > 0x8000) continue;
				if (req[r].offset == -1) req[r].offset = off;
				else if (req[r].offset != off) req[r].offset = -2;
			}
		}
	}
}

}
