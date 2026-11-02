ConVar gCV_AutoUpdate;
ConVar gCV_UpdateUrl;
float gF_LastUpdateCheck;
bool gB_UpdateBusy;
bool gB_UpdateStaged;
char gS_UpdateVersion[32];
char gS_UpdateHash[65];

bool UpdateSafeHash(char[] hash)
{
	if (strlen(hash) != 64)
	{
		return false;
	}

	for (int i = 0; i < 64; i++)
	{
		char c = hash[i];

		if (c >= 'A' && c <= 'F')
		{
			hash[i] = c + 32;
		}
		else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
		{
			return false;
		}
	}

	return true;
}

bool UpdateExtensionLoaded()
{
	char error[2];
	return GetExtensionFileStatus("rip.ext", error, sizeof(error)) == 1;
}

bool UpdateSafeVersion(const char[] version)
{
	int length = strlen(version);

	if (length < 1 || length > 31)
	{
		return false;
	}

	for (int i = 0; i < length; i++)
	{
		char c = version[i];

		if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' || c == '-' || c == '_'))
		{
			return false;
		}
	}

	return true;
}

void UpdatePath(char[] buffer, int maxlength, const char[] suffix)
{
	char name[PLATFORM_MAX_PATH];
	GetPluginFilename(GetMyHandle(), name, sizeof(name));
	BuildPath(Path_SM, buffer, maxlength, "plugins/%s%s", name, suffix);
}

int UpdateHumans()
{
	int humans = 0;

	for (int i = 1; i <= MaxClients; i++)
	{
		if (IsClientConnected(i) && !IsFakeClient(i))
		{
			humans++;
		}
	}

	return humans;
}

void Update_OnPluginStart()
{
	gCV_AutoUpdate = CreateConVar("sm_bhoptools_autoupdate", "1", "Keep this plugin up to date from the bhoptools GitHub releases: a newer release is downloaded and the plugin reloads itself when nobody is on or the map ends. Needs the ripext extension.", 0, true, 0.0, true, 1.0);
	gCV_UpdateUrl = CreateConVar("sm_bhoptools_update_url", "https://github.com", "Where the releases are fetched from. Only change this to test.", FCVAR_PROTECTED);
	RegAdminCmd("sm_bhoptools_checkupdate", Command_CheckUpdate, ADMFLAG_RCON, "Check for a bhoptools plugin update now.");
	CreateTimer(60.0, Timer_UpdateApply, _, TIMER_REPEAT);
}

void Update_OnConfigsExecuted()
{
	Update_Check();
}

void Update_OnMapEnd()
{
	if (gB_UpdateStaged)
	{
		Update_Apply();
	}
}

public Action Command_CheckUpdate(int client, int args)
{
	Update_Check(true);
	ReplyToCommand(client, "[bhoptools] checking for an update, see the SourceMod log");
	return Plugin_Handled;
}

public Action Timer_UpdateApply(Handle timer)
{
	if (gB_UpdateStaged && UpdateHumans() == 0)
	{
		Update_Apply();
	}

	return Plugin_Continue;
}

void Update_Check(bool force = false)
{
	if (gB_UpdateBusy || gB_UpdateStaged || (!force && !gCV_AutoUpdate.BoolValue) || StrEqual(BHOPTOOLS_VERSION, "dev") || !UpdateExtensionLoaded())
	{
		return;
	}

	float now = GetEngineTime();

	if (!force && gF_LastUpdateCheck != 0.0 && now - gF_LastUpdateCheck < 3600.0)
	{
		return;
	}

	gF_LastUpdateCheck = now;
	gB_UpdateBusy = true;

	char base[128];
	gCV_UpdateUrl.GetString(base, sizeof(base));
	HTTPClient client = new HTTPClient(base);
	client.SetHeader("User-Agent", "bhoptools-plugin");
	client.Timeout = 30;
	client.Get("ddyfad/bhoptools/releases/download/plugin/manifest.json", Update_OnManifest);
	delete client;
}

