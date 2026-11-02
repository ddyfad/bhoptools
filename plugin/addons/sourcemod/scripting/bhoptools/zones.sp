// bhoptools_zplayer flags; must match the client library.
#define ZSP_AUTO           (1 << 0)
#define ZSP_STYLE_AUTOBHOP (1 << 1)
#define ZSP_STYLE_EASYBHOP (1 << 2)
#define ZSP_PAUSED         (1 << 3)
#define ZSP_ENFORCE        (1 << 4)
#define ZSP_STOPPED        (1 << 5)
#define ZSP_TEAM           (1 << 6)

#define ZONES_FORMAT 1
// Published next to the mapdiff payload (MAPDIFF_MAX), within the signon message's 288000 bytes.
#define ZONES_MAX 32000

ConVar gCV_BoxOffset;
ConVar gCV_EnforceTracks;
ConVar gCV_PreSpeed;
ConVar gCV_PrestrafeLimit;
ConVar gCV_BlockPreJump;
ConVar gCV_NoZAxisSpeed;
ConVar gCV_Accelerate;
ConVar gCV_Friction;

char gS_Zones[ZONES_MAX];
int gI_ZonesCrc;
int gI_ZoneSeq;
bool gB_ZonesPublished;
bool gB_ZonesOver;
char gS_SentZPlayer[MAXPLAYERS+1][160];

void FindZoneCvars()
{
	gCV_BoxOffset = FindConVar("shavit_zones_box_offset");
	gCV_EnforceTracks = FindConVar("shavit_zones_enforcetracks");
	gCV_PreSpeed = FindConVar("shavit_misc_prespeed");
	gCV_PrestrafeLimit = FindConVar("shavit_misc_prestrafelimit");
	gCV_BlockPreJump = FindConVar("shavit_core_blockprejump");
	gCV_NoZAxisSpeed = FindConVar("shavit_core_nozaxisspeed");
	gCV_Accelerate = FindConVar("sv_accelerate");
	gCV_Friction = FindConVar("sv_friction");
}

void ResetZonePlayer(int client)
{
	gS_SentZPlayer[client][0] = 0;
}

void BuildZonePayload(int index, char[] buffer, int maxlen)
{
	zone_cache_t cache;
	Shavit_GetZone(index, cache);

	float origin[3], half[3];
	AddVectors(cache.fCorner1, cache.fCorner2, origin);
	ScaleVector(origin, 0.5);
	bool offset = gCV_BoxOffset != null && gCV_BoxOffset.FloatValue != 0.0;

	for (int i = 0; i < 3; i++)
	{
		float sub = offset ? ((i < 2) ? 16.0 : 31.0) : 0.0;
		half[i] = FloatAbs(cache.fCorner1[i] - cache.fCorner2[i]) / 2.0;

		if (half[i] > sub)
		{
			half[i] -= sub;
		}
	}

	int hammerid = 0;

	if ((cache.iForm == ZoneForm_trigger_multiple || cache.iForm == ZoneForm_trigger_teleport)
		&& cache.iEntity > MaxClients && IsValidEntity(cache.iEntity))
	{
		hammerid = GetEntProp(cache.iEntity, Prop_Data, "m_iHammerID");
	}

	FormatEx(buffer, maxlen, "%d %d %d %d %d %x %x %x %x %x %x %x %x %x", cache.iType, cache.iTrack, cache.iForm, cache.iData, hammerid,
		origin[0], origin[1], origin[2], half[0], half[1], half[2],
		cache.fDestination[0], cache.fDestination[1], cache.fDestination[2]);
}

