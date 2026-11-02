// Wire values, must match the client library.
#define RS_TELEPORT (1 << 0)
#define RS_ANGLES   (1 << 1)
#define RS_START    (1 << 2)
#define RS_STAMINA  (1 << 3)
#define RS_WALK     (1 << 4)
#define RS_SS_GROUND      (1 << 5)
#define RS_SS_GROUND_KEEP (1 << 6)
#define RS_SS_AIR         (1 << 7)
#define RS_SS_AIR_KEEP    (1 << 8)

bool gB_RestartCaps[MAXPLAYERS+1];
char gS_SentRestart[MAXPLAYERS+1][128];

// Unpatched shavit: where shavit-zones restarts the player on each track, as seen on an impulse restart or
// worked out from a setstart since. shavit-zones keeps setstarts to itself and loads them from its database,
// so a track is unknown until the player restarts there once.
bool gB_DestKnown[MAXPLAYERS+1][TRACKS_SIZE];
float gF_DestPos[MAXPLAYERS+1][TRACKS_SIZE][3];
float gF_DestAng[MAXPLAYERS+1][TRACKS_SIZE][3];
bool gB_DestAngles[MAXPLAYERS+1][TRACKS_SIZE];
bool gB_DestStarts[MAXPLAYERS+1][TRACKS_SIZE];
// A delete-setstart menu may be open: which track it deletes isn't known until it's picked, so nothing is
// known or learned until the player has no menu open.
bool gB_DeleteMenu[MAXPLAYERS+1];
bool gB_InRestart[MAXPLAYERS+1];
bool gB_Restarted[MAXPLAYERS+1];
bool gB_RestartStarted[MAXPLAYERS+1];
int gI_RestartTrack[MAXPLAYERS+1];
bool gB_SetStartPending[MAXPLAYERS+1];
int gI_ZoneCount = -1;
ConVar gCV_CoreRestart;
ConVar gCV_TeleportToStart;
ConVar gCV_UnNoclipOnRestart;
ConVar gCV_ExtraSpawnHeight;

char gS_SetStartCommands[][] = { "sm_set", "sm_setstart", "sm_ss", "sm_sp", "sm_startpoint" };
char gS_DeleteStartCommands[][] = { "sm_deletestart", "sm_deletesetstart", "sm_delss", "sm_delsp" };

void FindRestartCvars()
{
	if (gCV_CoreRestart == null)
	{
		gCV_CoreRestart = FindConVar("shavit_core_restart");
	}

	if (gCV_TeleportToStart == null)
	{
		gCV_TeleportToStart = FindConVar("shavit_zones_teleporttostart");
	}

	if (gCV_UnNoclipOnRestart == null)
	{
		gCV_UnNoclipOnRestart = FindConVar("shavit_misc_unnocliponrestart");
	}

	if (gCV_ExtraSpawnHeight == null && (gCV_ExtraSpawnHeight = FindConVar("shavit_zones_extra_spawn_height")) != null)
	{
		gCV_ExtraSpawnHeight.AddChangeHook(OnSpawnHeightChanged);
	}
}

public void OnSpawnHeightChanged(ConVar convar, const char[] oldValue, const char[] newValue)
{
	for (int i = 1; i <= MaxClients; i++)
	{
		ForgetDestinations(i);
	}
}

bool RestartAvailable()
{
	FindRestartCvars();
	return gB_Zones && gCV_CoreRestart != null && gCV_TeleportToStart != null && CommandExists("sm_r") && CommandExists("sm_setstart")
		&& GetFeatureStatus(FeatureType_Native, "Shavit_GetZoneCount") == FeatureStatus_Available
		&& GetFeatureStatus(FeatureType_Native, "Shavit_IsKZMap") == FeatureStatus_Available
		&& GetFeatureStatus(FeatureType_Native, "Shavit_GetClientTimescale") == FeatureStatus_Available;
}

