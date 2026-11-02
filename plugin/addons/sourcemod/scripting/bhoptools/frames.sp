// Raw mouse totals sent by the client for cheat detection. Off until it's agreed on; build with
// BHOPTOOLS_FRAMES defined to turn it on.
#if defined BHOPTOOLS_FRAMES

int gI_FrameCmd[MAXPLAYERS+1][256];
int gI_FrameA[MAXPLAYERS+1][256];
int gI_FrameB[MAXPLAYERS+1][256];
GlobalForward gH_OnFrame;

public Action Command_Frame(int client, int args)
{
	if (client == 0 || !gB_Predicting[client] || args < 4 || args > 18 || GetCmdArgInt(1) != gI_Session[client])
	{
		return Plugin_Handled;
	}

	int cmd = GetCmdArgInt(2);

	if (cmd <= 0)
	{
		return Plugin_Handled;
	}

	for (int arg = 3; arg < args; arg += 2, cmd++)
	{
		int slot = cmd % 256;
		gI_FrameCmd[client][slot] = cmd;
		gI_FrameA[client][slot] = GetCmdArgInt(arg);
		gI_FrameB[client][slot] = GetCmdArgInt(arg + 1);

		Call_StartForward(gH_OnFrame);
		Call_PushCell(client);
		Call_PushCell(cmd);
		Call_PushCell(gI_FrameA[client][slot]);
		Call_PushCell(gI_FrameB[client][slot]);
		Call_Finish();
	}

	return Plugin_Handled;
}

public any Native_GetFrame(Handle plugin, int numParams)
{
	int client = GetNativeCell(1);
	int cmd = GetNativeCell(2);

	if (client < 1 || client > MaxClients || cmd <= 0 || gI_FrameCmd[client][cmd % 256] != cmd)
	{
		return false;
	}

	SetNativeCellRef(3, gI_FrameA[client][cmd % 256]);
	SetNativeCellRef(4, gI_FrameB[client][cmd % 256]);
	return true;
}

void Frames_AskPluginLoad2()
{
	CreateNative("Bhoptools_GetFrame", Native_GetFrame);
}

void Frames_OnPluginStart()
{
	RegConsoleCmd("bhoptools_frame", Command_Frame);
	gH_OnFrame = new GlobalForward("Bhoptools_OnFrame", ET_Ignore, Param_Cell, Param_Cell, Param_Cell, Param_Cell);
}

void Frames_OnClientPutInServer(int client)
{
	for (int i = 0; i < 256; i++)
	{
		gI_FrameCmd[client][i] = 0;
	}
}

void Frames_OnHello(int client, int session)
{
	ClientCommand(client, "bhoptools_frames %d 1", session);
}

#else

void Frames_AskPluginLoad2() {}
void Frames_OnPluginStart() {}
void Frames_OnClientPutInServer(int client)
{
	#pragma unused client
}

void Frames_OnHello(int client, int session)
{
	#pragma unused client
	#pragma unused session
}

#endif
