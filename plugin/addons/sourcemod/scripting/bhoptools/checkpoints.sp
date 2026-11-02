#define MENUKEY_SAVE (1 << 0)
#define MENUKEY_TELE (1 << 1)

#define CP_ANGLES   (1 << 0)
#define CP_VELOCITY (1 << 1)

// FNV-1a, 32 bits: the offset basis 0x811C9DC5 as a signed cell, and the prime.
#define FNV_OFFSET -2128831035
#define FNV_PRIME  16777619

#define APPLY_KZ (1 << 0)

// Stands in for the checkpoint number in the save line; must match the client library.
#define CHAT_NUMBER 1234567890

ConVar gCV_RefusedSnap;
ConVar gCV_Quiet;
ConVar gCV_MaxCPSegmented;
ConVar gCV_MaxCP;
chatstrings_t gS_ChatStrings;

bool gB_SavePending[MAXPLAYERS+1];
int gI_Checkpoint[MAXPLAYERS+1];
bool gB_CheckpointPredicted[MAXPLAYERS+1];
int gI_SentTotal[MAXPLAYERS+1];
int gI_SentCpset[MAXPLAYERS+1];
int gI_SentMax[MAXPLAYERS+1];

bool gB_ListDirty[MAXPLAYERS+1];
int gI_ListNext[MAXPLAYERS+1];

// Shavit_SaveCheckpoint returns the current index even when it refused the
// save, so watch for Shavit_OnSave firing inside the call instead.
bool gB_InImpulseSave[MAXPLAYERS+1];
int gI_ImpulseSaveIndex[MAXPLAYERS+1];

bool gB_Teleported[MAXPLAYERS+1];

float gF_TurnYaw[MAXPLAYERS+1];
float gF_EffYaw[MAXPLAYERS+1];
int gI_OldFrameLeft[MAXPLAYERS+1];

public void Shavit_OnChatConfigLoaded()
{
	Shavit_GetChatStringsStruct(gS_ChatStrings);

	for (int i = 1; i <= MaxClients; i++)
	{
		if (IsClientInGame(i) && gB_Predicting[i] && gI_Proto[i] >= 2)
		{
			SendChatLine(i);
		}
	}
}

void SendChatLine(int client)
{
	char input[300];
	char line[256];
	FormatEx(input, sizeof(input), "%T", "MiscCheckpointsSaved", client, CHAT_NUMBER, gS_ChatStrings.sVariable, gS_ChatStrings.sText);
	FormatEx(line, 253, "%s%s%s%s", gS_ChatStrings.sPrefix, (gS_ChatStrings.sPrefix[0] != 0) ? " " : "", gS_ChatStrings.sText, input);

	char command[32];
	FormatEx(command, sizeof(command), "bhoptools_chat %d", gI_Session[client]);
	SendLine(client, command, line);
}

// Chunked because ClientCommand is limited to 254 characters.
void SendLine(int client, const char[] command, const char[] line)
{
	int len = strlen(line);
	int start = 0;
	int n = 0;
	char chunk[160];

	for (int i = 0; i <= len; i++)
	{
		if (i == len || n > sizeof(chunk) - 4)
		{
			chunk[n] = 0;
			ClientCommand(client, "%s %d %d \"%s\"", command, len, start, chunk);

			if (i == len)
			{
				break;
			}

			start = i;
			n = 0;
		}

		int c = line[i] & 0xFF;

		if (c < 0x20 || c > 0x7E || c == '"' || c == ';' || c == '%')
		{
			n += FormatEx(chunk[n], sizeof(chunk) - n, "%%%02x", c);
		}
		else
		{
			chunk[n++] = c;
		}
	}
}

