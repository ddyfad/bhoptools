#define ENT_DISABLED (1 << 0)
#define ENT_REMOVED  (1 << 1)
#define ENT_SPENT    (1 << 2)
#define ENT_BRANCH   (1 << 3)
#define ENT_REFIRE   (1 << 4)
#define ENT_HITMIN   (1 << 5)
#define ENT_HITMAX   (1 << 6)
// A door's m_toggle_state, two bits.
#define ENT_DOOR_SHIFT 8

// bhoptools_ents entities per command, the most the client takes.
#define ENTS_CHUNK 8
// bhoptools_ents messages per command. A whole map in one frame overflows the client's command buffer.
#define ENTS_MESSAGES 2
#define MAX_SYNC 1024
// NUM_ENT_ENTRIES: edictless entities like logic_relay sit above the 2048 edicts.
#define MAX_ENTITY_INDEX 4096

// eventqueuefix: clones of this player's pending events (cell 0) and trigger_multiple waits (cell 1,
// { caller ref, float wait in commands }). Newer versions write 6 cells, older ones 2.
native bool GetClientEvents(int client, any[] eventpack, int packsize = 8);

enum
{
	SYNC_TRIGGER, // trigger_teleport, trigger_push, trigger_gravity
	SYNC_MULTIPLE,
	SYNC_ONCE,
	SYNC_RELAY,
	SYNC_COUNTER,
	SYNC_BRANCH,
	SYNC_CASE,
	SYNC_DOOR, // func_door, func_door_rotating
	SYNC_KINDS
}

char gS_SyncClasses[][] = { "trigger_teleport", "trigger_push", "trigger_gravity", "trigger_multiple", "trigger_once", "logic_relay", "math_counter", "logic_branch", "logic_case", "func_door", "func_door_rotating" };
int gI_SyncClassKind[] = { SYNC_TRIGGER, SYNC_TRIGGER, SYNC_TRIGGER, SYNC_MULTIPLE, SYNC_ONCE, SYNC_RELAY, SYNC_COUNTER, SYNC_BRANCH, SYNC_CASE, SYNC_DOOR, SYNC_DOOR };

// The datamap fields ReadEntity reads for each kind, in order. m_pfnTouch: the trigger_once's touch
// function, cleared when it fires. m_OutValue: math_counter's COutputFloat, whose variant_t starts
// with the float. A door's m_bLocked goes out as ENT_DISABLED.
char gS_KindProps[SYNC_KINDS][4][20] =
{
	{ "m_bDisabled", "", "", "" },
	{ "m_bDisabled", "m_pfnTouch", "", "" },
	{ "m_bDisabled", "m_pfnTouch", "", "" },
	{ "m_bDisabled", "m_bWaitForRefire", "", "" },
	{ "m_bDisabled", "m_bHitMin", "m_bHitMax", "m_OutValue" },
	{ "m_bInValue", "", "", "" },
	{ "", "", "", "" },
	{ "m_bLocked", "m_toggle_state", "m_flMoveDoneTime", "m_flLocalTime" }
};
int gI_KindOffset[SYNC_KINDS][4];
bool gB_KindOffsets[SYNC_KINDS];

StringMap gH_SyncClasses;
// Slots, one per hammerid; the entity is -1 once removed, and the slot is reused if it's spawned again.
int gI_Synced;
int gI_SyncHammer[MAX_SYNC];
int gI_SyncEntity[MAX_SYNC];
int gI_SyncKind[MAX_SYNC];
int gI_EntitySlot[MAX_ENTITY_INDEX];
bool gB_SyncMultiple;

