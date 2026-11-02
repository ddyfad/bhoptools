#define EFL_CHECK_UNTOUCH (1 << 24)

ConVar gCV_SyncOrigin;
ConVar gCV_DuckTeleportFix;
float gF_PreOrigin[MAXPLAYERS+1][3];
bool gB_PreDucking[MAXPLAYERS+1];

Handle gH_Untouch;

// PostThinkVPhysics resets the physics shadow when FL_DUCKING changes, with the player's velocity.
// After a trigger teleport that command, the destination's fall velocity drives it into the floor.
void FixDuckTeleportShadow(int client)
{
	if (!gCV_DuckTeleportFix.BoolValue || !IsPlayerAlive(client)
		|| ((GetEntityFlags(client) & FL_DUCKING) != 0) == gB_PreDucking[client])
	{
		return;
	}

	float origin[3], velocity[3];
	GetClientAbsOrigin(client, origin);
	GetEntPropVector(client, Prop_Data, "m_vecAbsVelocity", velocity);

	// Moved further than the command could have: teleported.
	if (GetVectorDistance(origin, gF_PreOrigin[client]) < GetVectorLength(velocity) * GetTickInterval() + 64.0)
	{
		return;
	}

	TeleportEntity(client, origin, NULL_VECTOR, view_as<float>({0.0, 0.0, 0.0}));
	// Straight into the fields, as rngfix does, so the shadow keeps the zero velocity.
	SetEntPropVector(client, Prop_Data, "m_vecAbsVelocity", velocity);
	SetEntPropVector(client, Prop_Data, "m_vecVelocity", velocity);
}

// Haze-style landfixes write only m_vecAbsOrigin, and nothing moves m_vecOrigin after that while
// standing still, so predicting clients get pulled back down later.
void SyncNetworkedOrigin(int client)
{
	if (!IsPlayerAlive(client) || GetEntPropEnt(client, Prop_Data, "m_hMoveParent") != -1)
	{
		return;
	}

	float abs[3], net[3];
	GetEntPropVector(client, Prop_Data, "m_vecAbsOrigin", abs);
	GetEntPropVector(client, Prop_Data, "m_vecOrigin", net);

	if (abs[0] != net[0] || abs[1] != net[1] || abs[2] != net[2])
	{
		SetEntPropVector(client, Prop_Send, "m_vecOrigin", abs);
	}
}
