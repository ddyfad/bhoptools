#include <sourcemod>
#include <sdktools>
#include <sdkhooks>
#include <dhooks>
#include <shavit/core>
#include <shavit/checkpoints>
#undef REQUIRE_PLUGIN
#tryinclude <tpcomp>
#include <shavit/replay-recorder>
#include <shavit/zones>
#include <shavit/misc>
#define REQUIRE_PLUGIN
#undef REQUIRE_EXTENSIONS
#include <clientprefs>
#define REQUIRE_EXTENSIONS

// Natives a patched shavit can add; stock shavit has none of them, and bhoptools works out the same
// things itself without them.
native bool Shavit_GetAutobhop(int client);
native int Shavit_GetCheckpointOptions(int client);
native bool Shavit_GetRestartPosition(int client, int track, float pos[3], float ang[3], bool &setsangles = false, bool &startstimer = false);
native bool Shavit_ShouldDisplayStopWarning(int client);
// What sm_nc and +noclip would do now, as NoclipAccess_* flags.
native int Shavit_GetNoclipAccess(int client);

enum
{
	NoclipAccess_Enter = (1 << 0), // sm_nc enters noclip, instead of opening the stop timer warning
	NoclipAccess_Leave = (1 << 1), // sm_nc leaves noclip
	NoclipAccess_Plus = (1 << 2)   // +noclip enters noclip, instead of opening the stop timer warning
};

#pragma semicolon 1
#pragma newdecls required

public Plugin myinfo =
{
	name = "bhoptools",
	author = "daf",
	description = "Server side of client-predicted checkpoint saves, teleports, noclip and restarts",
	version = "1.0",
	url = "https://github.com/ddyfad/bhoptools"
};

#include "bhoptools/core.sp"
#include "bhoptools/frames.sp"
#include "bhoptools/checkpoints.sp"
#include "bhoptools/autobhop.sp"
#include "bhoptools/noclip.sp"
#include "bhoptools/restart.sp"
#include "bhoptools/move.sp"
#include "bhoptools/keys.sp"
#include "bhoptools/autosync.sp"
#include "bhoptools/rngfix.sp"
#include "bhoptools/msf.sp"
#include "bhoptools/state.sp"
#include "bhoptools/fixes.sp"
#include "bhoptools/tables.sp"
#include "bhoptools/mapdiff.sp"
#include "bhoptools/zones.sp"
#include "bhoptools/teleportview.sp"

public APLRes AskPluginLoad2(Handle myself, bool late, char[] error, int err_max)
{
	MarkNativeAsOptional("TPComp_Suspend");
	MarkNativeAsOptional("TPComp_SetClientPredicts");
	MarkNativeAsOptional("SuperLandfix_GetMethod");
	MarkNativeAsOptional("GetClientEvents");
	MarkNativeAsOptional("Shavit_IsKZMap");
	MarkNativeAsOptional("Shavit_GetClientTimescale");
	MarkNativeAsOptional("Shavit_GetAutobhop");
	MarkNativeAsOptional("Shavit_GetCheckpointOptions");
	MarkNativeAsOptional("Shavit_GetRestartPosition");
	MarkNativeAsOptional("Shavit_ShouldDisplayStopWarning");
	MarkNativeAsOptional("Shavit_GetNoclipAccess");
	Frames_AskPluginLoad2();
	CreateNative("Bhoptools_GetClientFlags", Native_GetClientFlags);
	CreateNative("Bhoptools_GetSession", Native_GetSession);
	RegPluginLibrary("bhoptools");
	return APLRes_Success;
}