int gI_SentFlags[MAXPLAYERS+1][MAX_SYNC];
int gI_SentValue[MAXPLAYERS+1][MAX_SYNC];
int gI_SentWait[MAXPLAYERS+1][MAX_SYNC];
bool gB_StateDump[MAXPLAYERS+1];
bool gB_DumpOpen[MAXPLAYERS+1];
// Entries sent in the open dump; the end says how many, so the client can tell it lost some.
int gI_DumpEntries[MAXPLAYERS+1];
// The client's last command that ran; state goes out at the end of the frame it ran in.
int gI_StateCmd[MAXPLAYERS+1];
bool gB_StateRan[MAXPLAYERS+1];
bool gB_UseHeld[MAXPLAYERS+1];
float gF_SentGravity[MAXPLAYERS+1];
int gI_SentBaseVel[MAXPLAYERS+1];
char gS_SentName[MAXPLAYERS+1][96];
char gS_SentClass[MAXPLAYERS+1][64];

// eventqueuefix waits for the command being sent, valid where the stamp matches.
int gI_WaitStamp;
int gI_SlotWaitStamp[MAX_SYNC];
int gI_SlotWait[MAX_SYNC];

public void OnMapEnd()
{
	ResetSync();
	Update_OnMapEnd();
}

void ResetSync()
{
	gI_Synced = 0;
	gB_SyncMultiple = false;

	for (int i = 0; i < MAX_ENTITY_INDEX; i++)
	{
		gI_EntitySlot[i] = -1;
	}
}

// CleanUpMap respawns the map's entities before round_start.
public void Event_RoundStart(Event event, const char[] name, bool dontBroadcast)
{
	RequestStateDumps();
}

void RequestStateDumps()
{
	for (int i = 1; i <= MaxClients; i++)
	{
		gB_StateDump[i] = true;
	}
}

public void OnEntityCreated(int entity, const char[] classname)
{
	int kind;

	// Keyvalues, the hammerid among them, aren't set until it spawns.
	if (gH_SyncClasses != null && gH_SyncClasses.GetValue(classname, kind))
	{
		SDKHook(entity, SDKHook_SpawnPost, OnSyncSpawned);
	}
}

public void OnSyncSpawned(int entity)
{
	char classname[32];
	int kind;

	if (GetEntityClassname(entity, classname, sizeof(classname)) && gH_SyncClasses.GetValue(classname, kind))
	{
		TrackEntity(entity, kind);
	}
}

public void OnEntityDestroyed(int entity)
{
	int index = EntRefToEntIndex(entity);

	if (index < 0 || index >= MAX_ENTITY_INDEX || gI_EntitySlot[index] == -1)
	{
		return;
	}

	gI_SyncEntity[gI_EntitySlot[index]] = -1;
	gI_EntitySlot[index] = -1;
}

// Entities are named by hammerid on the client. A point_template can spawn a second copy with the same one;
// the client has one entity per hammerid, so only a copy spawned after the first is gone takes its place.
void TrackEntity(int entity, int kind)
{
	int index = EntRefToEntIndex(entity);

	if (index < 0 || index >= MAX_ENTITY_INDEX || gI_EntitySlot[index] != -1 || !FindKindOffsets(index, kind))
	{
		return;
	}

	int hammer = GetEntProp(index, Prop_Data, "m_iHammerID");

	// Touch-open doors are mpbhops' platforms, which it keeps still, and a map can have hundreds.
	if (hammer == 0 || (kind == SYNC_DOOR && GetEntProp(index, Prop_Data, "m_spawnflags") & 1024))
	{
		return;
	}

	int slot = 0;

	while (slot < gI_Synced && gI_SyncHammer[slot] != hammer)
	{
		slot++;
	}

	if (slot == gI_Synced)
	{
		if (gI_Synced == MAX_SYNC)
		{
			LogError("More than %d synced entities, hammerid %d isn't synced", MAX_SYNC, hammer);
			return;
		}

		gI_Synced++;
		gI_SyncHammer[slot] = hammer;

		for (int i = 1; i <= MaxClients; i++)
		{
			gI_SentFlags[i][slot] = -1;
		}
	}
	else if (gI_SyncEntity[slot] != -1)
	{
		return;
	}

	gI_SyncEntity[slot] = index;
	gI_SyncKind[slot] = kind;
	gI_EntitySlot[index] = slot;

	if (kind == SYNC_MULTIPLE)
	{
		gB_SyncMultiple = true;
	}
}

