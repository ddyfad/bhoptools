#pragma once

#include <cstddef>

// CMoveData, checked against CPrediction::SetupMove.
const ptrdiff_t MV_Flags         = 0x00; // bit 0 first run, bit 1 m_bGameCodeMovedPlayer
const ptrdiff_t MV_Impulse       = 0x08;
const ptrdiff_t MV_ViewAngles    = 0x0C;
const ptrdiff_t MV_AbsViewAngles = 0x18;
const ptrdiff_t MV_Buttons       = 0x24;
const ptrdiff_t MV_OldButtons    = 0x28;
const ptrdiff_t MV_ForwardMove   = 0x2C;
const ptrdiff_t MV_SideMove      = 0x34;
const ptrdiff_t MV_UpMove        = 0x38;
const ptrdiff_t MV_MaxSpeed      = 0x3C;
const ptrdiff_t MV_Velocity      = 0x44;
const ptrdiff_t MV_Angles        = 0x50;
const ptrdiff_t MV_AbsOrigin     = 0x9C;

const ptrdiff_t UC_CommandNumber = 0x08;
const ptrdiff_t UC_ViewAngles    = 0x10;
const ptrdiff_t UC_Buttons       = 0x28;
const ptrdiff_t UC_Impulse       = 0x2C;

// INetChannelInfo has no destructor, so the slots match on both ABIs (checked against CNetChan's x64 vtable).
const int VT_INetChannelInfo_GetTime        = 2;  // net_time
const int VT_INetChannelInfo_IsValidPacket  = 17; // (int flow, int sequence)
const int VT_INetChannelInfo_GetPacketTime  = 18; // (int flow, int sequence)

// IInput has no destructor, so the slots match on both ABIs (checked against CInput's x64 vtable).
const int VT_IInput_CreateMove = 3; // (int sequence, float frametime, bool active)
const int VT_IInput_GetUserCmd = 8; // (int sequence), null unless that command is still stored

// Checked against the engine's tier1. Registered ones borrow the vtable of the engine's "echo".
struct ConCommandX {
	void*         vtable;
	void*         next;
	bool          registered;
	const char*   name;
	const char*   help;
	int           flags;
	void*         callback;
	void*         completion;
	unsigned char bits;
};
static_assert(offsetof(ConCommandX, name) == 0x18 && offsetof(ConCommandX, flags) == 0x28
	&& offsetof(ConCommandX, callback) == 0x30 && offsetof(ConCommandX, bits) == 0x40, "ConCommand layout");

// ConVar: the parent (itself unless it's a duplicate) and the int value.
static const ptrdiff_t OFF_ConVar_Parent   = 0x38;
static const ptrdiff_t OFF_ConVar_IntValue = 0x58;

struct CCommandX {
	int         argc;
	int         argv0Size;
	char        argS[512];
	char        argvBuf[512];
	const char* argv[64];
};
static_assert(offsetof(CCommandX, argv) == 0x408, "CCommand layout");

template <typename Fn>
Fn VFunc(void* inst, int index) { return reinterpret_cast<Fn>((*reinterpret_cast<void***>(inst))[index]); }
