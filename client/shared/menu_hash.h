#pragma once

#include <cstdint>

// Must match bhoptools.sp's HashString: FNV-1a over unsigned bytes. The server prints it signed.
inline uint32_t MenuHash(const char* s, int len)
{
	uint32_t h = 2166136261u;
	for (int i = 0; i < len; ++i) {
		h ^= (unsigned char)s[i];
		h *= 16777619u;
	}
	return h;
}