void SendCheckpointCount(int client)
{
	if (!gB_Predicting[client] || gI_Proto[client] < 2)
	{
		return;
	}

	int total = Shavit_GetTotalCheckpoints(client);
	int max = 0;

	if (gCV_MaxCPSegmented != null && Shavit_GetStyleSettingBool(Shavit_GetBhopStyle(client), "segments"))
	{
		max = gCV_MaxCPSegmented.IntValue;
	}

	if (total != gI_SentTotal[client] || max != gI_SentMax[client])
	{
		gI_SentTotal[client] = total;
		gI_SentMax[client] = max;
		ClientCommand(client, "bhoptools_ack %d %d n %d %d", gI_Session[client], gI_LastCmd[client], total, max);
		Debug(client, "checkpoint count %d, cap %d", total, max);
	}

	if (!gB_ListDirty[client])
	{
		return;
	}

	cp_cache_t cp;

	for (int sent = 0; sent < 4 && gI_ListNext[client] <= total; sent++, gI_ListNext[client]++)
	{
		if (Shavit_GetCheckpoint(client, gI_ListNext[client], cp))
		{
			AckState(client, "l", gI_LastCmd[client], gI_ListNext[client], cp);
		}
	}

	gB_ListDirty[client] = gI_ListNext[client] <= total;

	if (!gB_ListDirty[client])
	{
		Debug(client, "checkpoint list sent (%d)", total);
	}
}

void MarkListDirty(int client)
{
	gB_ListDirty[client] = true;
	gI_ListNext[client] = 1;
}

// gI_Checkpoint is a shavit index, so follow it when shavit renumbers. Saving at the cap
// deletes checkpoint 1 without calling Shavit_OnDelete.
public Action Shavit_OnSave(int client, int index, bool overflow, bool duplicate)
{
	if (gB_InImpulseSave[client])
	{
		gI_ImpulseSaveIndex[client] = index;
	}

	if (overflow)
	{
		MarkListDirty(client);
		Debug(client, "save at the cap dropped checkpoint 1");
	}

	if (overflow && gI_Checkpoint[client] > 0)
	{
		gI_Checkpoint[client]--;

		if (gI_Checkpoint[client] == 0 && !gB_InImpulseSave[client])
		{
			AckCleared(client);
		}
	}

	return Plugin_Continue;
}

public Action Shavit_OnDelete(int client, int index, bool cleared)
{
	MarkListDirty(client);
	Debug(client, "checkpoint %d deleted%s, tracking %d", index, cleared ? " (all)" : "", gI_Checkpoint[client]);

	if (cleared || index == gI_Checkpoint[client])
	{
		if (gI_Checkpoint[client] > 0)
		{
			AckCleared(client);
		}

		gI_Checkpoint[client] = 0;
	}
	else if (index < gI_Checkpoint[client])
	{
		gI_Checkpoint[client]--;
	}

	return Plugin_Continue;
}

public Action Shavit_OnCheckpointMenuMade(int client, bool segmented, Menu menu)
{
	if (!gB_Predicting[client] || gI_Proto[client] < 2 || !(gI_ClientFlags[client] & CLIENT_CPMENU))
	{
		return Plugin_Continue;
	}

	char info[16];

	if (!menu.GetItem(2, info, sizeof(info)) || !StrEqual(info, "prev") || !menu.GetItem(3, info, sizeof(info)) || !StrEqual(info, "next"))
	{
		return Plugin_Continue;
	}

	for (int i = 4; i < menu.ItemCount; i++)
	{
		if (menu.GetItem(i, info, sizeof(info)) && StrEqual(info, "useselfs"))
		{
			return Plugin_Continue;
		}
	}

	int total = Shavit_GetTotalCheckpoints(client);
	int len, hash;

	if (MenuHash(menu, len, hash))
	{
		char items[16];
		MenuItems(menu, items, sizeof(items));
		ClientCommand(client, "bhoptools_menu %d %d %d %d %d %d %d %s", gI_Session[client], Shavit_GetCurrentCheckpoint(client), total, gI_LastCmd[client], len, hash, MenuKeys(client, menu, segmented, total), items);

		// The client plays the key sounds itself, without the round trip.
		if (gI_ClientFlags[client] & CLIENT_MENU_SOUND)
		{
			menu.OptionFlags |= MENUFLAG_NO_SOUND;
		}
	}
	else
	{
		ClientCommand(client, "bhoptools_menu %d %d %d %d", gI_Session[client], Shavit_GetCurrentCheckpoint(client), total, gI_LastCmd[client]);
	}

	return Plugin_Continue;
}

int HashString(const char[] s, int hash, int &len)
{
	for (int i = 0; s[i] != 0; i++)
	{
		hash = (hash ^ (s[i] & 0xFF)) * FNV_PRIME;
		len++;
	}

	return hash;
}

