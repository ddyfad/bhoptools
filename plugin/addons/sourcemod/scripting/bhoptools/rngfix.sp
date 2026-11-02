// rngfix's cvars aren't replicated, and the client only predicts what the server does.
// rngfix registers no library and its cvars outlive it, so look for the running plugin.
char gS_RngfixCvars[][] = { "rngfix_downhill", "rngfix_uphill", "rngfix_edge", "rngfix_triggerjump", "rngfix_telehop", "rngfix_stairs", "rngfix_useoldslopefixlogic" };
ConVar gCV_Rngfix[sizeof(gS_RngfixCvars)];
char gS_Rngfix[64];
bool gB_RngfixPending;

public Action Timer_CheckRngfix(Handle timer)
{
	UpdateRngfix();
	UpdateMsf();
	UpdateSyncstyle();
	UpdateZones();
	return Plugin_Continue;
}

public void OnRngfixCvarChanged(ConVar convar, const char[] oldValue, const char[] newValue)
{
	// A config sets them one at a time; send the result once.
	if (!gB_RngfixPending)
	{
		gB_RngfixPending = true;
		RequestFrame(Frame_UpdateRngfix);
	}
}

public void Frame_UpdateRngfix()
{
	gB_RngfixPending = false;
	UpdateRngfix();
}

bool IsRngfix(Handle plugin)
{
	char name[PLATFORM_MAX_PATH];

	if (GetPluginInfo(plugin, PlInfo_Name, name, sizeof(name)) && StrEqual(name, "RNGFix"))
	{
		return true;
	}

	GetPluginFilename(plugin, name, sizeof(name));
	int slash = FindCharInString(name, '/', true);
	return StrEqual(name[slash + 1], "rngfix.smx", false);
}

// `unloading` still counts as running while OnNotifyPluginUnloaded tells us about it.
bool RngfixRunning(Handle unloading)
{
	Handle iter = GetPluginIterator();
	bool running = false;

	while (!running && MorePlugins(iter))
	{
		Handle plugin = ReadPlugin(iter);
		running = plugin != unloading && GetPluginStatus(plugin) == Plugin_Running && IsRngfix(plugin);
	}

	delete iter;
	return running;
}

void UpdateRngfix(Handle unloading = null)
{
	int values[sizeof(gS_RngfixCvars)];
	bool loaded = RngfixRunning(unloading);

	for (int i = 0; i < sizeof(gS_RngfixCvars); i++)
	{
		// The cvars, and so this handle and its hook, stay around after rngfix unloads.
		if (gCV_Rngfix[i] == null && (gCV_Rngfix[i] = FindConVar(gS_RngfixCvars[i])) != null)
		{
			gCV_Rngfix[i].AddChangeHook(OnRngfixCvarChanged);
		}

		if (gCV_Rngfix[i] != null)
		{
			values[i] = gCV_Rngfix[i].IntValue;
		}
		// Versions before rngfix_useoldslopefixlogic only had the new logic; one without any
		// of the others isn't an rngfix the client knows.
		else if (i != sizeof(gS_RngfixCvars) - 1)
		{
			loaded = false;
		}
	}

	char state[64];

	if (loaded)
	{
		FormatEx(state, sizeof(state), "1 %d %d %d %d %d %d %d", values[0], values[1], values[2], values[3], values[4], values[5], values[6]);
	}
	else
	{
		state = "0 0 0 0 0 0 0 0";
	}

	if (StrEqual(state, gS_Rngfix))
	{
		return;
	}

	gS_Rngfix = state;

	for (int i = 1; i <= MaxClients; i++)
	{
		if (IsClientInGame(i))
		{
			SendRngfix(i);
		}
	}
}

void SendRngfix(int client)
{
	if (gB_Predicting[client] && gI_Proto[client] >= 2 && (gI_ClientFlags[client] & CLIENT_RNGFIX))
	{
		ClientCommand(client, "bhoptools_rngcvars %d %s", gI_Session[client], gS_Rngfix);
		Debug(client, "rngfix %s", gS_Rngfix);
	}
}
