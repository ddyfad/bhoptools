// Must match the client library.
#define IMPULSE_SAVE 150
#define IMPULSE_TELE 151
#define IMPULSE_PREV 152
#define IMPULSE_NEXT 153
#define IMPULSE_NOCLIP 154
#define IMPULSE_NOCLIP_ON 155
#define IMPULSE_NOCLIP_OFF 156
#define IMPULSE_RESTART 157
#define IMPULSE_SETSTART 158

// Must match the client library.
#define OLD_FRAME_WINDOW 4

#define PROTO 2

// Wire values
#define CAPS_EVENTQUEUEFIX (1 << 0)
#define CAPS_PREVNEXT      (1 << 1)
#define CAPS_RNGFIX        (1 << 2)
#define CAPS_AUTOBHOP      (1 << 3)
#define CAPS_NOCLIP        (1 << 4)
#define CAPS_MOVE          (1 << 5)
#define CAPS_UNTOUCH       (1 << 6)
#define CAPS_MSF           (1 << 7)
#define CAPS_MPBHOPS       (1 << 8)
#define CAPS_STATE         (1 << 9)
#define CAPS_CPSET         (1 << 10)
#define CAPS_RESTART       (1 << 11)
#define CAPS_KEYS          (1 << 12)
#define CAPS_AUTOSYNC      (1 << 13)
#define CAPS_MAPDIFF       (1 << 14)
#define CAPS_ZONES         (1 << 16)
#define CAPS_TELEPORT_VIEW (1 << 17)

#define CLIENT_PREDICTS_TRIGGER_TELEPORTS (1 << 0)
#define CLIENT_CPMENU (1 << 1)
#define CLIENT_RNGFIX (1 << 2)
#define CLIENT_AUTOBHOP (1 << 3)
#define CLIENT_NOCLIP (1 << 4)
#define CLIENT_MOVE (1 << 5)
#define CLIENT_UNTOUCH (1 << 6)
#define CLIENT_MSF (1 << 7)
#define CLIENT_STATE (1 << 8)
#define CLIENT_CPSET (1 << 9)
#define CLIENT_RESTART (1 << 10)
#define CLIENT_KEYS (1 << 11)
#define CLIENT_AUTOSYNC (1 << 12)
#define CLIENT_ZONES (1 << 15)
#define CLIENT_TELEPORT_VIEW (1 << 16)
#define CLIENT_MENU_SOUND (1 << 17)

// CPlayerState::fixangle, from pl.deadflag, 32-bit only
#define FIXANGLE_OFFSET 20

#if !defined _tpcomp_included
native void TPComp_Suspend(int client, bool suspend);
// Same as tpcomp.inc, for building without it.
native void TPComp_SetClientPredicts(int client, bool predicts);
#endif

ConVar gCV_Debug;
ConVar gCV_Log;
char gS_LogPath[PLATFORM_MAX_PATH];
ConVar gCV_StopTimerWarning;
ConVar gCV_PauseMovement;
Cookie gH_CheckpointsCookie;
Cookie gH_AutoBhopCookie;
int gI_PlayerState;
Cookie gH_MainHaterCookie;

bool gB_Predicting[MAXPLAYERS+1];
int gI_Session[MAXPLAYERS+1];
int gI_Proto[MAXPLAYERS+1];
int gI_ClientFlags[MAXPLAYERS+1];
int gI_LastImpulseCmd[MAXPLAYERS+1];
int gI_LastCmd[MAXPLAYERS+1];

bool gB_InCmd[MAXPLAYERS+1];
int gI_RunCmd[MAXPLAYERS+1];
bool gB_OwnTeleport[MAXPLAYERS+1];

bool gB_Zones;
GlobalForward gH_OnClientFlags;

// Clients stop sending impulses a server without the plugin would ignore.
public void OnPluginEnd()
{
	for (int i = 1; i <= MaxClients; i++)
	{
		if (IsClientInGame(i) && gB_Predicting[i] && gI_Proto[i] >= 2)
		{
			ClientCommand(i, "bhoptools_caps 0 %d 0 0", gI_Session[i]);
			Debug(i, "plugin unloading, told the client to stop");
		}
	}
}