public void OnPluginStart()
{
	RegConsoleCmd("bhoptools_hello", Command_Hello);
	RegConsoleCmd("bhoptools_resync", Command_Resync);
	Frames_OnPluginStart();
	gH_OnClientFlags = new GlobalForward("Bhoptools_OnClientFlags", ET_Ignore, Param_Cell, Param_Cell);
	// Next to shavit-syncstyle's own handler, which doesn't stop others running.
	RegConsoleCmd("sm_synctoggle", Command_SyncToggle);

	for (int i = 0; i < sizeof(gS_SetStartCommands); i++)
	{
		AddCommandListener(Listener_SetStart, gS_SetStartCommands[i]);
	}

	for (int i = 0; i < sizeof(gS_DeleteStartCommands); i++)
	{
		AddCommandListener(Listener_SetStart, gS_DeleteStartCommands[i]);
	}

	AddCommandListener(Listener_Say, "say");
	AddCommandListener(Listener_Say, "say_team");
	LoadTranslations("shavit-misc.phrases");

	gCV_Debug = CreateConVar("sm_bhoptools_debug", "0", "Print what the server does with each impulse to the player's console.", 0, true, 0.0, true, 1.0);
	gCV_Log = CreateConVar("sm_bhoptools_log", "0", "Log every save, teleport and protocol message to logs/bhoptools.log.", 0, true, 0.0, true, 1.0);
	BuildPath(Path_SM, gS_LogPath, sizeof(gS_LogPath), "logs/bhoptools.log");
	gCV_RefusedSnap = CreateConVar("sm_bhoptools_refused_snap", "1", "When a teleport the client predicted is refused, snap an old (protocol 1) client's view back to where the command was aiming. Protocol 2 clients turn back themselves.", 0, true, 0.0, true, 1.0);
	gCV_Quiet = CreateConVar("sm_bhoptools_quiet", "1", "Don't print shavit's \"teleported to checkpoint\" chat line for impulse teleports.", 0, true, 0.0, true, 1.0);
	gCV_DuckTeleportFix = CreateConVar("sm_bhoptools_duck_teleport_fix", "1", "Stop the engine snapping a player into the floor after a trigger teleport on the command they duck or unduck (it seeds the physics shadow with their fall velocity at the destination).", 0, true, 0.0, true, 1.0);
	gCV_SyncOrigin = CreateConVar("sm_bhoptools_sync_origin", "1", "After each command, copy a player's abs origin to the networked m_vecOrigin when a plugin moved only the abs origin (Haze-style landfix lifts): 0 off, 1 predicting clients, 2 everyone.", 0, true, 0.0, true, 2.0);

	GameData gamedata = new GameData("shavit.games");

	if (gamedata != null)
	{
		StartPrepSDKCall(SDKCall_Entity);

		if (PrepSDKCall_SetFromConf(gamedata, SDKConf_Signature, "PhysicsCheckForEntityUntouch"))
		{
			gH_Untouch = EndPrepSDKCall();
		}

		delete gamedata;
	}

	if (gH_Untouch == null)
	{
		LogError("PhysicsCheckForEntityUntouch not found in shavit.games; touches end once per frame");
	}

	GameData sdktools = new GameData("sdktools.games");
	int teleport = (sdktools != null) ? sdktools.GetOffset("Teleport") : -1;
	delete sdktools;

	if (teleport != -1)
	{
		gH_TeleportHook = new DynamicHook(teleport, HookType_Entity, ReturnType_Void, ThisPointer_CBaseEntity);
		gH_TeleportHook.AddParam(HookParamType_VectorPtr);
		gH_TeleportHook.AddParam(HookParamType_VectorPtr);
		gH_TeleportHook.AddParam(HookParamType_VectorPtr);
	}
	else
	{
		LogError("Teleport not found in sdktools.games; autosync isn't reported and teleports snap the view");
	}

	gI_PlayerState = FindSendPropInfo("CBasePlayer", "deadflag");

	if (gI_PlayerState <= 0)
	{
		SetFailState("Couldn't find CBasePlayer::pl.deadflag");
	}

	CreateTimer(1.0, Timer_CheckRngfix, _, TIMER_REPEAT);
	UpdateRngfix();
	UpdateMsf();

	gH_SyncClasses = new StringMap();

	for (int i = 0; i < sizeof(gS_SyncClasses); i++)
	{
		gH_SyncClasses.SetValue(gS_SyncClasses[i], gI_SyncClassKind[i]);
	}

	ResetSync();
	HookEvent("round_start", Event_RoundStart, EventHookMode_PostNoCopy);

	// Clients only say hello after a map load, so a reload has to ask.
	for (int i = 1; i <= MaxClients; i++)
	{
		if (IsClientInGame(i) && !IsFakeClient(i))
		{
			HookTeleport(i);
			ClientCommand(i, "bhoptools_ask");
		}
	}
}

