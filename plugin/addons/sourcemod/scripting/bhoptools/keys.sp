// bhoptools_keys flags; must match the client library.
#define KEYS_BLOCK_W        (1 << 0)
#define KEYS_BLOCK_A        (1 << 1)
#define KEYS_BLOCK_S        (1 << 2)
#define KEYS_BLOCK_D        (1 << 3)
#define KEYS_AD_ONLY        (1 << 4)
#define KEYS_HSW            (1 << 5)
#define KEYS_SHSW           (1 << 6)
#define KEYS_GROUNDKEYS     (1 << 7)
#define KEYS_PLEFT          (1 << 8)
#define KEYS_PRIGHT         (1 << 9)
#define KEYS_TIMER          (1 << 10)
#define KEYS_NOZONES        (1 << 11)
#define KEYS_INSTART        (1 << 12)
#define KEYS_FREESTYLE      (1 << 13)
#define KEYS_ALLKEYS        (1 << 14)
#define KEYS_SIMPLERLADDERS (1 << 15)
#define KEYS_PAUSED         (1 << 16)

ConVar gCV_SimplerLadders;

int gI_SentKeys[MAXPLAYERS+1];
int gI_SentKeyCombo[MAXPLAYERS+1];

// Mirrors shavit-core's OnPlayerRunCmd, which zeroes vel and clears the buttons of blocked keys before the move.
int KeysFlags(int client, int &combo)
{
	if (gCV_PauseMovement == null)
	{
		gCV_PauseMovement = FindConVar("shavit_core_pause_movement");
	}

	if (gCV_SimplerLadders == null)
	{
		gCV_SimplerLadders = FindConVar("shavit_core_simplerladders");
	}

	timer_snapshot_t snapshot;
	Shavit_SaveSnapshot(client, snapshot);
	combo = snapshot.iKeyCombo;
	int style = snapshot.bsStyle;
	int flags = 0;

	if (Shavit_GetStyleSettingBool(style, "block_w")) flags |= KEYS_BLOCK_W;
	if (Shavit_GetStyleSettingBool(style, "block_a")) flags |= KEYS_BLOCK_A;
	if (Shavit_GetStyleSettingBool(style, "block_s")) flags |= KEYS_BLOCK_S;
	if (Shavit_GetStyleSettingBool(style, "block_d")) flags |= KEYS_BLOCK_D;
	if (Shavit_GetStyleSettingBool(style, "a_or_d_only")) flags |= KEYS_AD_ONLY;
	if (Shavit_GetStyleSettingBool(style, "force_groundkeys")) flags |= KEYS_GROUNDKEYS;
	if (Shavit_GetStyleSettingInt(style, "block_pleft") > 0) flags |= KEYS_PLEFT;
	if (Shavit_GetStyleSettingInt(style, "block_pright") > 0) flags |= KEYS_PRIGHT;

	int hsw = Shavit_GetStyleSettingInt(style, "force_hsw");
	if (hsw > 0) flags |= KEYS_HSW;
	if (hsw == 2) flags |= KEYS_SHSW;

	if (snapshot.bTimerEnabled && !snapshot.bClientPaused) flags |= KEYS_TIMER;
	if (snapshot.bCanUseAllKeys) flags |= KEYS_ALLKEYS;
	if (gCV_SimplerLadders != null && gCV_SimplerLadders.BoolValue) flags |= KEYS_SIMPLERLADDERS;

	if (snapshot.bClientPaused && gCV_PauseMovement != null && (gCV_PauseMovement.IntValue == 0 || gCV_PauseMovement.IntValue == 3))
	{
		flags |= KEYS_PAUSED;
	}

	if (!gB_Zones)
	{
		flags |= KEYS_NOZONES;
	}
	else
	{
		if (Shavit_InsideZone(client, Zone_Start, snapshot.iTimerTrack)) flags |= KEYS_INSTART;
		if (Shavit_InsideZone(client, Zone_Freestyle, -1)) flags |= KEYS_FREESTYLE;
	}

	return flags;
}

// After the command: its zone touches, timer starts and shavit's combo and ladder updates have
// happened by now, so this is what the next command starts from.
void SendKeys(int client, int cmdnum)
{
	if (!gB_Predicting[client] || gI_Proto[client] < 2 || !(gI_ClientFlags[client] & CLIENT_KEYS))
	{
		return;
	}

	int combo;
	int flags = KeysFlags(client, combo);

	if (flags != gI_SentKeys[client] || combo != gI_SentKeyCombo[client])
	{
		gI_SentKeys[client] = flags;
		gI_SentKeyCombo[client] = combo;
		ClientCommand(client, "bhoptools_keys %d %d %d %d", gI_Session[client], flags, combo, cmdnum);
		Debug(client, "keys %x, combo %d", flags, combo);
	}
}
