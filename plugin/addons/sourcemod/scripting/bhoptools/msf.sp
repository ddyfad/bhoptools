// momsurffix2 replaces TryPlayerMove; the client runs the same code with the same cvars, which
// aren't replicated either.
char gS_MsfCvars[][] = { "momsurffix_ramp_bumpcount", "momsurffix_ramp_initial_retrace_length", "momsurffix_enable_noclip_workaround" };
ConVar gCV_Msf[sizeof(gS_MsfCvars)];
char gS_Msf[64];
bool gB_MsfPending;

public void OnMsfCvarChanged(ConVar convar, const char[] oldValue, const char[] newValue)
{
	if (!gB_MsfPending)
	{
		gB_MsfPending = true;
		RequestFrame(Frame_UpdateMsf);
	}
}

public void Frame_UpdateMsf()
{
	gB_MsfPending = false;
	UpdateMsf();
}

bool IsMsf(Handle plugin)
{
	char name[PLATFORM_MAX_PATH];

	if (GetPluginInfo(plugin, PlInfo_Name, name, sizeof(name)) && StrEqual(name, "Momentum surf fix '2"))
	{
		return true;
	}

	GetPluginFilename(plugin, name, sizeof(name));
	int slash = FindCharInString(name, '/', true);
	return StrEqual(name[slash + 1], "momsurffix2.smx", false);
}

bool MsfRunning(Handle unloading)
{
	Handle iter = GetPluginIterator();
	bool running = false;

	while (!running && MorePlugins(iter))
	{
		Handle plugin = ReadPlugin(iter);
		running = plugin != unloading && GetPluginStatus(plugin) == Plugin_Running && IsMsf(plugin);
	}

	delete iter;
	return running;
}

// The plugin's gamedata OSType, 1 Windows or 2 Linux: on Windows it clips velocity with its own
// SourcePawn copy of the old ClipVelocity, on Linux it calls the engine's.
int MsfOS()
{
	static int os;

	if (os == 0)
	{
		GameData gamedata = new GameData("momsurffix2.games");

		if (gamedata != null)
		{
			os = gamedata.GetOffset("OSType");
			delete gamedata;
		}
	}

	return os;
}

void UpdateMsf(Handle unloading = null)
{
	bool loaded = MsfRunning(unloading);

	for (int i = 0; i < sizeof(gS_MsfCvars); i++)
	{
		if (gCV_Msf[i] == null && (gCV_Msf[i] = FindConVar(gS_MsfCvars[i])) != null)
		{
			gCV_Msf[i].AddChangeHook(OnMsfCvarChanged);
		}

		if (gCV_Msf[i] == null)
		{
			loaded = false;
		}
	}

	char state[64];

	if (loaded)
	{
		FormatEx(state, sizeof(state), "1 %d %x %d %d", gCV_Msf[0].IntValue, gCV_Msf[1].FloatValue, gCV_Msf[2].IntValue, MsfOS());
	}
	else
	{
		state = "0 0 0 0 0";
	}

	if (StrEqual(state, gS_Msf))
	{
		return;
	}

	gS_Msf = state;

	for (int i = 1; i <= MaxClients; i++)
	{
		if (IsClientInGame(i))
		{
			SendMsf(i);
		}
	}
}

void SendMsf(int client)
{
	if (gB_Predicting[client] && gI_Proto[client] >= 2 && (gI_ClientFlags[client] & CLIENT_MSF))
	{
		ClientCommand(client, "bhoptools_msf %d %s", gI_Session[client], gS_Msf);
		Debug(client, "momsurffix %s", gS_Msf);
	}
}
