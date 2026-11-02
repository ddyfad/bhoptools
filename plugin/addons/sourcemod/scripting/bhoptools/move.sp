// bhoptools_move landfix methods; must match the client library. SuperLandfix_GetMethod uses the same numbers.
#define LANDFIX_OFF    0
#define LANDFIX_HAZE   1
#define LANDFIX_OLIVIA 2 // NoTimeLoss

// bhoptools_lfskip entities per command, inside ClientCommand's 254 characters.
#define LFSKIP_CHUNK 32

native int SuperLandfix_GetMethod(int client);

ConVar gCV_StaticPrestrafe;
bool gB_SuperLandfix;

// gI_SentLandfix -1 sends bhoptools_move again.
float gF_SentRunspeed[MAXPLAYERS+1];
int gI_SentLandfix[MAXPLAYERS+1];
int gI_SentEasybhop[MAXPLAYERS+1];

// shavit-misc post-hooks CCSPlayer::GetPlayerMaxSpeed with shavit_misc_staticprestrafe and returns the
// style's runspeed for every live player (ProcessMovement's m_flMaxSpeed). 0 when nothing overrides it;
// the client checks alive itself.
float Runspeed(int client)
{
	if (gCV_StaticPrestrafe == null)
	{
		gCV_StaticPrestrafe = FindConVar("shavit_misc_staticprestrafe");
	}

	if (gCV_StaticPrestrafe == null || !gCV_StaticPrestrafe.BoolValue || !CommandExists("sm_nc"))
	{
		return 0.0;
	}

	return Shavit_GetStyleSettingFloat(Shavit_GetBhopStyle(client), "runspeed");
}

// Read after the command, once SuperLandfix has made its changes for it (its jump toggles flip on landing).
int LandfixMethod(int client)
{
	if (gB_SuperLandfix && GetFeatureStatus(FeatureType_Native, "SuperLandfix_GetMethod") == FeatureStatus_Available)
	{
		return SuperLandfix_GetMethod(client);
	}

	return LANDFIX_OFF;
}

// shavit-core zeroes m_flStamina on every jump for an easybhop style or inside an easybhop zone, and on
// landing for the style: 1 the style, 2 the zone, 0 neither.
int Easybhop(int client)
{
	if (Shavit_GetStyleSettingBool(Shavit_GetBhopStyle(client), "easybhop"))
	{
		return 1;
	}

	return gB_Zones && Shavit_InsideZone(client, Zone_Easybhop, Shavit_GetClientTrack(client)) ? 2 : 0;
}

// cmd is the command these applied to, so the client re-predicts from there instead of a round trip
// later. Clients older than 0.10 ignore the last two arguments.
void SendMove(int client, int cmdnum)
{
	if (!gB_Predicting[client] || gI_Proto[client] < 2 || !(gI_ClientFlags[client] & CLIENT_MOVE))
	{
		return;
	}

	float runspeed = Runspeed(client);
	int landfix = LandfixMethod(client);
	int easybhop = Easybhop(client);

	if (landfix != gI_SentLandfix[client] || runspeed != gF_SentRunspeed[client] || easybhop != gI_SentEasybhop[client])
	{
		gI_SentLandfix[client] = landfix;
		gF_SentRunspeed[client] = runspeed;
		gI_SentEasybhop[client] = easybhop;
		ClientCommand(client, "bhoptools_move %d %x %d %d %d", gI_Session[client], runspeed, landfix, easybhop, cmdnum);
		Debug(client, "runspeed %.2f, landfix %d, easybhop %d", runspeed, landfix, easybhop);
	}
}

// Haze's landfix leaves ground with m_currentSound alone (func_movelinear, func_water_analog).
bool HazeSkips(int entity)
{
	return IsValidEntity(entity) && HasEntProp(entity, Prop_Data, "m_currentSound");
}

void SendLandfixSkip(int client)
{
	if (!(gI_ClientFlags[client] & CLIENT_MOVE))
	{
		return;
	}

	int max = GetMaxEntities();
	int total = 0;

	for (int entity = MaxClients + 1; entity < max; entity++)
	{
		if (HazeSkips(entity))
		{
			total++;
		}
	}

	char msg[254];
	int offset = 0;
	int entity = MaxClients + 1;

	do
	{
		int len = FormatEx(msg, sizeof(msg), "bhoptools_lfskip %d %d %d", gI_Session[client], total, offset);

		for (int n = 0; n < LFSKIP_CHUNK && offset < total && entity < max; entity++)
		{
			if (HazeSkips(entity))
			{
				len += FormatEx(msg[len], sizeof(msg) - len, " %d", entity);
				offset++;
				n++;
			}
		}

		ClientCommand(client, "%s", msg);
	}
	while (offset < total && entity < max);

	Debug(client, "landfix skips %d entities", total);
}