public void Update_OnManifest(HTTPResponse response, any value, const char[] error)
{
	if (response.Status != HTTPStatus_OK || response.Data == null)
	{
		gB_UpdateBusy = false;
		LogMessage("Update check failed (%d) %s", view_as<int>(response.Status), error);
		return;
	}

	JSONObject manifest = view_as<JSONObject>(response.Data);
	char version[32], hash[65];
	int size = manifest.GetInt("size");

	if (!manifest.GetString("version", version, sizeof(version)) || !UpdateSafeVersion(version) || StrEqual(version, BHOPTOOLS_VERSION) || size < 1000 || size > 4194304)
	{
		gB_UpdateBusy = false;
		return;
	}

	if (!manifest.GetString("sha256", hash, sizeof(hash)) || !UpdateSafeHash(hash))
	{
		gB_UpdateBusy = false;
		LogMessage("Update to %s has no usable sha256 in its manifest, skipping it", version);
		return;
	}

	strcopy(gS_UpdateVersion, sizeof(gS_UpdateVersion), version);
	strcopy(gS_UpdateHash, sizeof(gS_UpdateHash), hash);

	char path[PLATFORM_MAX_PATH], endpoint[160], base[128];
	UpdatePath(path, sizeof(path), ".new");
	Format(endpoint, sizeof(endpoint), "ddyfad/bhoptools/releases/download/plugin/bhoptools-%s.smx", version);
	gCV_UpdateUrl.GetString(base, sizeof(base));
	HTTPClient client = new HTTPClient(base);
	client.SetHeader("User-Agent", "bhoptools-plugin");
	client.Timeout = 120;
	client.DownloadFile(endpoint, path, Update_OnDownloaded, size);
	delete client;
}

public void Update_OnDownloaded(HTTPStatus status, any size, const char[] error)
{
	gB_UpdateBusy = false;

	char path[PLATFORM_MAX_PATH], hash[65];
	UpdatePath(path, sizeof(path), ".new");

	if (status != HTTPStatus_OK || FileSize(path) != size || !UpdateIsPlugin(path))
	{
		LogMessage("Update to %s failed (%d) %s", gS_UpdateVersion, view_as<int>(status), error);
		DeleteFile(path);
		return;
	}

	if (!Sha256File(path, hash, sizeof(hash)) || !StrEqual(hash, gS_UpdateHash))
	{
		LogMessage("Update to %s doesn't match its sha256, discarding it", gS_UpdateVersion);
		DeleteFile(path);
		return;
	}

	gB_UpdateStaged = true;
	LogMessage("Update to %s downloaded, it takes effect when nobody is on or the map ends", gS_UpdateVersion);

	if (UpdateHumans() == 0)
	{
		Update_Apply();
	}
}

bool UpdateIsPlugin(const char[] path)
{
	File file = OpenFile(path, "rb");

	if (file == null)
	{
		return false;
	}

	int magic = 0;
	bool read = file.ReadInt32(magic);
	delete file;
	return read && magic == 0x53504646;
}

void Update_Apply()
{
	char current[PLATFORM_MAX_PATH], staged[PLATFORM_MAX_PATH], backup[PLATFORM_MAX_PATH], name[PLATFORM_MAX_PATH];
	UpdatePath(current, sizeof(current), "");
	UpdatePath(staged, sizeof(staged), ".new");
	UpdatePath(backup, sizeof(backup), ".bak");
	gB_UpdateStaged = false;

	DeleteFile(backup);

	if (!RenameFile(backup, current))
	{
		LogError("Update: couldn't back up the current plugin");
		DeleteFile(staged);
		return;
	}

	if (!RenameFile(current, staged))
	{
		RenameFile(current, backup);
		LogError("Update: couldn't put the new plugin in place");
		return;
	}

	GetPluginFilename(GetMyHandle(), name, sizeof(name));
	LogMessage("Updated to %s, reloading (the previous version is kept as %s.bak)", gS_UpdateVersion, name);
	ServerCommand("sm plugins reload %s", name);
}