void TrackMissing(int hammer, int kind)
{
	if (hammer == 0 || kind == SYNC_DOOR || gI_Synced == MAX_SYNC)
	{
		return;
	}

	for (int slot = 0; slot < gI_Synced; slot++)
	{
		if (gI_SyncHammer[slot] == hammer)
		{
			return;
		}
	}

	int slot = gI_Synced++;
	gI_SyncHammer[slot] = hammer;
	gI_SyncEntity[slot] = -1;
	gI_SyncKind[slot] = kind;

	for (int i = 1; i <= MaxClients; i++)
	{
		gI_SentFlags[i][slot] = -1;
	}
}

bool FindKindOffsets(int entity, int kind)
{
	if (gB_KindOffsets[kind])
	{
		return true;
	}

	for (int i = 0; i < 4 && gS_KindProps[kind][i][0] != 0; i++)
	{
		if ((gI_KindOffset[kind][i] = FindDataMapInfo(entity, gS_KindProps[kind][i])) == -1)
		{
			char classname[32];
			GetEntityClassname(entity, classname, sizeof(classname));
			LogError("%s has no %s, not synced", classname, gS_KindProps[kind][i]);
			return false;
		}
	}

	gB_KindOffsets[kind] = true;
	return true;
}

public Action Command_Resync(int client, int args)
{
	if (client != 0 && gB_Predicting[client] && GetCmdArgInt(1) == gI_Session[client])
	{
		gB_StateDump[client] = true;
	}

	return Plugin_Handled;
}

// SourceMod runs OnGameFrame before the game's GameFrame, which runs the players' commands, then the
// touch manager's untouch, then the event queue. So here the previous frame is complete, timed events
// included, as the client's snapshot for its last command there is.
public void OnGameFrame()
{
	for (int i = 1; i <= MaxClients; i++)
	{
		if (gB_StateRan[i] && IsClientInGame(i))
		{
			SendState(i, gI_StateCmd[i]);
		}
	}
}

void SendState(int client, int cmdnum)
{
	if (!gB_Predicting[client] || gI_Proto[client] < 2 || !(gI_ClientFlags[client] & CLIENT_STATE))
	{
		return;
	}

	int session = gI_Session[client];

	if (gB_StateDump[client])
	{
		gB_StateDump[client] = false;
		gB_DumpOpen[client] = true;
		gI_DumpEntries[client] = 0;

		for (int slot = 0; slot < gI_Synced; slot++)
		{
			gI_SentFlags[client][slot] = -1;
		}

		ClientCommand(client, "bhoptools_sync %d %d begin", session, cmdnum);
	}

	bool waits = gB_SyncMultiple && ReadWaits(client);
	char prefix[48], entries[256], entry[48];
	int prefixLen = FormatEx(prefix, sizeof(prefix), "bhoptools_ents %d %d 0", session, cmdnum);
	int len = 0;
	int count = 0;
	int sent = 0;
	int messages = 0;
	bool more = false;

	for (int slot = 0; slot < gI_Synced; slot++)
	{
		int value;
		int flags = ReadEntity(slot, value);
		int wait = -1;

		if (gI_SyncKind[slot] == SYNC_MULTIPLE && waits && !(flags & ENT_REMOVED))
		{
			wait = (gI_SlotWaitStamp[slot] == gI_WaitStamp) ? gI_SlotWait[slot] : 0;
		}
		else if (gI_SyncKind[slot] == SYNC_DOOR && !(flags & ENT_REMOVED))
		{
			wait = DoorWait(slot, flags);
		}

		// A wait counts down with every command, so compare the command it ends on. A door's wait only
		// changes with a new move.
		int waitEnd = (wait > 0) ? cmdnum + wait : wait;
		if (flags == gI_SentFlags[client][slot] && value == gI_SentValue[client][slot]
			&& (waitEnd == gI_SentWait[client][slot] || gI_SyncKind[slot] == SYNC_DOOR))
		{
			continue;
		}

		int entryLen = FormatEx(entry, sizeof(entry), " %d %d %x %d", gI_SyncHammer[slot], flags, value, wait);

		if (count == ENTS_CHUNK || prefixLen + len + entryLen >= 250)
		{
			ClientCommand(client, "bhoptools_ents %d %d %d%s", session, cmdnum, count, entries);
			len = 0;
			count = 0;

			if (++messages == ENTS_MESSAGES)
			{
				more = true;
				break;
			}
		}

		gI_SentFlags[client][slot] = flags;
		gI_SentValue[client][slot] = value;
		gI_SentWait[client][slot] = waitEnd;
		len += strcopy(entries[len], sizeof(entries) - len, entry);
		count++;
		sent++;
	}

	if (count > 0)
	{
		ClientCommand(client, "bhoptools_ents %d %d %d%s", session, cmdnum, count, entries);
	}

	gI_DumpEntries[client] += sent;
	bool dump = gB_DumpOpen[client] && !more;
	SendPlayer(client, cmdnum, dump);

	if (dump)
	{
		gB_DumpOpen[client] = false;
		ClientCommand(client, "bhoptools_sync %d %d end %d", session, cmdnum, gI_DumpEntries[client]);
	}

	if (sent > 0 || dump)
	{
		Debug(client, "state after cmd %d: %d of %d entities%s", cmdnum, sent, gI_Synced, dump ? ", full" : "");
	}
}