bool MenuHash(Menu menu, int &len, int &hash)
{
	char text[512];
	char info[2];
	char num[8];
	int style;

	len = 0;
	hash = FNV_OFFSET;
	menu.GetTitle(text, sizeof(text));

	if (text[0] != 0)
	{
		hash = HashString(text, hash, len);
		hash = HashString("\n", hash, len);
	}

	for (int i = 0; i < 4; i++)
	{
		if (!menu.GetItem(i, info, sizeof(info), style, text, sizeof(text)) || (style & (ITEMDRAW_RAWLINE|ITEMDRAW_NOTEXT|ITEMDRAW_SPACER)))
		{
			return false;
		}

		if (!(style & ITEMDRAW_DISABLED))
		{
			hash = HashString("->", hash, len);
		}

		FormatEx(num, sizeof(num), "%d. ", i + 1);
		hash = HashString(num, hash, len);
		hash = HashString(text, hash, len);
		hash = HashString("\n", hash, len);
	}

	// SourceMod's radio display is cut at 511 bytes.
	return len <= 511;
}

// Keys 1 and 2 only where the impulse does what the menu item would: at the cap the menu's save drops
// the oldest checkpoint but Shavit_SaveCheckpoint refuses outside segmented styles, and the menu's
// teleport follows use angles/velocity, which a predicted teleport follows only for CLIENT_CPSET clients.
int MenuKeys(int client, Menu menu, bool segmented, int total)
{
	char info[16];
	int style;
	int keys = 0;

	if (menu.GetItem(0, info, sizeof(info), style) && StrEqual(info, "save") && !(style & ITEMDRAW_DISABLED)
		&& (segmented || (gCV_MaxCP != null && total < gCV_MaxCP.IntValue)))
	{
		keys |= MENUKEY_SAVE;
	}

	if (menu.GetItem(1, info, sizeof(info)) && StrEqual(info, "tele")
		&& ((gI_ClientFlags[client] & CLIENT_CPSET) || TeleportRestore(client) == (CP_ANGLES|CP_VELOCITY)))
	{
		keys |= MENUKEY_TELE;
	}

	return keys;
}

void MenuItems(Menu menu, char[] items, int size)
{
	char info[16];
	int n = 0;

	for (int i = 0; i < menu.ItemCount && n < size - 1; i++)
	{
		menu.GetItem(i, info, sizeof(info));
		char c = 'x';

		if (StrEqual(info, "save")) c = 's';
		else if (StrEqual(info, "tele")) c = 't';
		else if (StrEqual(info, "prev")) c = 'p';
		else if (StrEqual(info, "next")) c = 'n';
		else if (StrEqual(info, "del")) c = 'd';
		else if (StrEqual(info, "reset")) c = 'r';
		else if (StrEqual(info, "useothers")) c = 'o';
		else if (StrEqual(info, "tsminus")) c = '-';
		else if (StrEqual(info, "tsplus")) c = '+';
		else if (StrEqual(info, "pause")) c = 'z';
		else if (StringToInt(info) == CP_ANGLES) c = 'a';
		else if (StringToInt(info) == CP_VELOCITY) c = 'v';

		items[n++] = c;
	}

	items[n] = 0;
}

int TeleportRestore(int client)
{
	if (Shavit_GetStyleSettingBool(Shavit_GetBhopStyle(client), "segments"))
	{
		return CP_ANGLES|CP_VELOCITY;
	}

	if (GetFeatureStatus(FeatureType_Native, "Shavit_GetCheckpointOptions") == FeatureStatus_Available)
	{
		return Shavit_GetCheckpointOptions(client) & (CP_ANGLES|CP_VELOCITY);
	}

	// unpatched shavit
	return CookieInt(client, gH_CheckpointsCookie, CP_ANGLES|CP_VELOCITY) & (CP_ANGLES|CP_VELOCITY);
}

void SendCpset(int client)
{
	if (!gB_Predicting[client] || gI_Proto[client] < 2 || !(gI_ClientFlags[client] & CLIENT_CPSET))
	{
		return;
	}

	int restore = TeleportRestore(client);

	if (restore != gI_SentCpset[client])
	{
		gI_SentCpset[client] = restore;
		ClientCommand(client, "bhoptools_cpset %d %d", gI_Session[client], restore);
		Debug(client, "teleports restore %d", restore);
	}
}

