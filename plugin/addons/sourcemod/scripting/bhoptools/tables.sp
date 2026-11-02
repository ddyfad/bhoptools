// Under the engine's 16383-byte limit for an entry's data.
#define TABLE_ENTRY 16000

int gI_Crc32[256];

void InitCrc32()
{
	if (gI_Crc32[1] != 0)
	{
		return;
	}

	for (int i = 0; i < 256; i++)
	{
		int c = i;

		for (int k = 0; k < 8; k++)
		{
			c = (c & 1) ? (0xEDB88320 ^ (c >>> 1)) : (c >>> 1);
		}

		gI_Crc32[i] = c;
	}
}

int Crc32Byte(int crc, int c)
{
	return gI_Crc32[(crc ^ c) & 0xFF] ^ (crc >>> 8);
}

int Crc32String(int crc, const char[] s)
{
	for (int i = 0; s[i] != 0; i++)
	{
		crc = Crc32Byte(crc, s[i] & 0xFF);
	}

	return crc;
}

// The header goes last so it never points at missing entries.
bool PublishTable(const char[] name, const char[] data, int len, const char[] info)
{
	int table = FindStringTable("InfoPanel");

	if (table == INVALID_STRING_TABLE)
	{
		return false;
	}

	InitCrc32();

	char key[64], header[128];
	int entries = (len + TABLE_ENTRY - 1) / TABLE_ENTRY;
	int crc = -1;

	for (int i = 0; i < len; i++)
	{
		crc = Crc32Byte(crc, data[i] & 0xFF);
	}

	bool locked = LockStringTables(false);

	for (int i = 0; i < entries; i++)
	{
		int at = i * TABLE_ENTRY;
		FormatEx(key, sizeof(key), "%s_%d", name, i);
		AddToStringTable(table, key, data[at], (len - at < TABLE_ENTRY) ? len - at : TABLE_ENTRY);
	}

	FormatEx(header, sizeof(header), "%d %d %s", entries, ~crc, info);
	AddToStringTable(table, name, header, strlen(header) + 1);
	LockStringTables(locked);

	// A full table drops what doesn't fit.
	return FindStringIndex(table, name) != INVALID_STRING_INDEX;
}