public void OnAllPluginsLoaded()
{
	gB_SuperLandfix = LibraryExists("superlandfix");
	gCV_MaxCPSegmented = FindConVar("shavit_checkpoints_maxcp_seg");
	gCV_MaxCP = FindConVar("shavit_checkpoints_maxcp");
	FindNoclipCvars();
	FindRestartCvars();
	FindCookies();
	gB_Zones = LibraryExists("shavit-zones");
	FindZoneCvars();
	Shavit_GetChatStringsStruct(gS_ChatStrings);
	UpdateRngfix();
	UpdateMsf();
	UpdateSyncstyle();
}

// Map configs set rngfix's cvars; the change hooks catch those too.
public void OnConfigsExecuted()
{
	UpdateRngfix();
	UpdateMsf();
}

public void OnClientPutInServer(int client)
{
	gB_Predicting[client] = false;
	gI_Session[client] = 0;
	gI_Proto[client] = 0;
	gI_ClientFlags[client] = 0;
	Frames_OnClientPutInServer(client);
	gB_SavePending[client] = false;
	gI_Checkpoint[client] = 0;
	gI_OldFrameLeft[client] = 0;
	gI_LastImpulseCmd[client] = 0;
	gI_LastCmd[client] = 0;
	gB_InImpulseSave[client] = false;
	gI_SentAutobhop[client] = -1;
	gI_SentCpset[client] = -1;
	gB_NoclipCaps[client] = false;
	gI_SentNoclip[client] = -1;
	gI_SentLandfix[client] = -1;
	gI_SentKeys[client] = -1;
	gI_SentAutosync[client] = -1;
	gB_SyncOff[client] = false;
	gI_SyncZero[client] = 0;
	gB_SyncZeroNext[client] = false;
	gB_InCmd[client] = false;
	gB_OwnTeleport[client] = false;
	gB_StateDump[client] = false;
	gB_DumpOpen[client] = false;
	gB_StateRan[client] = false;
	gB_RestartCaps[client] = false;
	gS_SentRestart[client][0] = 0;
	gB_DeleteMenu[client] = false;
	gB_SetStartPending[client] = false;
	ResetZonePlayer(client);
	ForgetDestinations(client);

	if (!IsFakeClient(client))
	{
		HookTeleport(client);
	}
}

public void OnMapStart()
{
	for (int i = 0; i < sizeof(gS_SyncClasses); i++)
	{
		int entity = -1;

		while ((entity = FindEntityByClassname(entity, gS_SyncClasses[i])) != -1)
		{
			TrackEntity(entity, gI_SyncClassKind[i]);
		}
	}

	// The client simulates entities the world never has or already lost, like a point_template's
	// templates (removed at spawn). Those go out as removed.
	for (int i = 0; i < EntityLump.Length(); i++)
	{
		EntityLumpEntry entry = EntityLump.Get(i);
		char classname[32], hammerid[16];
		int kind;

		if (entry.GetNextKey("classname", classname, sizeof(classname)) != -1 && gH_SyncClasses.GetValue(classname, kind)
			&& entry.GetNextKey("hammerid", hammerid, sizeof(hammerid)) != -1)
		{
			TrackMissing(StringToInt(hammerid), kind);
		}

		delete entry;
	}

	RequestStateDumps();
	BuildMapdiff();
	gB_ZonesPublished = false;
}

