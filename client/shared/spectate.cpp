#include "spectate.h"
#include "teleport_predict.h"
#include "sigs.h"
#include "utils.h"
#include "game_layout.h"

#include <algorithm>

namespace spectate {
namespace {

typedef bool  (*InterpolateFn)(void*, float);
typedef bool  (*ShouldInterpolateFn)(void*);
typedef void  (*CalcInEyeCamViewFn)(void*, float*, float*, float*);
typedef void* (*EntityFn)(void*);
typedef float (*InterpAmountFn)();

// How long after the first-person view last asked for the watched player they are still let back
// on the list, so that leaving first person lets them go again.
const double kTargetHold = 0.5;

// The default cl_interp, where first-person spectating is smooth.
const float kSpectateInterp = 0.1f;

void** g_vt = nullptr;
InterpolateFn       oInterpolate;
ShouldInterpolateFn oShouldInterpolate;
CalcInEyeCamViewFn  oCalcInEyeCamView;
InterpAmountFn      g_interpAmount;

void*  g_target;
double g_targetSeen;

bool Watched(void* player)
{
	return player && player == g_target && MonotonicSeconds() - g_targetSeen < kTargetHold;
}

float Extra()
{
	// The game's own allowance: the lerp in whole ticks, and one more.
	const float interval = TeleportPredict_TickInterval();
	const float game = ((int)(g_interpAmount() / interval + 0.5f) + 1) * interval;
	return (std::max)(kSpectateInterp - game, 0.0f);
}

bool Hooked_Interpolate(void* player, float currentTime)
{
	if (g_interpAmount && Watched(player))
		currentTime -= Extra();
	return oInterpolate(player, currentTime);
}

bool Hooked_ShouldInterpolate(void* entity)
{
	return Watched(entity) || oShouldInterpolate(entity);
}

// Only watched: the one call that says a first-person view is drawn right now, and of whom.
void Hooked_CalcInEyeCamView(void* player, float* eye, float* angles, float* fov)
{
	oCalcInEyeCamView(player, eye, angles, fov);
	g_target = VFunc<EntityFn>(player, VT_CSPlayer_GetObserverTarget)(player);
	g_targetSeen = MonotonicSeconds();
}

}

bool Install()
{
	if (g_vt) return true;
	void** vt = FindVTable(CLIENT_MODULE, RTTI_CSPlayer);
	if (!vt) return false;
	g_vt = vt;
	for (int slot : { VT_CSPlayer_Interpolate, VT_CSPlayer_ShouldInterpolate, VT_CSPlayer_GetObserverTarget, VT_CSPlayer_CalcInEyeCamView })
		if (!InModuleCode(CLIENT_MODULE, vt[slot])) {
			CoreLog("spectate: the player class isn't the client build this was written for, or is already hooked");
			return true;
		}

	g_interpAmount = reinterpret_cast<InterpAmountFn>(FindPatternIn(CLIENT_MODULE, SIG_GetClientInterpAmount));
	if (!g_interpAmount) CoreLog("spectate: GetClientInterpAmount not found, so the watched player gets no extra interp");

	oInterpolate = reinterpret_cast<InterpolateFn>(vt[VT_CSPlayer_Interpolate]);
	oShouldInterpolate = reinterpret_cast<ShouldInterpolateFn>(vt[VT_CSPlayer_ShouldInterpolate]);
	oCalcInEyeCamView = reinterpret_cast<CalcInEyeCamViewFn>(vt[VT_CSPlayer_CalcInEyeCamView]);
	if (!PatchPointer(&vt[VT_CSPlayer_Interpolate], reinterpret_cast<void*>(&Hooked_Interpolate))
			|| !PatchPointer(&vt[VT_CSPlayer_ShouldInterpolate], reinterpret_cast<void*>(&Hooked_ShouldInterpolate))
			|| !PatchPointer(&vt[VT_CSPlayer_CalcInEyeCamView], reinterpret_cast<void*>(&Hooked_CalcInEyeCamView)))
		CoreLog("spectate: couldn't patch the player's vtable");
	return true;
}

#ifdef _WIN32
void Uninstall()
{
	if (!oCalcInEyeCamView) return;
	PatchPointer(&g_vt[VT_CSPlayer_Interpolate], reinterpret_cast<void*>(oInterpolate));
	PatchPointer(&g_vt[VT_CSPlayer_ShouldInterpolate], reinterpret_cast<void*>(oShouldInterpolate));
	PatchPointer(&g_vt[VT_CSPlayer_CalcInEyeCamView], reinterpret_cast<void*>(oCalcInEyeCamView));
	oCalcInEyeCamView = nullptr;
}
#endif

}
