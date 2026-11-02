#pragma once

#include "momsurffix_sim.h"

namespace msf {

bool Init(void* gameMovement);
// Windows puts the slots back when the launcher exits.
void Attach();
void Detach();

// Null outside ProcessMovement or with prediction off, which leaves stock movement. CheckStuck runs the
// server's way even without momsurffix.
struct Player {
	char*        pl;
	int          groundOffset, moveTypeOffset, frictionOffset;
	const float* frametime;  // &gpGlobals->frametime, which ProcessMovement scales by laggedmovement
	float        bounce;
	int          key;        // command number, or the tickbase without one
	float        curtime;    // the command's time, for the server's real-time CheckStuck spacing
};
void Arm(const Player* p);

void SetSettings(const MsfSettings& s, bool log);

void Describe(char* buf, size_t size);

// The client's IMoveHelper, null if Init didn't find it.
void* MoveHelper();

}
