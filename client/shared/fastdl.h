#pragma once

#include <cstdio>

namespace fastdl {

const char BASE[] = "https://main.fastdl.me/";

typedef bool (*ProcessServerInfoFn)(void* self, char* msg);
typedef bool (*MD5_MapFileFn)(unsigned char* md5, const char* mapfile);
typedef bool (*IsValidFileForTransferFn)(const char* filename);

extern ProcessServerInfoFn      oProcessServerInfo;
extern MD5_MapFileFn            MD5_MapFile;
extern IsValidFileForTransferFn oIsValidFileForTransfer;

bool Hooked_ProcessServerInfo(void* self, char* msg);
bool Hooked_IsValidFileForTransfer(const char* filename);

// From the OS's download queue hook: true when it queued fastdl.me's copy instead.
// `plain` is the last call for this file.
bool Queue(void* dlman, const char* gamePath, bool bz2, bool plain);
#ifdef _WIN32
void RemoveDownloadVtableHooks();
#endif

// The server's map name when the level is the maps/<sha1>.bsp stand-in for a mismatched local
// copy, otherwise levelName.
const char* ServerMapName(const char* levelName);

// Blocks on the network, so run it on its own thread.
void LoadLumpChecksums();

// Per OS: queue url from BASE as gamePath, a .bz2 attempt and/or the file itself.
void QueueFromFastdl(void* dlman, const char* url, const char* gamePath, bool bz2, bool plain);
// Per OS: lump_checksums.csv from the cache, fetched first when it's over 36 hours old.
FILE* OpenLumpChecksums();

}