public Action Shavit_OnTeleport(int client, int index, int target)
{
	gB_Teleported[client] = true;
	return Plugin_Continue;
}

void FollowCurrentCheckpoint(int client)
{
	// Old clients only know their own impulse saves.
	if (gB_Predicting[client] && gI_Proto[client] < 2)
	{
		return;
	}

	int index = Shavit_GetCurrentCheckpoint(client);
	cp_cache_t cp;

	if (index <= 0 || index == gI_Checkpoint[client] || !Shavit_GetCheckpoint(client, index, cp))
	{
		return;
	}

	gI_Checkpoint[client] = index;
	gB_CheckpointPredicted[client] = gB_Predicting[client];

	if (gB_Predicting[client])
	{
		AckSave(client, gI_LastCmd[client], index, cp);
	}

	Debug(client, "current checkpoint is now %d", index);
}

void ImpulseSave(int client, int cmdnum)
{
	gB_InImpulseSave[client] = true;
	gI_ImpulseSaveIndex[client] = 0;
	Shavit_SaveCheckpoint(client);
	gB_InImpulseSave[client] = false;

	int index = gI_ImpulseSaveIndex[client];

	// A refused save leaves the previous impulse checkpoint in place; the client drops its copy.
	if (index > 0)
	{
		gI_Checkpoint[client] = index;
		gB_CheckpointPredicted[client] = gB_Predicting[client];
	}

	cp_cache_t cp;

	if (gI_Proto[client] >= 2)
	{
		if (index > 0 && Shavit_GetCheckpoint(client, index, cp))
		{
			AckSave(client, cmdnum, index, cp);
		}
		else
		{
			ClientCommand(client, "bhoptools_ack %d %d s 0", gI_Session[client], cmdnum);
		}
	}

	Debug(client, "saved cp %d (%s, predicted %d)", index, index > 0 ? "ok" : "refused", gB_CheckpointPredicted[client]);
}

void AckSave(int client, int cmdnum, int index, cp_cache_t cp)
{
	AckState(client, "s", cmdnum, index, cp);
}

void AckState(int client, const char[] kind, int cmdnum, int index, cp_cache_t cp)
{
	int apply = Shavit_GetStyleSettingBool(cp.aSnapshot.bsStyle, "kzcheckpoints") ? APPLY_KZ : 0;

	ClientCommand(client, "bhoptools_ack %d %d %s %d %d %x %x %x %x %x %x %x %x %x %x %x %x %d %d %d %d",
		gI_Session[client], cmdnum, kind, index, apply,
		cp.fPosition[0], cp.fPosition[1], cp.fPosition[2],
		cp.fVelocity[0], cp.fVelocity[1], cp.fVelocity[2],
		cp.fAngles[0], cp.fAngles[1], cp.fAngles[2],
		cp.fGravity, cp.fStamina, cp.fDucktime,
		cp.iFlags, view_as<int>(cp.iMoveType), cp.bDucked ? 1 : 0, cp.bDucking ? 1 : 0);
}

void AckCleared(int client)
{
	if (gI_Proto[client] >= 2 && IsClientInGame(client))
	{
		ClientCommand(client, "bhoptools_ack %d %d c 0", gI_Session[client], gI_LastCmd[client]);
	}
}

bool Teleport(int client, float angles[3])
{
	int index = gI_Checkpoint[client];

	cp_cache_t cp;

	if (!Shavit_GetCheckpoint(client, index, cp))
	{
		gI_Checkpoint[client] = 0;
		return false;
	}

	bool predicted = gB_CheckpointPredicted[client];
	bool tpcomp = predicted && GetFeatureStatus(FeatureType_Native, "TPComp_Suspend") == FeatureStatus_Available;

	if (tpcomp)
	{
		TPComp_Suspend(client, true);
	}

	// tpcomp hooks every teleport, so keep it suspended through our own
	// TeleportEntity and fixangle clear below; that snap never reaches the client.
	float velocity[3];
	GetEntPropVector(client, Prop_Data, "m_vecAbsVelocity", velocity);
	gB_Teleported[client] = false;
	Shavit_TeleportToCheckpoint(client, index, gCV_Quiet.BoolValue);
	bool done = FinishTeleport(client, index, cp, predicted, angles, velocity);

	if (tpcomp)
	{
		TPComp_Suspend(client, false);
	}

	return done;
}

