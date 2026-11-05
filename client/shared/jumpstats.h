// bgs-jumpstats' HUD for our own play (jhud, trainer, prestrafe trainer, offsets, speedometer, showyaws, showkeys, ssj),
// drawn from our first-run commands with bhop-get-stats' numbers worked out here. The plugin sends
// the player's settings and leaves those out for us while we draw them.
#pragma once

struct CCommandX;

namespace js {

// A new session, or the plugin left: nothing is drawn until the plugin sends settings again.
void Reset(int session);
// From bhoptools_caps: our entity index and SayText2's id.
void SetChat(int client, int sayText2);

void Cmd_Settings(const CCommandX& c);
void Cmd_Colors(const CCommandX& c);
void Cmd_Comparison(const CCommandX& c);
void Cmd_Jump(const CCommandX& c);
void Cmd_Time(const CCommandX& c);

// Every IMoveHelper::AddToTouched in our move is a touch for bhop-get-stats.
void Attach(void* moveHelper);
#ifdef _WIN32
void Detach();
#endif

// A first-run command of ours while alive, as the plugins see it in OnPlayerRunCmdPre.
struct Move {
	int   cmd;
	int   buttons;
	float forward, side;
	float angles[3];
	float velocity[3];
	float origin[3];
	bool  onGround;
	bool  noStats;  // movetype none, noclip or ladder, or water level 2 and up
	float maxSpeed, lagged;
	float surfaceFriction, stamina;  // m_surfaceFriction and m_flStamina before the move
};
void PreMove(const Move& m);
// CheckJumpButton jumped; counts only between PreMove and PostMove.
void OnJump();
void PostMove(const float origin[3], const float velocity[3]);
// A predicted checkpoint teleport: Shavit_OnTeleport.
void OnTeleport();
// Once a rendered frame (IN_SetSampleTime), for the HUDs drawn per frame.
void OnFrame();

}