public void OnLibraryRemoved(const char[] name)
{
	if (StrEqual(name, "superlandfix"))
	{
		gB_SuperLandfix = false;
	}

	if (StrEqual(name, "shavit-zones"))
	{
		gB_Zones = false;
	}
}

public void OnLibraryAdded(const char[] name)
{
	if (StrEqual(name, "superlandfix"))
	{
		gB_SuperLandfix = true;
	}

	if (StrEqual(name, "shavit-zones"))
	{
		gB_Zones = true;
	}

	if (StrEqual(name, "tpcomp"))
	{
		for (int i = 1; i <= MaxClients; i++)
		{
			if (IsClientInGame(i) && gI_ClientFlags[i] != 0)
			{
				UpdateTPComp(i);
			}
		}
	}
}

public Action Command_Hello(int client, int args)
{
	if (client == 0)
	{
		return Plugin_Handled;
	}

	int session = GetCmdArgInt(1);
	int proto = (args >= 2) ? GetCmdArgInt(2) : 1;
	int flags = (args >= 3) ? GetCmdArgInt(3) : 0;
	int flagsBefore = ClientFlagsFor(client);
	int sessionBefore = gI_Session[client];

	if (flags != gI_ClientFlags[client])
	{
		gI_ClientFlags[client] = flags;
		UpdateTPComp(client);

		gB_StateDump[client] = true;
		Debug(client, "client flags %d", flags);
	}

	if (gB_Predicting[client] && session == gI_Session[client])
	{
		NotifyClientFlags(client, flagsBefore, sessionBefore);
		return Plugin_Handled;
	}

	// A new session means the client dropped its saves, so drop ours too. Not on the
	// first hello: the client predicted any checkpoint saved before it.
	if (gI_Session[client] != 0)
	{
		gI_Checkpoint[client] = 0;
	}
	else if (gI_Checkpoint[client] > 0)
	{
		gB_CheckpointPredicted[client] = true;
	}

	gB_Predicting[client] = true;
	gI_Session[client] = session;
	gI_Proto[client] = proto;
	Debug(client, "hello, session %d, protocol %d", session, proto);

	if (proto >= 2)
	{
		gB_NoclipCaps[client] = NoclipAvailable();
		gB_RestartCaps[client] = RestartAvailable();
		int caps = CAPS_PREVNEXT | CAPS_RNGFIX | CAPS_AUTOBHOP | CAPS_MOVE | (LibraryExists("eventqueuefix") ? CAPS_EVENTQUEUEFIX : 0) | (gB_NoclipCaps[client] ? CAPS_NOCLIP : 0) | (gH_Untouch != null ? CAPS_UNTOUCH : 0) | CAPS_MSF | (MpbhopsLoaded() ? CAPS_MPBHOPS : 0) | CAPS_STATE | CAPS_CPSET | (gB_RestartCaps[client] ? CAPS_RESTART : 0) | CAPS_KEYS | (gH_TeleportHook != null ? CAPS_AUTOSYNC : 0) | (gB_Mapdiff ? CAPS_MAPDIFF : 0) | (gB_Zones ? CAPS_ZONES : 0) | (gH_TeleportHook != null ? CAPS_TELEPORT_VIEW : 0);
		ClientCommand(client, "bhoptools_caps %d %d %d %d %d %d", PROTO, session, gI_LastCmd[client], caps, client, GetUserMessageId("SayText2"));
		Frames_OnHello(client, session);
		Debug(client, "caps sent, server flags %d", caps);
		SendChatLine(client);
		SendRngfix(client);
		SendMsf(client);
		gI_SentAutobhop[client] = -1;
		gI_SentCpset[client] = -1;
		gI_SentNoclip[client] = -1;
		gS_SentRestart[client][0] = 0;
		gI_SentLandfix[client] = -1;
		gI_SentKeys[client] = -1;
		gI_SentAutosync[client] = -1;
		gI_SentTotal[client] = -1;
		gB_StateDump[client] = true;
		gB_StateRan[client] = false;
		MarkListDirty(client);
		SendLandfixSkip(client);
		ResetZonePlayer(client);
	}

	NotifyClientFlags(client, flagsBefore, sessionBefore);
	return Plugin_Handled;
}