public void OnClientDisconnect(int client)
{
	if (gI_ClientFlags[client] & CLIENT_PREDICTS_TRIGGER_TELEPORTS)
	{
		gI_ClientFlags[client] = 0;
		UpdateTPComp(client);
	}
}

// By filename or name: it registers no library.
bool MpbhopsLoaded()
{
	char file[PLATFORM_MAX_PATH], name[64];
	bool found = false;
	Handle it = GetPluginIterator();

	while (!found && MorePlugins(it))
	{
		Handle plugin = ReadPlugin(it);

		if (GetPluginStatus(plugin) != Plugin_Running)
		{
			continue;
		}

		GetPluginFilename(plugin, file, sizeof(file));
		found = StrContains(file, "mpbhops_but_working", false) != -1
			|| (GetPluginInfo(plugin, PlInfo_Name, name, sizeof(name)) && StrEqual(name, "MPBHOPS, BUT WORKING"));
	}

	delete it;
	return found;
}

void UpdateTPComp(int client)
{
	if (LibraryExists("tpcomp") && GetFeatureStatus(FeatureType_Native, "TPComp_SetClientPredicts") == FeatureStatus_Available)
	{
		TPComp_SetClientPredicts(client, (gI_ClientFlags[client] & CLIENT_PREDICTS_TRIGGER_TELEPORTS) != 0);
	}
}

void FindCookies()
{
	if (GetFeatureStatus(FeatureType_Native, "FindClientCookie") != FeatureStatus_Available)
	{
		return;
	}

	if (gH_CheckpointsCookie == null)
	{
		gH_CheckpointsCookie = FindClientCookie("shavit_checkpoints");
	}

	if (gH_AutoBhopCookie == null)
	{
		gH_AutoBhopCookie = FindClientCookie("shavit_autobhop");
	}

	if (gH_MainHaterCookie == null)
	{
		gH_MainHaterCookie = FindClientCookie("shavit_mainhater");
	}
}

// shavit reads these cookies once cached and takes an empty one (or none yet) as the default,
// and writes them as they change, so they hold what it uses.
int CookieInt(int client, Cookie cookie, int def)
{
	if (cookie == null || !AreClientCookiesCached(client))
	{
		return def;
	}

	char value[16];
	GetClientCookie(client, cookie, value, sizeof(value));
	return (value[0] != 0) ? StringToInt(value) : def;
}

// Mirrors shavit-misc's ShouldDisplayStopWarning.
bool StopTimerWarning(int client)
{
	if (GetFeatureStatus(FeatureType_Native, "Shavit_ShouldDisplayStopWarning") == FeatureStatus_Available)
	{
		return Shavit_ShouldDisplayStopWarning(client);
	}

	// unpatched shavit
	return gCV_StopTimerWarning != null && gCV_StopTimerWarning.BoolValue && !Shavit_IsPracticeMode(client)
		&& Shavit_GetTimerStatus(client) != Timer_Stopped && Shavit_GetClientTime(client) > gCV_StopTimerWarning.FloatValue
		&& !Shavit_GetStyleSettingBool(Shavit_GetBhopStyle(client), "segments");
}

float NormalizeAngle(float angle)
{
	while (angle > 180.0)
	{
		angle -= 360.0;
	}

	while (angle < -180.0)
	{
		angle += 360.0;
	}

	return angle;
}

void Debug(int client, const char[] format, any ...)
{
	if (!gCV_Debug.BoolValue && !gCV_Log.BoolValue)
	{
		return;
	}

	char buffer[256];
	VFormat(buffer, sizeof(buffer), format, 3);

	if (gCV_Debug.BoolValue)
	{
		PrintToConsole(client, "[bhoptools server] tick %d: %s", GetGameTickCount(), buffer);
	}

	if (gCV_Log.BoolValue)
	{
		LogToFileEx(gS_LogPath, "%L tick %d: %s", client, GetGameTickCount(), buffer);
		PrintToServer("[bhoptools] %N tick %d: %s", client, GetGameTickCount(), buffer);
	}
}
