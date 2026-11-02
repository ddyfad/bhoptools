#pragma once
#include <cstdio>
#include <cstring>

// lump_checksums.csv lines are "<fastdl.me sha1>,<lump md5>\n", sometimes with \r\n.
inline bool LumpChecksumsFindSha1(const char* csv, const unsigned char md5[16], char sha1[41])
{
	char key[36];
	for (int i = 0; i < 16; ++i) snprintf(key + i * 2, 3, "%02x", md5[i]);
	key[32] = '\n'; key[33] = 0;
	const char* found = strstr(csv, key);
	if (!found && strchr(csv, '\r')) {
		key[32] = '\r'; key[33] = '\n'; key[34] = 0;
		found = strstr(csv, key);
	}
	if (!found || found - csv < 41 || found[-1] != ',') return false;
	memcpy(sha1, found - 41, 40);
	sha1[40] = 0;
	return true;
}