// What other plugins see: protocol 2 clients' hello flags, 0 for everyone else.
int ClientFlagsFor(int client)
{
	return (gB_Predicting[client] && gI_Proto[client] >= 2) ? gI_ClientFlags[client] : 0;
}

// Messages other plugins send carry the session, so a new one counts as a change.
void NotifyClientFlags(int client, int flagsBefore, int sessionBefore)
{
	int flags = ClientFlagsFor(client);

	if (flags == flagsBefore && gI_Session[client] == sessionBefore)
	{
		return;
	}

	Call_StartForward(gH_OnClientFlags);
	Call_PushCell(client);
	Call_PushCell(flags);
	Call_Finish();
}

public any Native_GetClientFlags(Handle plugin, int numParams)
{
	int client = GetNativeCell(1);
	return (client >= 1 && client <= MaxClients && IsClientInGame(client)) ? ClientFlagsFor(client) : 0;
}

public any Native_GetSession(Handle plugin, int numParams)
{
	int client = GetNativeCell(1);
	return (client >= 1 && client <= MaxClients && ClientFlagsFor(client) != 0) ? gI_Session[client] : 0;
}

public void OnNotifyPluginUnloaded(Handle plugin)
{
	if (IsRngfix(plugin))
	{
		UpdateRngfix(plugin);
	}

	if (IsMsf(plugin))
	{
		UpdateMsf(plugin);
	}

	UpdateSyncstyle(plugin);
}

