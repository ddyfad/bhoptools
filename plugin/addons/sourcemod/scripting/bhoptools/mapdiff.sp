// The client reads the bsp's own entity lump. EntityLump has what the game spawned from, after .lmp
// files, Stripper and OnMapInit edits.

#define MAPDIFF_FORMAT 1
// The whole InfoPanel table goes into each client's signon message, which has 288000 bytes for every
// table together.
#define MAPDIFF_MAX 128000

enum
{
	BSPENT_HAMMERID,
	BSPENT_CRC,
	BSPENT_SENT,
	BSPENT_MATCHED,
	BSPENT_FIELDS
}

// Mirrors the client's PredParseEntityLump.
enum
{
	LUMP_OUT,
	LUMP_KEY,
	LUMP_GAP,
	LUMP_VALUE
}

// The first of each of these is the one the client reads.
#define SEEN_HAMMERID   (1 << 0)
#define SEEN_CLASSNAME  (1 << 1)
#define SEEN_TARGETNAME (1 << 2)
#define SEEN_VSCRIPTS   (1 << 3)

bool gB_Mapdiff;
char gS_Mapdiff[MAPDIFF_MAX];
int gI_MapdiffLen;

// Whatever the client's trigger sim, filters, scripts and mpbhops doors look at; named entities are
// sent whatever their class, since destinations, parents and output targets are found by name.
bool MapdiffClass(const char[] classname)
{
	return StrContains(classname, "trigger_") != -1 || strncmp(classname, "filter_", 7) == 0
		|| StrEqual(classname, "logic_relay") || StrEqual(classname, "math_counter") || StrEqual(classname, "logic_branch")
		|| StrEqual(classname, "logic_case") || StrEqual(classname, "logic_script")
		|| StrEqual(classname, "func_door") || StrEqual(classname, "func_button");
}

void MapdiffKey(const char[] key, const char[] value, int &seen, int &hammerid, bool &sent)
{
	int bit;

	if (StrEqual(key, "hammerid", false))
	{
		bit = SEEN_HAMMERID;
	}
	else if (StrEqual(key, "classname", false))
	{
		bit = SEEN_CLASSNAME;
	}
	else if (StrEqual(key, "targetname", false))
	{
		bit = SEEN_TARGETNAME;
	}
	else if (StrEqual(key, "vscripts", false))
	{
		bit = SEEN_VSCRIPTS;
	}

	if (bit == 0 || (seen & bit))
	{
		return;
	}

	seen |= bit;

	if (bit == SEEN_HAMMERID)
	{
		hammerid = StringToInt(value);
	}
	else if (bit == SEEN_CLASSNAME)
	{
		sent = sent || MapdiffClass(value);
	}
	else
	{
		sent = sent || value[0] != 0;
	}
}

