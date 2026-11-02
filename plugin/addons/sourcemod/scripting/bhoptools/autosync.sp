// Wire values, must match the client library.
#define AUTOSYNC_BEFORE_CORE     0
#define AUTOSYNC_BEFORE_BHOPTOOLS 1
#define AUTOSYNC_AFTER_BHOPTOOLS  2

Handle gH_Syncstyle;
// shavit-syncstyle 1.2.1 and later only count teleports that move the player.
bool gB_SyncMovesOnly;
int gI_SyncOrder;
ConVar gCV_SyncString;
// Its sm_synctoggle, mirrored: it keeps the setting to itself.
bool gB_SyncOff[MAXPLAYERS+1];
// Its teleport count comes from a hook on CBaseEntity::Teleport; ours sees the same teleports. The
// command it reads the count at 0 on: the one a teleport happened before it ran on, else the next.
DynamicHook gH_TeleportHook;
int gI_SyncZero[MAXPLAYERS+1];
bool gB_SyncZeroNext[MAXPLAYERS+1];
int gI_SentAutosync[MAXPLAYERS+1];
int gI_SentSyncOrder[MAXPLAYERS+1];
int gI_SentSyncZero[MAXPLAYERS+1];

// shavit-syncstyle's autosync applies: its style's specialstring has the "autostrafer" cvar in it and
// sm_synctoggle is on. Whether it's alive, on the ground and so on the client works out itself.
bool AutosyncOn(int client)
{
	return gH_Syncstyle != null && !gB_SyncOff[client] && IsAutosyncStyle(Shavit_GetBhopStyle(client));
}

bool IsAutosyncStyle(int style)
{
	if (gCV_SyncString == null)
	{
		gCV_SyncString = FindConVar("autostrafer");
	}

	if (style < 0 || gCV_SyncString == null)
	{
		return false;
	}

	char special[256], value[sizeof(stylestrings_t::sSpecialString)];
	gCV_SyncString.GetString(value, sizeof(value));
	Shavit_GetStyleStrings(style, sSpecialString, special, sizeof(special));
	return StrContains(special, value, false) != -1;
}

void SendAutosync(int client, int cmdnum)
{
	if (!gB_Predicting[client] || gI_Proto[client] < 2 || !(gI_ClientFlags[client] & CLIENT_AUTOSYNC) || gH_TeleportHook == null)
	{
		return;
	}

	int on = AutosyncOn(client) ? 1 : 0;

	if (on != gI_SentAutosync[client] || (on && (gI_SyncOrder != gI_SentSyncOrder[client] || gI_SyncZero[client] != gI_SentSyncZero[client])))
	{
		gI_SentAutosync[client] = on;
		gI_SentSyncOrder[client] = gI_SyncOrder;
		gI_SentSyncZero[client] = gI_SyncZero[client];
		ClientCommand(client, "bhoptools_autosync %d %d %d %d %d %d", gI_Session[client], cmdnum, on, gI_SyncOrder, gI_SyncZero[client], gB_SyncMovesOnly);
		Debug(client, "autosync %d, order %d, teleport count 0 at %d", on, gI_SyncOrder, gI_SyncZero[client]);
	}
}

void HookTeleport(int client)
{
	if (gH_TeleportHook != null)
	{
		gH_TeleportHook.HookEntity(Hook_Post, client, Hook_TeleportPost);
		gH_TeleportHook.HookEntity(Hook_Pre, client, Hook_TeleportViewPre);
		gH_TeleportHook.HookEntity(Hook_Post, client, Hook_TeleportViewPost);
	}
}

// shavit-syncstyle zeroes its count right here. Teleports inside a command after it has run are read
// on the next command, as are ours when it runs before bhoptools. A plugin teleporting in its own
// OnPlayerRunCmd is taken to run on the same side of it as bhoptools.
public MRESReturn Hook_TeleportPost(int client, DHookParam params)
{
	if (gB_SyncMovesOnly && params.IsNull(1))
	{
		return MRES_Ignored;
	}

	if (!gB_InCmd[client])
	{
		gB_SyncZeroNext[client] = true;
	}
	else
	{
		gI_SyncZero[client] = (gB_OwnTeleport[client] && gI_SyncOrder == AUTOSYNC_AFTER_BHOPTOOLS) ? gI_RunCmd[client] : gI_RunCmd[client] + 1;
	}

	return MRES_Ignored;
}

public Action Command_SyncToggle(int client, int args)
{
	if (client > 0 && IsClientInGame(client) && IsPlayerAlive(client) && IsAutosyncStyle(Shavit_GetBhopStyle(client)))
	{
		gB_SyncOff[client] = !gB_SyncOff[client];
	}

	return Plugin_Continue;
}

public void Shavit_OnStyleChanged(int client, int oldstyle, int newstyle, int track, bool manual)
{
	if (IsClientInGame(client) && IsPlayerAlive(client) && IsAutosyncStyle(newstyle))
	{
		gB_SyncOff[client] = false;
	}
}

// Forwards run in plugin load order, which is the order the iterator gives. `unloading` is left out.
// Loading it again starts every player's sm_synctoggle over.
void UpdateSyncstyle(Handle unloading = null)
{
	Handle found = null;
	bool core = false, self = false;
	int order = AUTOSYNC_BEFORE_CORE;
	Handle me = GetMyHandle();
	Handle iter = GetPluginIterator();
	char file[PLATFORM_MAX_PATH], name[64];

	while (found == null && MorePlugins(iter))
	{
		Handle plugin = ReadPlugin(iter);

		if (plugin == me)
		{
			self = true;
			continue;
		}

		if (plugin == unloading || GetPluginStatus(plugin) != Plugin_Running)
		{
			continue;
		}

		GetPluginFilename(plugin, file, sizeof(file));
		int slash = FindCharInString(file, '/', true);
		if (!GetPluginInfo(plugin, PlInfo_Name, name, sizeof(name)))
		{
			name[0] = 0;
		}

		if (StrEqual(file[slash + 1], "shavit-core.smx", false) || StrEqual(name, "[shavit] Core"))
		{
			core = true;
		}
		else if (StrEqual(file[slash + 1], "shavit-syncstyle.smx", false) || StrEqual(name, "Sync style for shavit."))
		{
			found = plugin;
			order = self ? AUTOSYNC_AFTER_BHOPTOOLS : (core ? AUTOSYNC_BEFORE_BHOPTOOLS : AUTOSYNC_BEFORE_CORE);
		}
	}

	delete iter;

	if (found != gH_Syncstyle)
	{
		for (int i = 1; i <= MaxClients; i++)
		{
			gB_SyncOff[i] = false;
			gI_SentAutosync[i] = -1;
		}
	}

	gH_Syncstyle = found;
	gI_SyncOrder = order;

	char version[16];
	gB_SyncMovesOnly = found != null && GetPluginInfo(found, PlInfo_Version, version, sizeof(version)) && !StrEqual(version, "1.2.0");
}