bool RestartAgreed(int client)
{
	return gB_RestartCaps[client] && gB_Predicting[client] && gI_Proto[client] >= 2 && (gI_ClientFlags[client] & CLIENT_RESTART);
}

void ForgetDestinations(int client)
{
	for (int track = 0; track < TRACKS_SIZE; track++)
	{
		gB_DestKnown[client][track] = false;
	}
}

// shavit-core's Command_StartTimer for sm_r: the main track for players who hate main, else the timer's.
int RestartTrack(int client)
{
	return CookieInt(client, gH_MainHaterCookie, 0) == 1 ? Track_Main : Shavit_GetClientTrack(client);
}

bool RestartNative()
{
	return GetFeatureStatus(FeatureType_Native, "Shavit_GetRestartPosition") == FeatureStatus_Available;
}

// StartTimer sets the style's gravity and UpdateLaggedMovement's value, multiplied in the same order.
int RestartState(int client, int track, float pos[3], float ang[3], float &gravity, float &lagged)
{
	if (!gB_Zones || !gCV_CoreRestart.BoolValue || !gCV_TeleportToStart.BoolValue || !IsPlayerAlive(client)
		|| StopTimerWarning(client) || track < 0 || track >= TRACKS_SIZE
		|| !(Shavit_ZoneExists(Zone_Start, track) || Shavit_IsKZMap(track)))
	{
		return 0;
	}

	float dest[3], view[3];
	bool angles, starts;

	if (RestartNative())
	{
		if (!Shavit_GetRestartPosition(client, track, dest, view, angles, starts))
		{
			return 0;
		}
	}
	// unpatched shavit
	else if (gB_DeleteMenu[client] || !gB_DestKnown[client][track])
	{
		return 0;
	}
	else
	{
		dest = gF_DestPos[client][track];
		view = gF_DestAng[client][track];
		angles = gB_DestAngles[client][track];
		starts = gB_DestStarts[client][track];
	}

	int flags = RS_TELEPORT;
	pos = dest;

	if (angles)
	{
		flags |= RS_ANGLES;
		ang = view;
	}

	if (starts)
	{
		int style = Shavit_GetBhopStyle(client);
		float stylescale = Shavit_GetStyleSettingFloat(style, "timescale") * Shavit_GetStyleSettingFloat(style, "speed");
		flags |= RS_START;
		gravity = Shavit_GetStyleSettingFloat(style, "gravity");
		lagged = Shavit_GetClientTimescale(client) * stylescale;
	}

	// shavit-misc's Shavit_OnRestart.
	if (gCV_UnNoclipOnRestart != null)
	{
		flags |= RS_STAMINA | (gCV_UnNoclipOnRestart.BoolValue ? RS_WALK : 0);
	}

	// Command_SetStart saves for the timer's track. In the air it keeps the start zone's centre, which
	// a destination that doesn't start the timer isn't, and a custom spawn (no angles) ignores it.
	if (Shavit_GetClientTrack(client) != track)
	{
		flags |= RS_SS_GROUND_KEEP | RS_SS_AIR_KEEP;
	}
	else
	{
		flags |= RS_SS_GROUND;

		if (starts)
		{
			flags |= RS_SS_AIR;
		}
		else if (!angles)
		{
			flags |= RS_SS_AIR_KEEP;
		}
	}

	return flags;
}