void UpdateZones()
{
	InitCrc32();

	int count = gB_Zones ? Shavit_GetZoneCount() : 0;
	int len = 0;
	char payload[160];
	bool over = false;

	for (int i = 0; i < count; i++)
	{
		BuildZonePayload(i, payload, sizeof(payload));

		if (len + strlen(payload) + 1 >= sizeof(gS_Zones))
		{
			over = true;
			break;
		}

		len += FormatEx(gS_Zones[len], sizeof(gS_Zones) - len, "%s\n", payload);
	}

	if (over)
	{
		if (!gB_ZonesOver)
		{
			gB_ZonesOver = true;
			LogError("zones: the zone list is over %d bytes and isn't published", ZONES_MAX);
		}

		return;
	}

	gB_ZonesOver = false;
	int crc = Crc32String(-1, gS_Zones);

	if (crc != gI_ZonesCrc)
	{
		gI_ZonesCrc = crc;
		gI_ZoneSeq++;
		gB_ZonesPublished = false;
	}

	if (gB_ZonesPublished)
	{
		return;
	}

	char info[32];
	FormatEx(info, sizeof(info), "%d %d", ZONES_FORMAT, gI_ZoneSeq);
	gB_ZonesPublished = PublishTable("bhoptools_zones", gS_Zones, len, info);

	if (gB_ZonesPublished && gCV_Log.BoolValue)
	{
		LogToFileEx(gS_LogPath, "zones %d: %d zones, %d bytes", gI_ZoneSeq, count, len);
	}
}

// shavit-misc's prestrafe limit before a start zone's own (Shavit_OnUserCmdPre, Shavit_OnStartPre).
float PrestrafeLimit(int style)
{
	float runspeed = Shavit_GetStyleSettingFloat(style, "runspeed");
	float limit = runspeed + (gCV_PrestrafeLimit != null ? gCV_PrestrafeLimit.FloatValue : 30.0);
	float cfg = Shavit_GetStyleSettingFloat(style, "maxprestrafe");
	float max = MaxPrestrafe(runspeed, gCV_Accelerate.FloatValue, gCV_Friction.FloatValue, GetTickInterval());

	if (cfg > 0.0)
	{
		return cfg;
	}

	return limit > max ? max : limit;
}

int StyleOrCvar(int style, const char[] setting, ConVar cvar)
{
	int value = Shavit_GetStyleSettingInt(style, setting);
	return (value < 0 && cvar != null) ? cvar.IntValue : value;
}

void SendZonePlayer(int client, int cmdnum)
{
	if (!gB_Predicting[client] || gI_Proto[client] < 2 || !(gI_ClientFlags[client] & CLIENT_ZONES))
	{
		return;
	}

	if (gCV_PauseMovement == null)
	{
		gCV_PauseMovement = FindConVar("shavit_core_pause_movement");
	}

	int style = Shavit_GetBhopStyle(client);
	int flags = 0;

	if (AutoToggle(client)) flags |= ZSP_AUTO;
	if (Shavit_GetStyleSettingBool(style, "autobhop")) flags |= ZSP_STYLE_AUTOBHOP;
	if (Shavit_GetStyleSettingBool(style, "easybhop")) flags |= ZSP_STYLE_EASYBHOP;
	if (Shavit_IsPaused(client) && gCV_PauseMovement != null && (gCV_PauseMovement.IntValue == 0 || gCV_PauseMovement.IntValue == 3)) flags |= ZSP_PAUSED;
	if (gCV_EnforceTracks == null || gCV_EnforceTracks.BoolValue) flags |= ZSP_ENFORCE;
	if (Shavit_GetTimerStatus(client) == Timer_Stopped) flags |= ZSP_STOPPED;
	if (GetClientTeam(client) >= 2) flags |= ZSP_TEAM;

	char state[160];
	FormatEx(state, sizeof(state), "%d %d %x %x %d %d %d %d %x", Shavit_GetClientTrack(client), flags,
		Shavit_GetStyleSettingFloat(style, "gravity"), Shavit_GetStyleSettingFloat(style, "airaccelerate"),
		Shavit_GetStyleSettingInt(style, "prespeed"), StyleOrCvar(style, "prespeed_type", gCV_PreSpeed),
		StyleOrCvar(style, "blockprejump", gCV_BlockPreJump), StyleOrCvar(style, "nozaxisspeed", gCV_NoZAxisSpeed),
		PrestrafeLimit(style));

	if (!StrEqual(state, gS_SentZPlayer[client]))
	{
		strcopy(gS_SentZPlayer[client], sizeof(gS_SentZPlayer[]), state);
		ClientCommand(client, "bhoptools_zplayer %d %d %s", gI_Session[client], cmdnum, state);
		Debug(client, "zone player %s", state);
	}
}