int ReadEntity(int slot, int &value)
{
	value = 0;
	int entity = gI_SyncEntity[slot];

	if (entity == -1)
	{
		return ENT_REMOVED;
	}

	int kind = gI_SyncKind[slot];
	int flags = 0;

	if (kind != SYNC_BRANCH && kind != SYNC_CASE && GetEntData(entity, gI_KindOffset[kind][0], 1))
	{
		flags |= ENT_DISABLED;
	}

	switch (kind)
	{
		// SetTouch(NULL) when it fires, SUB_Remove 0.1s later. A trigger_multiple with a negative wait does the same.
		case SYNC_MULTIPLE, SYNC_ONCE:
		{
			if (GetEntData(entity, gI_KindOffset[kind][1]) == 0)
			{
				flags |= ENT_SPENT;
			}
		}
		case SYNC_RELAY:
		{
			if (GetEntData(entity, gI_KindOffset[kind][1], 1))
			{
				flags |= ENT_REFIRE;
			}
		}
		case SYNC_COUNTER:
		{
			if (GetEntData(entity, gI_KindOffset[kind][1], 1))
			{
				flags |= ENT_HITMIN;
			}

			if (GetEntData(entity, gI_KindOffset[kind][2], 1))
			{
				flags |= ENT_HITMAX;
			}

			value = GetEntData(entity, gI_KindOffset[kind][3]);
		}
		case SYNC_BRANCH:
		{
			if (GetEntData(entity, gI_KindOffset[kind][0], 1))
			{
				flags |= ENT_BRANCH;
			}
		}
		case SYNC_DOOR:
		{
			flags |= GetEntData(entity, gI_KindOffset[kind][1]) << ENT_DOOR_SHIFT;
			value = GetEntData(entity, gI_KindOffset[kind][2]);
		}
	}

	return flags;
}

// The client predicts the doors its +use opens. One it saw differently doesn't change here, so nothing
// would be sent: every door goes out again after a command +use went down in.
void ResendDoorsOnUse(int client, int buttons)
{
	bool held = (buttons & IN_USE) != 0;

	if (held && !gB_UseHeld[client])
	{
		for (int slot = 0; slot < gI_Synced; slot++)
		{
			if (gI_SyncKind[slot] == SYNC_DOOR)
			{
				gI_SentFlags[client][slot] = -1;
			}
		}
	}

	gB_UseHeld[client] = held;
}

