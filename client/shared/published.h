#pragma once

// Must match the plugin's PublishTable (bhoptools/tables.sp).

#include "prediction.h"
#include "game_layout.h"
#include "sigs.h"

#include <cstdio>
#include <string>

static std::string TableUserData(void* table, const char* name)
{
	const int i = VFunc<int (*)(void*, const char*)>(table, VT_INetworkStringTable_FindStringIndex)(table, name);
	if (i < 0 || i == 0xFFFF) return std::string();
	int len = 0;
	const char* p = VFunc<const char* (*)(void*, int, int*)>(table, VT_INetworkStringTable_GetStringUserData)(table, i, &len);
	return p && len > 0 ? std::string(p, (size_t)len) : std::string();
}

static void* PublishedTable(void* container)
{
	if (!container) return nullptr;
	return VFunc<void* (*)(void*, const char*)>(container, VT_INetworkStringTableContainer_FindTable)(container, "InfoPanel");
}

// The header changes whenever the data does.
static std::string PublishedHeader(void* container, const char* name)
{
	void* table = PublishedTable(container);
	return table ? TableUserData(table, name) : std::string();
}

static bool ReadPublished(void* container, const char* name, std::string& info, std::string& data)
{
	void* table = PublishedTable(container);
	if (!table) return false;
	const std::string header = TableUserData(table, name);
	int entries = 0, crc = 0, used = 0;
	if (sscanf(header.c_str(), "%d %d %n", &entries, &crc, &used) != 2) return false;
	info = header.c_str() + used;
	data.clear();
	char key[80];
	for (int i = 0; i < entries; ++i) {
		snprintf(key, sizeof(key), "%s_%d", name, i);
		const std::string part = TableUserData(table, key);
		if (part.empty()) return false;
		data += part;
	}
	return (int)PredCrc32(data.data(), data.size()) == crc;
}