// Parsed byte for byte as the client does, so an entity's position in `ents` is its position in the client's list.
bool ReadBspEntities(ArrayList ents, int &lumpCrc)
{
	char map[PLATFORM_MAX_PATH], path[PLATFORM_MAX_PATH];
	GetCurrentMap(map, sizeof(map));
	FormatEx(path, sizeof(path), "maps/%s.bsp", map);
	File f = OpenFile(path, "rb", true);

	if (f == null)
	{
		LogError("mapdiff: can't open %s", path);
		return false;
	}

	int ident, version, offset, length;
	bool ok = f.ReadInt32(ident) && ident == 0x50534256 && f.ReadInt32(version)
		&& f.ReadInt32(offset) && f.ReadInt32(length) && length > 0 && f.Seek(offset, SEEK_SET);

	static char buf[4096];
	char key[64], value[64];
	int crc = -1, state = LUMP_OUT, left = length;
	int entCrc, kvCrc, keyLen, valueLen, kvs, hammerid, seen;
	bool inEnt, ended, sent;

	while (ok && left > 0)
	{
		int n = f.ReadString(buf, sizeof(buf), left < sizeof(buf) ? left : sizeof(buf));

		if (n <= 0 || (left == length && n >= 4 && strncmp(buf, "LZMA", 4) == 0))
		{
			ok = false;
			break;
		}

		left -= n;

		for (int i = 0; i < n; i++)
		{
			int c = buf[i] & 0xFF;
			crc = gI_Crc32[(crc ^ c) & 0xFF] ^ (crc >>> 8);

			if (ended)
			{
				continue;
			}

			if (state == LUMP_KEY)
			{
				entCrc = Crc32Byte(entCrc, c);

				if (c == '"')
				{
					key[keyLen] = 0;
					state = LUMP_GAP;
				}
				else if (keyLen < sizeof(key) - 1)
				{
					key[keyLen++] = c;
				}

				continue;
			}

			if (state == LUMP_VALUE)
			{
				entCrc = Crc32Byte(entCrc, c);

				if (c == '"')
				{
					value[valueLen] = 0;
					kvs++;
					MapdiffKey(key, value, seen, hammerid, sent);
					state = LUMP_OUT;
				}
				else if (valueLen < sizeof(value) - 1)
				{
					value[valueLen++] = c;
				}

				continue;
			}

			if (state == LUMP_GAP)
			{
				if (c == '"')
				{
					valueLen = 0;
					state = LUMP_VALUE;
					continue;
				}

				if (c != '{' && c != '}')
				{
					continue;
				}

				// A key with no value: the client drops it and takes the brace as usual.
				entCrc = kvCrc;
				state = LUMP_OUT;
			}

			if (c == 0)
			{
				ended = true;
			}
			else if (c == '{')
			{
				inEnt = true;
				entCrc = -1;
				kvs = 0;
				hammerid = 0;
				seen = 0;
				sent = false;
			}
			else if (c == '}')
			{
				if (inEnt && kvs > 0)
				{
					int row = ents.Push(hammerid);
					ents.Set(row, entCrc, BSPENT_CRC);
					ents.Set(row, sent, BSPENT_SENT);
					ents.Set(row, false, BSPENT_MATCHED);
				}

				inEnt = false;
			}
			else if (c == '"' && inEnt)
			{
				kvCrc = entCrc;
				keyLen = 0;
				state = LUMP_KEY;
			}
		}
	}

	delete f;

	if (!ok)
	{
		LogError("mapdiff: couldn't read the entity lump of %s (missing, truncated or compressed)", path);
		return false;
	}

	lumpCrc = ~crc;
	return true;
}

void HashLumpEntry(EntityLumpEntry entry, int &hammerid, int &crc, bool &sent)
{
	static char key[256], value[4096];
	int seen;
	hammerid = 0;
	crc = -1;
	sent = false;

	for (int i = 0; i < entry.Length; i++)
	{
		entry.Get(i, key, sizeof(key), value, sizeof(value));
		crc = Crc32Byte(Crc32String(crc, key), '"');
		crc = Crc32Byte(Crc32String(crc, value), '"');
		MapdiffKey(key, value, seen, hammerid, sent);
	}
}

bool MapdiffAppend(const char[] format, any ...)
{
	int room = sizeof(gS_Mapdiff) - gI_MapdiffLen;
	int n = VFormat(gS_Mapdiff[gI_MapdiffLen], room, format, 2);

	// Anything that fills the buffer may have been cut short.
	if (n >= room - 1)
	{
		return false;
	}

	gI_MapdiffLen += n;
	return true;
}

bool MapdiffAppendEntity(int index)
{
	static char key[256], value[4096];
	EntityLumpEntry entry = EntityLump.Get(index);
	bool ok = MapdiffAppend("{");

	for (int i = 0; ok && i < entry.Length; i++)
	{
		entry.Get(i, key, sizeof(key), value, sizeof(value));
		ok = MapdiffAppend("%s\"%s\" \"%s\"", (i > 0) ? " " : "", key, value);
	}

	delete entry;
	return ok && MapdiffAppend("}\n");
}