// Frames after this one until the door's move, or its wait at the top, ends; -1 for none. The pusher
// moves min(time left, frametime) each frame and calls MoveDone in the frame that uses up the rest.
int DoorWait(int slot, int flags)
{
	int entity = gI_SyncEntity[slot];
	float done = GetEntDataFloat(entity, gI_KindOffset[SYNC_DOOR][2]);

	// TS_AT_BOTTOM; and a door that spawned open has never set a done time.
	if (((flags >> ENT_DOOR_SHIFT) & 3) == 1 || done <= 0.0)
	{
		return -1;
	}

	float left = done - GetEntDataFloat(entity, gI_KindOffset[SYNC_DOOR][3]);
	int frames = RoundToCeil(left / GetTickInterval() - 0.0001);
	return (frames > 1) ? frames : 1;
}

// eventqueuefix holds a trigger_multiple back from each player for its wait, counted down by one at the
// start of each of the player's commands and dropped once at most 1 is left. So one left with W after
// this command can activate again on command <cmd> + max(1, W - 1): W - 2 commands to wait. Its
// per-player timescale (shavit's laggedmovement) isn't known here and taken as 1, as the client does.
bool ReadWaits(int client)
{
	if (GetFeatureStatus(FeatureType_Native, "GetClientEvents") != FeatureStatus_Available)
	{
		return false;
	}

	any pack[8];

	if (!GetClientEvents(client, pack, sizeof(pack)))
	{
		return false;
	}

	ArrayList events = view_as<ArrayList>(pack[0]);
	ArrayList waits = view_as<ArrayList>(pack[1]);
	gI_WaitStamp++;

	for (int i = 0; i < waits.Length; i++)
	{
		int index = EntRefToEntIndex(waits.Get(i, 0));

		if (index < 0 || index >= MAX_ENTITY_INDEX || gI_EntitySlot[index] == -1)
		{
			continue;
		}

		int slot = gI_EntitySlot[index];
		int wait = RoundToCeil(view_as<float>(waits.Get(i, 1))) - 2;
		gI_SlotWait[slot] = (wait > 0) ? wait : 0;
		gI_SlotWaitStamp[slot] = gI_WaitStamp;
	}

	delete events;
	delete waits;
	return true;
}

void SendPlayer(int client, int cmdnum, bool force)
{
	float gravity = GetEntPropFloat(client, Prop_Data, "m_flGravity");
	int basevel = (GetEntityFlags(client) & FL_BASEVELOCITY) ? 1 : 0;
	char raw[64], name[96], classname[64];

	GetEntPropString(client, Prop_Data, "m_iName", raw, sizeof(raw));
	EncodeName(raw, name, sizeof(name));
	GetEntityClassname(client, raw, sizeof(raw));
	EncodeName(raw, classname, sizeof(classname));

	if (!force && gravity == gF_SentGravity[client] && basevel == gI_SentBaseVel[client]
		&& StrEqual(name, gS_SentName[client]) && StrEqual(classname, gS_SentClass[client]))
	{
		return;
	}

	gF_SentGravity[client] = gravity;
	gI_SentBaseVel[client] = basevel;
	strcopy(gS_SentName[client], sizeof(gS_SentName[]), name);
	strcopy(gS_SentClass[client], sizeof(gS_SentClass[]), classname);
	ClientCommand(client, "bhoptools_player %d %d %x %d %s %s", gI_Session[client], cmdnum, gravity, basevel, name, classname);
}

// "-" for empty, and %xx for anything that would split the arguments or the command; the engine's
// tokenizer breaks on ' ( ) { } : as well.
void EncodeName(const char[] input, char[] output, int maxlen)
{
	if (input[0] == 0)
	{
		strcopy(output, maxlen, "-");
		return;
	}

	int n = 0;

	for (int i = 0; input[i] != 0 && n < maxlen - 4; i++)
	{
		int c = input[i] & 0xFF;

		if (c <= 0x20 || c > 0x7E || c == '"' || c == ';' || c == '%' || c == '\'' || c == '(' || c == ')' || c == '{' || c == '}' || c == ':')
		{
			n += FormatEx(output[n], maxlen - n, "%%%02x", c);
		}
		else
		{
			output[n++] = c;
		}
	}

	output[n] = 0;
}