bool FinishTeleport(int client, int index, cp_cache_t cp, bool predicted, float angles[3], float velocity[3])
{
	float origin[3];
	GetClientAbsOrigin(client, origin);

	// shavit refused the teleport, or something moved the player off the checkpoint in
	// Shavit_OnTeleport. The client gets corrected back.
	if (!gB_Teleported[client] || GetVectorDistance(origin, cp.fPosition) > 0.1)
	{
		Debug(client, "tele to cp %d refused (%s), at %.1f %.1f %.1f", index, gB_Teleported[client] ? "moved off it after" : "shavit refused", origin[0], origin[1], origin[2]);
		return false;
	}

	if (!predicted)
	{
		Debug(client, "tele to cp %d, not predicted by the client", index);
		return true;
	}

	// The client has already turned its own view, so no angle snap goes out. kzcheckpoints styles teleport with
	// zero velocity in shavit-checkpoints; keep that. A velocity is always passed, since the client zeroes the
	// base velocity either way.
	int restore = CP_ANGLES|CP_VELOCITY;

	if ((gI_ClientFlags[client] & CLIENT_CPSET) && gI_SentCpset[client] >= 0)
	{
		restore = gI_SentCpset[client];
	}

	if (Shavit_GetStyleSettingBool(Shavit_GetBhopStyle(client), "kzcheckpoints"))
	{
		velocity[0] = 0.0;
		velocity[1] = 0.0;
		velocity[2] = 0.0;
	}
	else if (restore & CP_VELOCITY)
	{
		velocity = cp.fVelocity;
	}

	bool turn = (restore & CP_ANGLES) != 0;
	TeleportEntity(client, NULL_VECTOR, turn ? cp.fAngles : angles, velocity);

	// shavit records the checkpoint's angles for a round trip, covering a view snap this
	// client doesn't wait for.
	if (turn && LibraryExists("shavit-replay-recorder"))
	{
		Shavit_HijackAngles(client, cp.fAngles[0], cp.fAngles[1], 0);
	}

	// TeleportEntity just set FIXANGLE_ABSOLUTE; anything else means the offset is wrong for this build.
	if (GetEntData(client, gI_PlayerState + FIXANGLE_OFFSET) == 1)
	{
		SetEntData(client, gI_PlayerState + FIXANGLE_OFFSET, 0);
	}
	else
	{
		LogError("pl.fixangle not at deadflag+%d, leaving it alone", FIXANGLE_OFFSET);
	}

	if (!turn)
	{
		Debug(client, "tele to cp %d at %.1f %.1f %.1f, restoring %d", index, origin[0], origin[1], origin[2], restore);
		return true;
	}

	gF_TurnYaw[client] = NormalizeAngle(cp.fAngles[1] - angles[1]);
	gF_EffYaw[client] = cp.fAngles[1];
	gI_OldFrameLeft[client] = OLD_FRAME_WINDOW;

	// This command's movement runs from the checkpoint on the client too.
	angles[0] = cp.fAngles[0];
	angles[1] = cp.fAngles[1];
	angles[2] = cp.fAngles[2];

	Debug(client, "tele to cp %d at %.1f %.1f %.1f (predicted %d)", index, origin[0], origin[1], origin[2], predicted);
	return true;
}

// Same rule as the client: a command made before the turn lands closer to the previous one once the turn is added.
bool ShiftOldFrame(int client, float angles[3])
{
	if (gI_OldFrameLeft[client] <= 0)
	{
		return false;
	}

	float d = gF_TurnYaw[client];
	float eff = gF_EffYaw[client];

	if (FloatAbs(NormalizeAngle(angles[1] + d - eff)) >= FloatAbs(NormalizeAngle(angles[1] - eff)))
	{
		gI_OldFrameLeft[client] = 0;
		return false;
	}

	angles[1] = NormalizeAngle(angles[1] + d);
	gF_EffYaw[client] = angles[1];
	gI_OldFrameLeft[client]--;
	Debug(client, "made before the turn, shifted by %.1f", d);

	return true;
}
