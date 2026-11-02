int gI_SentAutobhop[MAXPLAYERS+1];

// shavit-core's OnPlayerRunCmd clears IN_JUMP from m_nOldButtons, before the move and so before
// rngfix reads it, when this holds and the player walks, isn't in water (level 2) and holds jump.
// gB_Auto there is sm_auto, kept in its cookie.
bool AutobhopOn(int client)
{
	if (!Shavit_GetStyleSettingBool(Shavit_GetBhopStyle(client), "autobhop")
		&& !(gB_Zones && Shavit_InsideZone(client, Zone_Autobhop, Shavit_GetClientTrack(client))))
	{
		return false;
	}

	return AutoToggle(client);
}

bool AutoToggle(int client)
{
	if (GetFeatureStatus(FeatureType_Native, "Shavit_GetAutobhop") == FeatureStatus_Available)
	{
		return Shavit_GetAutobhop(client);
	}

	// unpatched shavit
	return CookieInt(client, gH_AutoBhopCookie, 1) != 0;
}

void SendAutobhop(int client)
{
	if (!gB_Predicting[client] || gI_Proto[client] < 2 || !(gI_ClientFlags[client] & CLIENT_AUTOBHOP))
	{
		return;
	}

	int on = AutobhopOn(client) ? 1 : 0;

	if (on != gI_SentAutobhop[client])
	{
		gI_SentAutobhop[client] = on;
		ClientCommand(client, "bhoptools_autobhop %d %d", gI_Session[client], on);
		Debug(client, "autobhop %d", on);
	}
}