void SendRestart(int client, int cmdnum, bool force)
{
	if (!RestartAgreed(client))
	{
		return;
	}

	// Zone edits move start zones and custom spawns.
	int zones = gB_Zones ? Shavit_GetZoneCount() : 0;

	if (zones != gI_ZoneCount)
	{
		gI_ZoneCount = zones;

		for (int i = 1; i <= MaxClients; i++)
		{
			ForgetDestinations(i);
		}
	}

	if (gB_DeleteMenu[client] && GetClientMenu(client) == MenuSource_None)
	{
		gB_DeleteMenu[client] = false;
	}

	float pos[3], ang[3], gravity, lagged;
	int flags = RestartState(client, RestartTrack(client), pos, ang, gravity, lagged);
	char state[128];
	FormatEx(state, sizeof(state), "%d %x %x %x %x %x %x %x", flags, pos[0], pos[1], pos[2], ang[0], ang[1], gravity, lagged);

	if (!force && StrEqual(state, gS_SentRestart[client]))
	{
		return;
	}

	strcopy(gS_SentRestart[client], sizeof(gS_SentRestart[]), state);
	ClientCommand(client, "bhoptools_restart %d %d %s", gI_Session[client], cmdnum, state);
	Debug(client, "restart after cmd %d: flags %d, to %.1f %.1f %.1f", cmdnum, flags, pos[0], pos[1], pos[2]);
}

public void Shavit_OnRestart(int client, int track)
{
	if (gB_InRestart[client])
	{
		gB_Restarted[client] = true;
		gI_RestartTrack[client] = track;
	}
}

public Action Shavit_OnStart(int client, int track)
{
	if (gB_InRestart[client])
	{
		gB_RestartStarted[client] = true;
	}

	return Plugin_Continue;
}

// Through shavit-core's sm_r, before the move like the client, so its checks and side effects happen on
// this command. What it did is read back from Shavit_OnRestart and the fixangle its TeleportEntity leaves.
void Restart(int client, int cmdnum, float angles[3])
{
	float before[3], after[3], eye[3], velocity[3];
	GetClientAbsOrigin(client, before);
	int fixangle = GetEntData(client, gI_PlayerState + FIXANGLE_OFFSET);
	bool tpcomp = GetFeatureStatus(FeatureType_Native, "TPComp_Suspend") == FeatureStatus_Available;

	// The client turns its own view: tpcomp mustn't compensate it, and no snap goes out.
	if (tpcomp)
	{
		TPComp_Suspend(client, true);
	}

	gB_InRestart[client] = true;
	gB_Restarted[client] = false;
	gB_RestartStarted[client] = false;
	FakeClientCommand(client, "sm_r");
	gB_InRestart[client] = false;

	GetClientAbsOrigin(client, after);
	GetClientEyeAngles(client, eye);
	GetEntPropVector(client, Prop_Data, "m_vecAbsVelocity", velocity);
	bool done = gB_Restarted[client] && IsPlayerAlive(client) && GetVectorLength(velocity) == 0.0;
	bool turned = done && fixangle != 1 && GetEntData(client, gI_PlayerState + FIXANGLE_OFFSET) == 1;

	if (turned)
	{
		SetEntData(client, gI_PlayerState + FIXANGLE_OFFSET, 0);
		// This command moves with the new view on the client too.
		angles[0] = eye[0];
		angles[1] = eye[1];
	}

	if (tpcomp)
	{
		TPComp_Suspend(client, false);
	}

	int flags = 0;
	float gravity, lagged;

	if (done)
	{
		flags = RS_TELEPORT | (turned ? RS_ANGLES : 0) | (gB_RestartStarted[client] ? RS_START : 0)
			| (GetEntPropFloat(client, Prop_Send, "m_flStamina") == 0.0 ? RS_STAMINA : 0)
			| (GetEntityMoveType(client) == MOVETYPE_WALK ? RS_WALK : 0);
		gravity = GetEntPropFloat(client, Prop_Data, "m_flGravity");
		lagged = GetEntPropFloat(client, Prop_Data, "m_flLaggedMovementValue");

		// unpatched shavit
		if (!RestartNative())
		{
			LearnDestination(client, gI_RestartTrack[client], after, eye, turned, GetVectorDistance(before, after) > 0.0);
		}
	}

	ClientCommand(client, "bhoptools_ack %d %d r %d %d %x %x %x %x %x %x %x", gI_Session[client], cmdnum, done ? 1 : 0, flags,
		after[0], after[1], after[2], eye[0], eye[1], gravity, lagged);
	Debug(client, "restart %s, at %.1f %.1f %.1f, flags %d", done ? "done" : "refused", after[0], after[1], after[2], flags);
}

