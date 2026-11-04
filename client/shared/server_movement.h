// Float math is float32 throughout, like SourcePawn's.
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

// bhoptools_move's landfix field: SuperLandfix_GetMethod's numbers, and 3 for the SourceJump plugin's
// landfix, only taken from a server whose caps say it follows SourceJump's rules.
enum LandfixMethod { LANDFIX_OFF = 0, LANDFIX_HAZE = 1, LANDFIX_OLIVIA = 2, LANDFIX_SOURCEJUMP = 3 };

struct MoveSettings {
	float runspeed = 0.0f;       // 0: GetPlayerMaxSpeed isn't overridden
	int   landfix  = LANDFIX_OFF;
	bool  easybhop = false;
	std::vector<int> hazeSkip;
};


// CGameMovement::ProcessMovement stores player->GetPlayerMaxSpeed() to mv->m_flMaxSpeed (+0x3c):
// a call through [reg+disp32], then a store of xmm0 to [reg+0x3c] within a few instructions.
// Returns that vtable index, or -1.
static inline int FindMaxSpeedVcall(const uint8_t* code, size_t len)
{
	for (size_t i = 0; i + 6 <= len; ++i) {
		// FF /2 with mod 10 and no SIB: call qword ptr [reg+disp32].
		if (code[i] != 0xFF || (code[i + 1] & 0xF8) != 0x90 || (code[i + 1] & 7) == 4) continue;
		int32_t disp;
		memcpy(&disp, code + i + 2, 4);
		if (disp <= 0 || disp > 0x2000 || disp % 8) continue;
		for (size_t j = i + 6; j < i + 6 + 32 && j + 7 <= len; ++j) {
			size_t k = j;
			if (code[k++] != 0xF3) continue;
			if ((code[k] & 0xF0) == 0x40) ++k;
			if (code[k] != 0x0F || code[k + 1] != 0x11) continue;
			k += 2;
			const uint8_t modrm = code[k++];
			// movss [reg+disp8], xmm0
			if ((modrm & 0xC0) != 0x40 || (modrm & 0x38) != 0) continue;
			if ((modrm & 7) == 4) ++k;
			if (k < len && code[k] == 0x3C) return disp / 8;
		}
	}
	return -1;
}

// SuperLandfix's Haze type: in OnPlayerRunCmd, before the move, when
// m_hGroundEntity differs from the last command's and isn't none. SuperLandfix remembers a
// skipped ground too, so the history is simply every command's ground at its start.
static inline bool LandfixHazeWanted(bool havePrev, int prevGround, int ground, const std::vector<int>& skip)
{
	if (!havePrev || ground == prevGround || ground == -1) return false;
	for (int s : skip)
		if (s == ground) return false;
	return true;
}

// GetGroundUnits, then origin[2] += 1.5 - units. `traced`: alive, walking, water level <= 1 and
// the 2 unit hull trace down hit; otherwise the units are 0 and the lift is a full 1.5.
static inline float LandfixHazeZ(float z, bool traced, float endZ)
{
	float units = 0.0f;
	if (traced) {
		// RoundToFloor returns a cell, so the subtraction converts it back to float.
		float defaultHeight = endZ - (float)(int)floorf(endZ);
		if (defaultHeight > 0.03125f) defaultHeight = 0.03125f;
		units = z - endZ + defaultHeight;
	}
	return z + (1.5f - units);
}

// Olivia/NoTimeLoss: a pre hook on CheckJumpButton. The mv origin moves before the jump when
// none of these stop it.
struct OliviaState {
	bool  alive;
	float waterJumpTime;
	int   waterLevel;
	bool  onGround;      // m_hGroundEntity isn't none
	int   oldButtons;
};

static inline bool LandfixOliviaArmed(const OliviaState& s)
{
	const int IN_JUMP = 1 << 1, IN_DUCK = 1 << 2;
	return s.alive && s.waterJumpTime == 0.0f && s.waterLevel < 2 && s.onGround
		&& (s.oldButtons & (IN_JUMP | IN_DUCK)) == 0;
}

// After a hit of the same 2 unit hull trace down from the mv origin.
static inline bool LandfixOliviaZ(float z, float endZ, float* out)
{
	const float diff = fabsf(endZ - z);
	if (diff < 0.49f) { *out = endZ + 0.49f; return true; }
	if (diff > 1.5f && diff < 2.0f) { *out = endZ + 1.5f; return true; }
	return false;
}

// SourceJump's landfix.sp counts the commands that ended on the ground in OnPlayerRunCmdPost and lifts only
// a jump with at most one before it: a bhop on the first ground tick, or a jumpbug. The ground at a
// command's start is the last one's end; without that history the jump is taken as a bhop.
static inline bool LandfixSjJumpLifts(bool havePrev, int prevGround, int ground)
{
	return ground == -1 || !havePrev || prevGround == -1;
}

// Its lift after CheckJumpButton decides a jump. `hull(start, end, &hit, &startSolid, &endZ)` is the
// player's hull traced with MASK_PLAYERSOLID past other players, false when it can't trace. From 2 above
// the origin to 2.1 below it must hit ground, and 1.5 above that must be clear.
template <typename Hull>
static inline bool LandfixSjZ(const float origin[3], Hull hull, float* out)
{
	float above[3] = { origin[0], origin[1], origin[2] + 2.0f };
	float below[3] = { origin[0], origin[1], origin[2] - 2.1f };
	bool hit, startSolid;
	float endZ;
	if (!hull(above, below, &hit, &startSolid, &endZ) || !hit || startSolid) return false;
	below[2] = endZ;
	above[2] = endZ + 1.5f;
	if (!hull(below, above, &hit, &startSolid, &endZ) || hit || startSolid) return false;
	*out = above[2];
	return true;
}

// Each command's ground entity at its start, by command number, so a replay from a server
// snapshot knows the ground of the command before it.
struct GroundLog {
	static const int kSize = 256;
	int  key[kSize];
	int  ground[kSize];
	bool used[kSize];

	GroundLog() { Clear(); }
	void Clear() { memset(used, 0, sizeof(used)); }
	void Put(int k, int g)
	{
		const int i = k & (kSize - 1);
		key[i] = k; ground[i] = g; used[i] = true;
	}
	bool Get(int k, int* g) const
	{
		const int i = k & (kSize - 1);
		if (!used[i] || key[i] != k) return false;
		*g = ground[i];
		return true;
	}
};
