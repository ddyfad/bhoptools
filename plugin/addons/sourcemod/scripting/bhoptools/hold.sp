native bool BetterSeg_GetFreeze(int client);
native void BetterSeg_SetPredictedHold(int client, bool predicted);

bool gB_Betterseg;
int gI_SentHold[MAXPLAYERS+1];
bool gB_HoldPredicted[MAXPLAYERS+1];

bool BetterSegAvailable()
{
	return gB_Betterseg
		&& GetFeatureStatus(FeatureType_Native, "BetterSeg_GetFreeze") == FeatureStatus_Available
		&& GetFeatureStatus(FeatureType_Native, "BetterSeg_SetPredictedHold") == FeatureStatus_Available;
}

bool HoldTimerStarted(int client)
{
	return Shavit_GetTimerStatus(client) != Timer_Stopped && Shavit_GetClientTime(client) > 5.0 * GetTickInterval();
}

bool HoldArmed(int client)
{
	return BetterSeg_GetFreeze(client) && HoldTimerStarted(client) && !Shavit_IsPaused(client);
}

void SendHold(int client, int cmdnum)
{
	bool predicted = BetterSegAvailable() && gB_Predicting[client] && gI_Proto[client] >= 2 && (gI_ClientFlags[client] & CLIENT_HOLD) != 0;

	if (predicted != gB_HoldPredicted[client])
	{
		gB_HoldPredicted[client] = predicted;

		if (BetterSegAvailable())
		{
			BetterSeg_SetPredictedHold(client, predicted);
		}
	}

	if (!predicted)
	{
		return;
	}

	int armed = HoldArmed(client) ? 1 : 0;

	if (armed != gI_SentHold[client])
	{
		gI_SentHold[client] = armed;
		ClientCommand(client, "bhoptools_hold %d %d %d", gI_Session[client], armed, cmdnum);
		Debug(client, "hold %d", armed);
	}
}

public void OnClientDisconnect_Post(int client)
{
	gB_HoldPredicted[client] = false;
}