public Action OnPlayerRunCmd(int client, int &buttons, int &impulse, float vel[3], float angles[3], int &weapon, int &subtype, int &cmdnum, int &tickcount, int &seed, int mouse[2])
{
	gB_InCmd[client] = true;
	gI_RunCmd[client] = cmdnum;

	if (gB_SyncZeroNext[client])
	{
		gB_SyncZeroNext[client] = false;
		gI_SyncZero[client] = cmdnum;
	}

	GetClientAbsOrigin(client, gF_PreOrigin[client]);
	gB_PreDucking[client] = (GetEntityFlags(client) & FL_DUCKING) != 0;
	FollowCurrentCheckpoint(client);
	SendCheckpointCount(client);
	SendAutobhop(client);
	SendCpset(client);
	SendNoclip(client);
	SendRestart(client, gI_LastCmd[client], false);

	if (cmdnum == gI_LastCmd[client])
	{
		Debug(client, "cmd %d run again: more packets lost than the client backs up", cmdnum);
	}

	if (impulse >= IMPULSE_SAVE && impulse <= IMPULSE_SETSTART)
	{
		// When more packets are lost than the client backs up, the server reruns the last
		// command it got once per missing one, impulse included. The client never runs those.
		if (cmdnum == gI_LastImpulseCmd[client])
		{
			impulse = 0;
			ShiftOldFrame(client, angles);
			return Plugin_Changed;
		}

		gI_LastImpulseCmd[client] = cmdnum;
	}

	gI_LastCmd[client] = cmdnum;

	bool changed = (impulse != IMPULSE_TELE) && ShiftOldFrame(client, angles);

	if (impulse >= IMPULSE_NOCLIP && impulse <= IMPULSE_NOCLIP_OFF)
	{
		Noclip(client, impulse, cmdnum);
		impulse = 0;
		return Plugin_Changed;
	}

	if ((impulse == IMPULSE_RESTART || impulse == IMPULSE_SETSTART) && RestartAgreed(client))
	{
		if (impulse == IMPULSE_SETSTART)
		{
			// Command_SetStart saves where the move leaves the player, as the client does.
			gB_SetStartPending[client] = true;
		}
		else if (IsPlayerAlive(client))
		{
			gB_OwnTeleport[client] = true;
			Restart(client, cmdnum, angles);
			gB_OwnTeleport[client] = false;
		}
		else
		{
			// shavit-misc respawns the dead instead, which doesn't belong inside a command.
			FakeClientCommandEx(client, "sm_r");
			ClientCommand(client, "bhoptools_ack %d %d r 0", gI_Session[client], cmdnum);
		}

		impulse = 0;
		return Plugin_Changed;
	}

	// Through shavit's own commands so an open checkpoint menu is redrawn as usual.
	if (impulse == IMPULSE_PREV || impulse == IMPULSE_NEXT)
	{
		int from = Shavit_GetCurrentCheckpoint(client);
		FakeClientCommand(client, (impulse == IMPULSE_NEXT) ? "sm_nextcp" : "sm_prevcp");
		Debug(client, "cmd %d: %s, cp %d -> %d", cmdnum, (impulse == IMPULSE_NEXT) ? "next" : "prev", from, Shavit_GetCurrentCheckpoint(client));
		impulse = 0;
		return Plugin_Changed;
	}

	if (impulse == IMPULSE_SAVE)
	{
		// The client saves after the move, so save after the move too.
		gB_SavePending[client] = true;
		impulse = 0;
		return Plugin_Changed;
	}

	if (impulse != IMPULSE_TELE)
	{
		return changed ? Plugin_Changed : Plugin_Continue;
	}

	impulse = 0;
	gI_OldFrameLeft[client] = 0;

	float original[3];
	original = angles;

	int index = gI_Checkpoint[client];
	gB_OwnTeleport[client] = true;
	bool done = (index > 0) && Teleport(client, angles);
	gB_OwnTeleport[client] = false;

	if (index <= 0)
	{
		Debug(client, "tele ignored, no impulse checkpoint");
	}

	if (!done && gB_Predicting[client])
	{
		if (gI_Proto[client] < 2 && gCV_RefusedSnap.BoolValue)
		{
			// An old client already turned its view to the save; nothing else would turn it back.
			TeleportEntity(client, NULL_VECTOR, original, NULL_VECTOR);
		}
	}

	if (gI_Proto[client] >= 2)
	{
		ClientCommand(client, "bhoptools_ack %d %d t %d", gI_Session[client], cmdnum, done ? index : 0);
	}

	return Plugin_Changed;
}

public void OnPlayerRunCmdPost(int client, int buttons, int impulse, const float vel[3], const float angles[3], int weapon, int subtype, int cmdnum, int tickcount, int seed, const int mouse[2])
{
	if (gB_SavePending[client])
	{
		gB_SavePending[client] = false;
		ImpulseSave(client, cmdnum);
	}

	if (gB_SetStartPending[client])
	{
		gB_SetStartPending[client] = false;
		SetStartImpulse(client, cmdnum);
	}

	// Ends stale touches after every command, not once per frame like the engine.
	if (gH_Untouch != null && gB_Predicting[client] && (gI_ClientFlags[client] & CLIENT_UNTOUCH)
		&& (GetEntProp(client, Prop_Data, "m_iEFlags") & EFL_CHECK_UNTOUCH))
	{
		SDKCall(gH_Untouch, client);
	}

	SendMove(client, cmdnum);
	SendKeys(client, cmdnum);
	SendAutosync(client, cmdnum);
	SendZonePlayer(client, cmdnum);
	ResendDoorsOnUse(client, buttons);
	FixDuckTeleportShadow(client);

	int sync = gCV_SyncOrigin.IntValue;
	if (sync == 2 || (sync == 1 && gB_Predicting[client]))
	{
		SyncNetworkedOrigin(client);
	}

	gI_StateCmd[client] = cmdnum;
	gB_StateRan[client] = true;
	gB_InCmd[client] = false;
}