// Only from a restart that moved or turned the player: pressed again at the destination, one that didn't
// teleport would look the same.
void LearnDestination(int client, int track, float pos[3], float ang[3], bool turned, bool moved)
{
	if ((!turned && !moved) || gB_DeleteMenu[client] || track < 0 || track >= TRACKS_SIZE)
	{
		return;
	}

	gB_DestKnown[client][track] = true;
	gF_DestPos[client][track] = pos;
	gF_DestAng[client][track] = ang;
	gB_DestAngles[client][track] = turned;
	gB_DestStarts[client][track] = gB_RestartStarted[client];
}

void ApplySetStart(int client)
{
	int track = Shavit_GetClientTrack(client);

	if (!IsPlayerAlive(client) || gB_DeleteMenu[client] || track < 0 || track >= TRACKS_SIZE)
	{
		return;
	}

	float angles[3];
	GetClientEyeAngles(client, angles);

	if (GetEntPropEnt(client, Prop_Send, "m_hGroundEntity") != -1)
	{
		GetClientAbsOrigin(client, gF_DestPos[client][track]);
		gF_DestPos[client][track][2] += 1.0;
		gB_DestKnown[client][track] = true;
		gB_DestStarts[client][track] = false;
	}
	else if (!gB_DestKnown[client][track] || (!gB_DestStarts[client][track] && !gB_DestAngles[client][track]))
	{
		// Unknown stays unknown, and a custom spawn ignores the view.
		return;
	}
	else if (!gB_DestStarts[client][track])
	{
		// Replaces a setstart with a position: the start zone's centre it goes back to hasn't been seen.
		gB_DestKnown[client][track] = false;
		return;
	}

	gF_DestAng[client][track] = angles;
	gB_DestAngles[client][track] = true;
}

void SetStartImpulse(int client, int cmdnum)
{
	FakeClientCommand(client, "sm_setstart");

	// unpatched shavit
	if (!RestartNative())
	{
		ApplySetStart(client);
	}

	SendRestart(client, cmdnum, true);
}

void SetStartCommand(int client, const char[] command)
{
	// unpatched shavit
	if (RestartNative())
	{
		return;
	}

	for (int i = 0; i < sizeof(gS_SetStartCommands); i++)
	{
		if (StrEqual(command, gS_SetStartCommands[i], false))
		{
			ApplySetStart(client);
			return;
		}
	}

	for (int i = 0; i < sizeof(gS_DeleteStartCommands); i++)
	{
		if (StrEqual(command, gS_DeleteStartCommands[i], false))
		{
			gB_DeleteMenu[client] = true;
			ForgetDestinations(client);
			return;
		}
	}
}

// Before the command runs, from the same state it reads.
public Action Listener_SetStart(int client, const char[] command, int argc)
{
	if (client > 0 && IsClientInGame(client))
	{
		SetStartCommand(client, command);
	}

	return Plugin_Continue;
}

// Chat triggers ("!sp", "/delss") arrive as say. Seen twice if the command they run is listened to as
// well, which does nothing more.
public Action Listener_Say(int client, const char[] command, int argc)
{
	if (client <= 0 || !IsClientInGame(client))
	{
		return Plugin_Continue;
	}

	char text[32];
	GetCmdArgString(text, sizeof(text));
	StripQuotes(text);
	TrimString(text);

	if (text[0] != '!' && text[0] != '/')
	{
		return Plugin_Continue;
	}

	char name[40];
	FormatEx(name, sizeof(name), "sm_%s", text[1]);
	int space = FindCharInString(name, ' ');

	if (space != -1)
	{
		name[space] = 0;
	}

	SetStartCommand(client, name);
	return Plugin_Continue;
}