void BuildMapdiff()
{
	gB_Mapdiff = false;
	gI_MapdiffLen = 0;

	InitCrc32();

	ArrayList bsp = new ArrayList(BSPENT_FIELDS);

	int lumpCrc;

	// An empty EntityLump means SourceMod couldn't parse the map's; everything would look removed.
	if (EntityLump.Length() == 0 || !ReadBspEntities(bsp, lumpCrc))
	{
		delete bsp;
		return;
	}

	char key[24];
	int ord;
	StringMap byHammer = new StringMap();

	for (int i = 0; i < bsp.Length; i++)
	{
		int hammerid = bsp.Get(i, BSPENT_HAMMERID);

		if (hammerid != 0)
		{
			IntToString(hammerid, key, sizeof(key));
			byHammer.SetValue(key, byHammer.ContainsKey(key) ? -1 : i);
		}
	}

	StringMap byCrc = new StringMap();
	StringMap crcCount = new StringMap();

	for (int i = 0; i < bsp.Length; i++)
	{
		int hammerid = bsp.Get(i, BSPENT_HAMMERID);
		IntToString(hammerid, key, sizeof(key));

		if (hammerid == 0 || (byHammer.GetValue(key, ord) && ord == -1))
		{
			int crc = bsp.Get(i, BSPENT_CRC), n;
			FormatEx(key, sizeof(key), "%x", crc);
			crcCount.GetValue(key, n);
			crcCount.SetValue(key, n + 1);
			FormatEx(key, sizeof(key), "%x_%d", crc, n);
			byCrc.SetValue(key, i);
		}
	}

	crcCount.Clear();
	ArrayList live = new ArrayList(3);
	int count = EntityLump.Length();

	for (int i = 0; i < count; i++)
	{
		EntityLumpEntry entry = EntityLump.Get(i);
		int hammerid, crc;
		bool sent;
		HashLumpEntry(entry, hammerid, crc, sent);
		delete entry;

		IntToString(hammerid, key, sizeof(key));

		if (hammerid == 0 || !byHammer.GetValue(key, ord) || ord == -1 || bsp.Get(ord, BSPENT_MATCHED))
		{
			int n;
			FormatEx(key, sizeof(key), "%x", crc);
			crcCount.GetValue(key, n);
			crcCount.SetValue(key, n + 1);
			FormatEx(key, sizeof(key), "%x_%d", crc, n);

			if (!byCrc.GetValue(key, ord))
			{
				ord = -1;
			}
		}

		if (ord != -1)
		{
			bsp.Set(ord, true, BSPENT_MATCHED);
		}

		int row = live.Push(ord);
		live.Set(row, crc, 1);
		live.Set(row, sent, 2);
	}

	delete byHammer;
	delete byCrc;
	delete crcCount;

	bool ok = true;
	int removed, modified, added;

	for (int i = 0; ok && i < bsp.Length; i++)
	{
		if (!bsp.Get(i, BSPENT_MATCHED) && bsp.Get(i, BSPENT_SENT))
		{
			ok = MapdiffAppend("r %d %d\n", i, bsp.Get(i, BSPENT_HAMMERID));
			removed++;
		}
	}

	int last = -1;

	for (int i = 0; ok && i < count; i++)
	{
		ord = live.Get(i);
		bool sent = live.Get(i, 2);

		if (ord != -1)
		{
			if (live.Get(i, 1) != bsp.Get(ord, BSPENT_CRC) && (sent || bsp.Get(ord, BSPENT_SENT)))
			{
				ok = MapdiffAppend("m %d %d ", ord, bsp.Get(ord, BSPENT_HAMMERID)) && MapdiffAppendEntity(i);
				modified++;
			}

			last = ord;
		}
		else if (sent)
		{
			ok = MapdiffAppend("a %d ", last) && MapdiffAppendEntity(i);
			added++;
		}
	}

	delete bsp;
	delete live;

	// A diff published earlier this map stays: the spawned entities haven't changed since.
	if (!ok)
	{
		LogError("mapdiff: the entity diff is over %d bytes; clients predict from the bsp", MAPDIFF_MAX);
		return;
	}

	char info[32];
	FormatEx(info, sizeof(info), "%d %d", MAPDIFF_FORMAT, lumpCrc);
	gB_Mapdiff = PublishTable("bhoptools_mapdiff", gS_Mapdiff, gI_MapdiffLen, info);

	if (gCV_Log.BoolValue)
	{
		LogToFileEx(gS_LogPath, "mapdiff: %d removed, %d modified, %d added, %d bytes, lump %08x, published %d",
			removed, modified, added, gI_MapdiffLen, lumpCrc, gB_Mapdiff);
	}
}
