// Angled teleports for clients that turn their own view.
// With pl.fixangle cleared the server takes the client's angles from the next command on and sends no
// snap, so the client can turn straight away.

int gI_TeleportFixangle[MAXPLAYERS+1];

public MRESReturn Hook_TeleportViewPre(int client, DHookParam params)
{
	gI_TeleportFixangle[client] = GetEntData(client, gI_PlayerState + FIXANGLE_OFFSET);
	return MRES_Ignored;
}

public MRESReturn Hook_TeleportViewPost(int client, DHookParam params)
{
	int want = CLIENT_PREDICTS_TRIGGER_TELEPORTS | CLIENT_TELEPORT_VIEW;

	// Our own teleports clear the snap themselves, or keep it for a client that didn't predict them.
	if (params.IsNull(2) || gB_OwnTeleport[client] || !gB_Predicting[client] || gI_Proto[client] < 2
		|| (gI_ClientFlags[client] & want) != want)
	{
		return MRES_Ignored;
	}

	// Only an absolute snap this teleport made. One already waiting, or a relative one, goes out as usual.
	if (gI_TeleportFixangle[client] != 0 || GetEntData(client, gI_PlayerState + FIXANGLE_OFFSET) != 1)
	{
		return MRES_Ignored;
	}

	SetEntData(client, gI_PlayerState + FIXANGLE_OFFSET, 0);

	float angles[3];
	GetClientEyeAngles(client, angles);
	ClientCommand(client, "bhoptools_tsnap %d %d %x %x %x", gI_Session[client], gI_RunCmd[client], angles[0], angles[1], angles[2]);
	Debug(client, "cmd %d: teleport turned the view to %.1f %.1f, snap cleared", gI_RunCmd[client], angles[0], angles[1]);
	return MRES_Ignored;
}
