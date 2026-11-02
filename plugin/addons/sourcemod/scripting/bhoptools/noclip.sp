#define NOCLIP_ENTER (1 << 0)
#define NOCLIP_LEAVE (1 << 1)
#define NOCLIP_PLUS  (1 << 2)

ConVar gCV_NoclipMe;
bool gB_NoclipCaps[MAXPLAYERS+1];
int gI_SentNoclip[MAXPLAYERS+1];

void FindNoclipCvars()
{
	if (gCV_NoclipMe == null)
	{
		gCV_NoclipMe = FindConVar("shavit_misc_noclipme");
	}

	if (gCV_StopTimerWarning == null)
	{
		gCV_StopTimerWarning = FindConVar("shavit_misc_stoptimerwarning");
	}

	if (gCV_PauseMovement == null)
	{
		gCV_PauseMovement = FindConVar("shavit_core_pause_movement");
	}
}

// shavit-misc registers no library; its noclip command and cvar stand in for it.
bool NoclipAvailable()
{
	FindNoclipCvars();
	return gCV_NoclipMe != null && CommandExists("sm_nc");
}

int NoclipAllow(int client)
{
	if (GetFeatureStatus(FeatureType_Native, "Shavit_GetNoclipAccess") == FeatureStatus_Available)
	{
		int access = Shavit_GetNoclipAccess(client);
		return ((access & NoclipAccess_Enter) ? NOCLIP_ENTER : 0) | ((access & NoclipAccess_Leave) ? NOCLIP_LEAVE : 0)
			| ((access & NoclipAccess_Plus) ? NOCLIP_PLUS : 0);
	}

	// unpatched shavit
	int noclipme = gCV_NoclipMe.IntValue;
	bool warning = StopTimerWarning(client);
	int allow = 0;

	if (noclipme == 1 || (noclipme == 2 && CheckCommandAccess(client, "admin_noclipme", ADMFLAG_CHEATS)))
	{
		allow |= NOCLIP_LEAVE;

		if (!warning || (gCV_PauseMovement != null && gCV_PauseMovement.BoolValue && Shavit_IsPaused(client)))
		{
			allow |= NOCLIP_ENTER;
		}
	}

	if ((noclipme == 1 || (noclipme == 2 && CheckCommandAccess(client, "noclipme", ADMFLAG_CHEATS))) && !warning)
	{
		allow |= NOCLIP_PLUS;
	}

	return allow;
}

void SendNoclip(int client)
{
	if (!gB_NoclipCaps[client] || !gB_Predicting[client] || gI_Proto[client] < 2 || !(gI_ClientFlags[client] & CLIENT_NOCLIP) || gCV_NoclipMe == null)
	{
		return;
	}

	int allow = NoclipAllow(client);

	if (allow != gI_SentNoclip[client])
	{
		gI_SentNoclip[client] = allow;
		ClientCommand(client, "bhoptools_noclip %d %d", gI_Session[client], allow);
		Debug(client, "noclip allowed %d", allow);
	}
}

// Through shavit-misc's own commands, before the move like the client, so its checks and the
// timer stop apply to this command. Whether the movetype changed is the answer.
void Noclip(int client, int impulse, int cmdnum)
{
	MoveType before = GetEntityMoveType(client);
	FakeClientCommand(client, (impulse == IMPULSE_NOCLIP) ? "sm_nc" : (impulse == IMPULSE_NOCLIP_ON) ? "+noclip" : "-noclip");
	MoveType after = GetEntityMoveType(client);

	if (gB_Predicting[client] && gI_Proto[client] >= 2 && (gI_ClientFlags[client] & CLIENT_NOCLIP))
	{
		ClientCommand(client, "bhoptools_ack %d %d o %d", gI_Session[client], cmdnum, (after != before) ? 1 : 0);
	}

	Debug(client, "noclip impulse %d, movetype %d -> %d", impulse, before, after);
}
