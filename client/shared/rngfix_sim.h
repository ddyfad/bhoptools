// Port of rngfix.sp 1.1.3 (github.com/jason-e/rngfix). Any
// difference from the server is a misprediction, so check, trace, hull and evaluation order are
// rngfix's, and floats are 32-bit like SourcePawn's.
#pragma once

#include <cmath>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "rngfix_api.h"
#include "prediction.h"

static const float RNG_LAND_HEIGHT          = 2.0f;
static const float RNG_NON_JUMP_VELOCITY    = 140.0f;
static const float RNG_MIN_STANDABLE_ZNRM   = 0.7f;
static const float RNG_AIR_SPEED_CAP        = 30.0f;
static const float RNG_DEFAULT_JUMP_IMPULSE = 301.99337741f; // sqrt(2 * 57 * 800)

static const int RNG_IN_JUMP = 1 << 1;
static const int RNG_IN_DUCK = 1 << 2;

static const int RNG_NO_TICK   = INT_MIN / 2; // "never", and rngfix's 0 after PreventCollision
static const int RNG_RING_SIZE = 256;

static const int RNG_UPHILL_LOSS    = -1;
static const int RNG_UPHILL_NEUTRAL = 1;

enum RngFixBits {
	RNG_PREDICTED        = 1 << 0,
	RNG_COLLISION        = 1 << 1,
	RNG_FIX_UPHILL       = 1 << 2,
	RNG_FIX_EDGE         = 1 << 3,
	RNG_FIX_INCLINE      = 1 << 4,
	RNG_FIX_TRIGGERJUMP  = 1 << 5,
	RNG_FIX_STAIRS       = 1 << 6,
	RNG_FIX_TELEHOP      = 1 << 7,
	RNG_LANDED           = 1 << 8,
	RNG_SJ_BOOSTER_SKIP  = 1 << 9, // the trigger jump fix's touch of a booster, refused by landfix.sp
};

struct RngEnv {
	float gravity       = 800.0f;
	float airAccelerate = 10.0f;
	float maxVelocity   = 3500.0f;
	float jumpImpulse   = RNG_DEFAULT_JUMP_IMPULSE; // sv_jump_impulse is CS:GO only
	bool  autoBunnyhop  = false; // CS:GO only
	float stepSize      = 18.0f;
	bool  surfMap       = false;
	float hullStand     = 62.0f;
	float hullDuck      = 45.0f;
};

static void RngSetMapName(RngEnv& env, const char* map)
{
	if (!map) { env.surfMap = false; return; }
	const char* base = map;
	for (const char* p = map; *p; ++p)
		if (*p == '/' || *p == '\\') base = p + 1;
	env.surfMap = strncasecmp(base, "surf_", 5) == 0;
}

struct RngMoveInput {
	int   buttons = 0;
	int   oldButtons = 0;
	float forwardMove = 0.0f;
	float sideMove = 0.0f;
	float maxSpeed = 0.0f;
	float viewAngles[3] = {0, 0, 0};
	bool  ducking = false;
	bool  alive = true;
	int   waterLevel = 0;
	float laggedMovement = 1.0f;
	// rngfix's IsDuckCoolingDown needs props stock CS:S doesn't have, so it's always false there.
	bool  duckCoolingDown = false;
};

struct RngPlayerState {
	int   lastTickPredicted = RNG_NO_TICK;
	int   lastCollisionTick = RNG_NO_TICK;
	int   lastLandTick = RNG_NO_TICK;
	bool  lastOnGround = true; // rngfix's g_iLastGroundEnt starts at 0 (the world)
	// CanJump reads these before they're refreshed.
	int   buttons = 0;
	int   oldButtons = 0;
	float preCollisionVelocity[3] = {0, 0, 0};
	float lastBaseVelocity[3] = {0, 0, 0};
	float collisionPoint[3] = {0, 0, 0};
	float collisionNormal[3] = {0, 0, 0};
};

// The stairs fix only needs to know one is there; its filter is unknown, so it counts as passing.
struct RngOtherTrigger {
	int       modelIndex;
	PredXform xf;
	int       spawnflags;
};

struct RngRuntime {
	RngSettings settings;
	RngEnv      env;
	RngTracer*  tracer = nullptr;

	RngPlayerState st;
	RngPlayerState ring[RNG_RING_SIZE];
	int            ringKey[RNG_RING_SIZE];
	int            prevKey = INT_MIN;
	bool           initialized = false;

	int          tick = 0;
	bool         inTick = false;
	bool         postThinkDone = false;
	bool         preOnGround = false;
	int          waterLevelPost = 0;
	int          moveTypePost = PRED_MOVETYPE_WALK;
	float        frameTime = 0.0f;
	RngMoveInput in;
	unsigned     fixes = 0;

	const PredMap*               otherFor = nullptr;
	std::vector<RngOtherTrigger> other;
	std::vector<int>             scratch;

	RngRuntime() { for (int& t : ringKey) t = INT_MIN; }
};

static void RngReset(RngRuntime& r)
{
	r.st = RngPlayerState();
	for (int& t : r.ringKey) t = INT_MIN;
	r.prevKey = INT_MIN;
	r.initialized = false;
	r.inTick = false;
	r.otherFor = nullptr;
	r.other.clear();
}

static int RngSlot(int tick) { return ((tick % RNG_RING_SIZE) + RNG_RING_SIZE) % RNG_RING_SIZE; }

// Same rule as PredCommandStart: a replayed tick starts from the state the tick before it
// ended with. Without that snapshot, forget ticks from "the future".
static void RngCommandStart(RngRuntime& r, int tick, int key)
{
	if (key <= r.prevKey || !r.initialized) {
		const int prev = key - 1;
		const int slot = RngSlot(prev);
		if (r.ringKey[slot] == prev) {
			r.st = r.ring[slot];
		} else {
			RngPlayerState& s = r.st;
			if (s.lastTickPredicted >= tick) s.lastTickPredicted = RNG_NO_TICK;
			if (s.lastCollisionTick >= tick) s.lastCollisionTick = RNG_NO_TICK;
			if (s.lastLandTick >= tick)      s.lastLandTick = RNG_NO_TICK;
		}
	}
	r.initialized = true;
}

static void RngCommandEnd(RngRuntime& r, int key)
{
	const int slot = RngSlot(key);
	r.ring[slot] = r.st;
	r.ringKey[slot] = key;
	r.prevKey = key;
}

struct RngTrace {
	RngTraceResult tr;
	bool DidHit() const { return tr.fraction < 1.0f || tr.allSolid || tr.startSolid; }
};

static bool RngTraceHull(RngRuntime& r, const float start[3], const float end[3], const float mins[3],
	const float maxs[3], RngTrace& t)
{
	t.tr = RngTraceResult();
	return r.tracer && r.tracer->TraceHull(start, end, mins, maxs, t.tr);
}

static void RngCopy(float d[3], const float s[3]) { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; }

static float RngDot(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

// SourceMod's NormalizeVector leaves a zero vector alone.
static float RngNormalize(const float v[3], float out[3])
{
	const float len = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
	if (len != 0.0f) {
		out[0] = v[0] / len; out[1] = v[1] / len; out[2] = v[2] / len;
	} else {
		out[0] = v[0]; out[1] = v[1]; out[2] = v[2];
	}
	return len;
}

static void RngAngleVectors(const float ang[3], float fwd[3], float right[3])
{
	float sp, cp, sy, cy, sr, cr;
	PredSinCos(ang[1], &sy, &cy);
	PredSinCos(ang[0], &sp, &cp);
	PredSinCos(ang[2], &sr, &cr);
	fwd[0] = cp * cy;
	fwd[1] = cp * sy;
	fwd[2] = -sp;
	right[0] = (-1 * sr * sp * cy + -1 * cr * -sy);
	right[1] = (-1 * sr * sp * sy + -1 * cr * cy);
	right[2] = -1 * sr * cp;
}

static float RngLocalGravity(const PredPlayerIO& io)
{
	const float g = io.gravity ? *io.gravity : 1.0f;
	return g == 0.0f ? 1.0f : g;
}

// shavit-core's OnPlayerRunCmd runs before rngfix's ProcessMovement pre hook and clears IN_JUMP from
// m_nOldButtons when autobhop is on. sv_autobunnyhopping stays 0 on the server, so this is all of
// autobhop that rngfix's CanJump sees.
static void RngShavitAutobhop(RngMoveInput& in, bool autobhop, int moveType)
{
	if (autobhop && (in.buttons & RNG_IN_JUMP) != 0 && moveType == PRED_MOVETYPE_WALK && in.waterLevel < 2)
		in.oldButtons &= ~RNG_IN_JUMP;
}

// Uses the stored buttons, which RunPreTickChecks reads before refreshing.
static bool RngCanJump(const RngRuntime& r)
{
	if ((r.st.buttons & RNG_IN_JUMP) == 0) return false;
	if ((r.st.oldButtons & RNG_IN_JUMP) != 0 && !r.env.autoBunnyhop) return false;
	return true;
}

static void RngCheckVelocity(const RngRuntime& r, float v[3])
{
	const float m = r.env.maxVelocity;
	for (int i = 0; i < 3; ++i) {
		if      (v[i] >  m) v[i] =  m;
		else if (v[i] < -m) v[i] = -m;
	}
}

// Basevelocity isn't cleared like the engine does: this is only a prediction.
static void RngStartGravity(const RngRuntime& r, const PredPlayerIO& io, float v[3], const float baseVel[3])
{
	const float localGravity = RngLocalGravity(io);
	v[2] -= localGravity * r.env.gravity * 0.5f * r.frameTime;
	v[2] += baseVel[2] * r.frameTime;
	RngCheckVelocity(r, v);
}

static void RngFinishGravity(const RngRuntime& r, const PredPlayerIO& io, float v[3])
{
	const float localGravity = RngLocalGravity(io);
	v[2] -= localGravity * r.env.gravity * 0.5f * r.frameTime;
	RngCheckVelocity(r, v);
}

static bool RngDuck(RngRuntime& r, const PredPlayerIO& io, float origin[3], float mins[3], float maxs[3])
{
	const bool ducking = (*io.flags & FL_DUCKING) != 0;
	bool nextDucking = ducking;
	const float duckDelta = (r.env.hullStand - r.env.hullDuck) / 2;
	const float hullMins[3] = { -16.0f, -16.0f, 0.0f };
	const float maxsUnducked[3] = { 16.0f, 16.0f, r.env.hullStand };
	const float maxsDucked[3] = { 16.0f, 16.0f, r.env.hullDuck };

	if ((r.st.buttons & RNG_IN_DUCK) != 0 && !ducking) {
		if (!r.in.duckCoolingDown) {
			origin[2] += duckDelta;
			nextDucking = true;
		}
	} else if ((r.st.buttons & RNG_IN_DUCK) == 0 && ducking) {
		origin[2] -= duckDelta;
		RngTrace t;
		if (!RngTraceHull(r, origin, origin, hullMins, maxsUnducked, t)) return false;
		if (t.DidHit()) origin[2] += duckDelta;
		else nextDucking = false;
	}

	RngCopy(mins, hullMins);
	RngCopy(maxs, nextDucking ? maxsDucked : maxsUnducked);
	return true;
}

// CS:S: jumping while ducked sets the Z velocity rather than adding to it.
static void RngCheckJumpButton(const RngRuntime& r, const PredPlayerIO& io, float v[3])
{
	if ((*io.flags & FL_ONGROUND) == 0) return;
	if (!RngCanJump(r)) return;
	if (r.in.ducking || (*io.flags & FL_DUCKING) != 0) v[2] = r.env.jumpImpulse;
	else v[2] += r.env.jumpImpulse;
	RngFinishGravity(r, io, v);
}

// Includes the start of AirMove. No surface friction, same as rngfix.
static void RngAirAccelerate(const RngRuntime& r, float v[3])
{
	float fore[3], side[3], wishvel[3] = {0, 0, 0}, wishdir[3];
	RngAngleVectors(r.in.viewAngles, fore, side);
	fore[2] = 0.0f;
	side[2] = 0.0f;
	RngNormalize(fore, fore);
	RngNormalize(side, side);

	const float vel[2] = { r.in.forwardMove, r.in.sideMove };
	for (int i = 0; i < 2; ++i) wishvel[i] = fore[i] * vel[0] + side[i] * vel[1];

	float wishspeed = RngNormalize(wishvel, wishdir);
	const float maxSpeed = r.in.maxSpeed;
	if (wishspeed > maxSpeed && maxSpeed != 0.0f) wishspeed = maxSpeed;

	if (wishspeed != 0.0f) {
		float wishspd = wishspeed;
		if (wishspd > RNG_AIR_SPEED_CAP) wishspd = RNG_AIR_SPEED_CAP;
		const float currentspeed = RngDot(v, wishdir);
		const float addspeed = wishspd - currentspeed;
		if (addspeed > 0) {
			float accelspeed = r.env.airAccelerate * wishspeed * r.frameTime;
			if (accelspeed > addspeed) accelspeed = addspeed;
			for (int i = 0; i < 2; ++i) v[i] += accelspeed * wishdir[i];
		}
	}
}

// No overbounce adjust: only used on walkable surfaces.
static void RngClipVelocity(const float v[3], const float n[3], float out[3])
{
	const float backoff = RngDot(v, n);
	for (int i = 0; i < 3; ++i) out[i] = v[i] - n[i] * backoff;
}

static void RngPreventCollision(RngRuntime& r, PredPlayerIO& io, const float collisionPoint[3], const float velocityTick[3])
{
	float o[3];
	for (int i = 0; i < 3; ++i) o[i] = collisionPoint[i] - velocityTick[i];
	o[2] += 0.1f;
	RngCopy(io.origin, o);
	r.st.lastCollisionTick = RNG_NO_TICK;
}

// Both of rngfix's SetVelocity paths end with the same velocity and basevelocity.
static void RngSetVelocity(const RngRuntime& r, PredPlayerIO& io, const float v[3])
{
	for (int i = 0; i < 3; ++i) io.velocity[i] = v[i] - r.st.lastBaseVelocity[i];
}

// mins/maxs end up as the quadrant last traced, like rngfix's, and `t` as its trace.
static bool RngTracePlayerBBoxForGround(RngRuntime& r, const float origin[3], const float originBelow[3],
	float mins[3], float maxs[3], RngTrace& t, bool& failed)
{
	failed = false;
	float origMins[3], origMaxs[3];
	RngCopy(origMins, mins);
	RngCopy(origMaxs, maxs);

	for (int q = 0; q < 4; ++q) {
		switch (q) {
		case 0:
			RngCopy(mins, origMins);
			maxs[0] = origMaxs[0] > 0.0f ? 0.0f : origMaxs[0];
			maxs[1] = origMaxs[1] > 0.0f ? 0.0f : origMaxs[1];
			maxs[2] = origMaxs[2];
			break;
		case 1:
			mins[0] = origMins[0] < 0.0f ? 0.0f : origMins[0];
			mins[1] = origMins[1] < 0.0f ? 0.0f : origMins[1];
			mins[2] = origMins[2];
			RngCopy(maxs, origMaxs);
			break;
		case 2:
			mins[0] = origMins[0];
			mins[1] = origMins[1] < 0.0f ? 0.0f : origMins[1];
			mins[2] = origMins[2];
			maxs[0] = origMaxs[0] > 0.0f ? 0.0f : origMaxs[0];
			maxs[1] = origMaxs[1];
			maxs[2] = origMaxs[2];
			break;
		default:
			mins[0] = origMins[0] < 0.0f ? 0.0f : origMins[0];
			mins[1] = origMins[1];
			mins[2] = origMins[2];
			maxs[0] = origMaxs[0];
			maxs[1] = origMaxs[1] > 0.0f ? 0.0f : origMaxs[1];
			maxs[2] = origMaxs[2];
			break;
		}
		if (!RngTraceHull(r, origin, originBelow, mins, maxs, t)) { failed = true; return false; }
		if (t.DidHit() && t.tr.normal[2] >= RNG_MIN_STANDABLE_ZNRM) return true;
	}
	return false;
}

// Disabled triggers have no FSOLID_TRIGGER, so they aren't enumerated.
static void RngFindTriggers(PredRuntime& rt, const float feet[3], const float mins[3],
	const float maxs[3], std::vector<int>& out)
{
	out.clear();
	const PredMap& map = *rt.map;
	PredGatherCandidates(rt, feet, mins, maxs);
	for (int i : rt.candidates) {
		const PredSimEnt& e = map.sim[(size_t)i];
		const PredEntState& st = rt.world.ents[(size_t)i];
		if (!PredIsTriggerClass(e.cls) || st.removed || st.disabled) continue;
		if (PredBoxInModel(map, e.modelIndex, e.xf, feet, mins, maxs, -1)) out.push_back(i);
	}
}

static void RngBuildOtherTriggers(RngRuntime& r, const PredMap& map)
{
	if (r.otherFor == &map) return;
	r.otherFor = &map;
	r.other.clear();
	for (const PredMapEntity& ent : map.entities) {
		const char* cls = ent.Get("classname");
		if (!strstr(cls, "trigger_") || PredIsTriggerClass(PredClassify(cls))) continue;
		const char* model = ent.Get("model");
		if (model[0] != '*') continue;
		if (atoi(ent.Get("StartDisabled", "0")) != 0) continue;
		RngOtherTrigger t;
		t.modelIndex = atoi(model + 1);
		if (t.modelIndex <= 0 || (size_t)t.modelIndex >= map.models.size()) continue;
		float org[3], ang[3];
		PredParseVec3(ent.Get("origin", "0 0 0"), org);
		PredParseVec3(ent.Get("angles", "0 0 0"), ang);
		PredAngleMatrix(ang, org, t.xf);
		t.spawnflags = PredApplySpawnFixup(PC_NONE, atoi(ent.Get("spawnflags", "0")));
		r.other.push_back(t);
	}
}

static bool RngTriggerBlocksStairs(RngRuntime& r, PredRuntime* rt, PredPlayerIO& io, const float start[3],
	const float mins[3], const float maxs[3])
{
	if (!rt) return false;
	const PredMap& map = *rt->map;
	RngFindTriggers(*rt, start, mins, maxs, r.scratch);
	for (int i : r.scratch)
		if (PredPassesFilters(map, rt->world, map.sim[(size_t)i], io)) return true;

	RngBuildOtherTriggers(r, map);
	const bool client = (*io.flags & FL_CLIENT_BIT) != 0;
	for (const RngOtherTrigger& t : r.other) {
		if (!PredBoxInModel(map, t.modelIndex, t.xf, start, mins, maxs, -1)) continue;
		const int sf = t.spawnflags;
		if (((sf & 0x40) || ((sf & 0x01) && client)) && !(sf & 0x20)) return true;
	}
	return false;
}

static void RngRunPreTickChecks(RngRuntime& r, PredPlayerIO& io)
{
	RngPlayerState& st = r.st;
	const RngSettings& s = r.settings;
	const int tick = r.tick;

	if (!r.in.alive) return;
	if (io.moveType != PRED_MOVETYPE_WALK) return;
	if (r.in.waterLevel > 1) return;

	st.lastOnGround = r.preOnGround;

	// Uses last predicted tick's buttons, like rngfix.
	if (st.lastOnGround && !RngCanJump(r)) return;

	st.lastTickPredicted = tick;
	r.fixes |= RNG_PREDICTED;

	st.buttons = r.in.buttons;
	st.oldButtons = r.in.oldButtons;

	float velocity[3], baseVelocity[3], origin[3], nextOrigin[3], mins[3], maxs[3];
	RngCopy(velocity, io.velocity);
	RngCopy(baseVelocity, io.baseVelocity);
	RngCopy(origin, io.origin);
	RngCopy(nextOrigin, origin);

	// Don't let PostThink act on half a prediction.
	if (!RngDuck(r, io, nextOrigin, mins, maxs)) { st.lastTickPredicted = RNG_NO_TICK; return; }
	RngStartGravity(r, io, velocity, baseVelocity);
	RngCheckJumpButton(r, io, velocity);
	RngCheckVelocity(r, velocity);
	RngAirAccelerate(r, velocity);

	baseVelocity[2] = 0.0f;
	RngCopy(st.lastBaseVelocity, baseVelocity);
	for (int i = 0; i < 3; ++i) velocity[i] += baseVelocity[i];

	RngCopy(st.preCollisionVelocity, velocity);

	float velocityTick[3];
	for (int i = 0; i < 3; ++i) velocityTick[i] = velocity[i] * r.frameTime;
	for (int i = 0; i < 3; ++i) nextOrigin[i] += velocityTick[i];

	RngTrace t;
	if (!RngTraceHull(r, origin, nextOrigin, mins, maxs, t)) { st.lastTickPredicted = RNG_NO_TICK; return; }
	if (!t.DidHit()) return;

	float nrm[3], collisionPoint[3];
	RngCopy(nrm, t.tr.normal);
	RngCopy(collisionPoint, t.tr.endpos);
	const float fraction = t.tr.fraction;

	st.lastCollisionTick = tick;
	RngCopy(st.collisionPoint, collisionPoint);
	RngCopy(st.collisionNormal, nrm);
	r.fixes |= RNG_COLLISION;

	if (velocity[2] > RNG_NON_JUMP_VELOCITY) return;
	if (nrm[2] < RNG_MIN_STANDABLE_ZNRM) return;

	if (s.uphill == RNG_UPHILL_NEUTRAL) {
		if (nrm[2] < 1.0f && nrm[0] * velocity[0] + nrm[1] * velocity[1] < 0.0f) {
			bool shouldDoDownhillFixInstead = false;
			if (s.downhill) {
				float newVelocity[3];
				RngClipVelocity(velocity, nrm, newVelocity);
				if (newVelocity[0] * newVelocity[0] + newVelocity[1] * newVelocity[1] > velocity[0] * velocity[0] + velocity[1] * velocity[1])
					shouldDoDownhillFixInstead = true;
			}
			if (!shouldDoDownhillFixInstead) {
				RngPreventCollision(r, io, collisionPoint, velocityTick);
				r.fixes |= RNG_FIX_UPHILL;
				// This also prevents any edge bug.
				return;
			}
		}
	}

	if (s.edge) {
		// Assumes no second collision.
		float tickEnd[3];
		const float fractionLeft = 1.0f - fraction;
		if (nrm[2] == 1.0f) {
			tickEnd[0] = collisionPoint[0] + velocityTick[0] * fractionLeft;
			tickEnd[1] = collisionPoint[1] + velocityTick[1] * fractionLeft;
			tickEnd[2] = collisionPoint[2];
		} else {
			float velocity2[3];
			RngClipVelocity(velocity, nrm, velocity2);
			// Can't land at this Z velocity anyway.
			if (velocity2[2] > RNG_NON_JUMP_VELOCITY) return;
			const float scale = r.frameTime * fractionLeft;
			for (int i = 0; i < 3; ++i) velocity2[i] *= scale;
			for (int i = 0; i < 3; ++i) tickEnd[i] = collisionPoint[i] + velocity2[i];
		}

		const float tickEndBelow[3] = { tickEnd[0], tickEnd[1], tickEnd[2] - RNG_LAND_HEIGHT };
		RngTrace t2;
		if (!RngTraceHull(r, tickEnd, tickEndBelow, mins, maxs, t2)) return;
		if (t2.DidHit()) {
			if (t2.tr.normal[2] >= RNG_MIN_STANDABLE_ZNRM) return;
			RngTrace t3;
			bool failed;
			if (RngTracePlayerBBoxForGround(r, tickEnd, tickEndBelow, mins, maxs, t3, failed)) return;
			if (failed) return;
		}

		RngPreventCollision(r, io, collisionPoint, velocityTick);
		r.fixes |= RNG_FIX_EDGE;
	}
}

static bool RngDoTriggerjumpFix(RngRuntime& r, PredRuntime* rt, PredPlayerIO& io, const float landingPoint[3],
	const float landingMins[3], const float landingMaxs[3])
{
	if (!r.settings.triggerjump) return false;
	if (!rt) return false;

	const float landingMaxsBelow[3] = { landingMaxs[0], landingMaxs[1], io.origin[2] - landingPoint[2] };

	// All found first, then touched, like rngfix's ArrayList.
	std::vector<int> triggers;
	RngFindTriggers(*rt, landingPoint, landingMins, landingMaxsBelow, triggers);

	bool didSomething = false;
	for (int i : triggers) {
		const PredEntState& st = rt->world.ents[(size_t)i];
		// g_bTouchingTrigger: MarkEntitiesAsTouching would Touch again, so skip live links.
		if (st.linked || st.removed) continue;
		// landfix.sp detours MarkEntitiesAsTouching, so while a lift lasts a booster is touched only from
		// the unlifted position, with the player's own hull.
		const PredSimEnt& e = rt->map->sim[(size_t)i];
		if (e.sjBooster && rt->world.landfixLift != 0.0f) {
			float unlifted[3];
			PredSjUnlifted(*rt, io.origin, io.mins, io.maxs, unlifted);
			if (!PredBoxInModel(*rt->map, e.modelIndex, e.xf, unlifted, io.mins, io.maxs, -1)) {
				r.fixes |= RNG_SJ_BOOSTER_SKIP;
				continue;
			}
		}
		PredLinkAndTouch(*rt, i);
		didSomething = true;
	}
	if (didSomething) r.fixes |= RNG_FIX_TRIGGERJUMP;
	return didSomething;
}

static int RngTeleportTick(const PredRuntime* rt) { return rt ? rt->world.rngTeleportTick : RNG_NO_TICK; }

static bool RngDoStairsFix(RngRuntime& r, PredRuntime* rt, PredPlayerIO& io)
{
	RngPlayerState& st = r.st;
	const int tick = r.tick;
	if (!r.settings.stairs) return false;
	if (st.lastTickPredicted != tick) return false;
	// Bad side effects on bhop, and it hardly ever helps there.
	if (!r.env.surfMap) return false;
	// Teleports win, including ones the trigger jump fix set off.
	if (RngTeleportTick(rt) == tick) return false;
	// Moving up: no position would let the player slide up.
	if (st.preCollisionVelocity[2] > 0.0f) return false;

	if (st.lastCollisionTick == tick && st.collisionNormal[2] == 0.0f) {
		float velocityDir[3];
		RngCopy(velocityDir, st.preCollisionVelocity);
		velocityDir[2] = 0.0f;
		if (RngNormalize(velocityDir, velocityDir) * r.frameTime < 1.0f) return false;

		const float* mins = io.mins;
		const float* maxs = io.maxs;
		const float stepsize = r.env.stepSize;

		float end[3], start[3];
		RngCopy(end, st.collisionPoint);
		end[2] -= stepsize;
		RngTrace t;
		if (!RngTraceHull(r, st.collisionPoint, end, mins, maxs, t)) return false;
		if (t.DidHit()) {
			if (t.tr.normal[2] < RNG_MIN_STANDABLE_ZNRM) return false;
			RngCopy(start, t.tr.endpos);

			// Standing there would touch a trigger (likely a ledge with a fail teleport): leave it.
			if (RngTriggerBlocksStairs(r, rt, io, start, mins, maxs)) return false;

			// CGameMovement::StepMove: up, over (one unit, just to find a step), and down.
			RngCopy(end, start);
			end[2] += stepsize;
			if (!RngTraceHull(r, start, end, mins, maxs, t)) return false;
			if (t.DidHit()) RngCopy(end, t.tr.endpos);

			RngCopy(start, end);
			for (int i = 0; i < 3; ++i) end[i] = start[i] + velocityDir[i];
			if (!RngTraceHull(r, start, end, mins, maxs, t)) return false;
			// Too tall to be a step, or the ceiling is too low.
			if (t.DidHit()) return false;

			RngCopy(start, end);
			end[2] -= stepsize;
			if (!RngTraceHull(r, start, end, mins, maxs, t)) return false;
			if (!t.DidHit()) return false;
			if (t.tr.normal[2] < RNG_MIN_STANDABLE_ZNRM) return false;

			RngCopy(io.origin, t.tr.endpos);
			RngSetVelocity(r, io, st.preCollisionVelocity);
			r.fixes |= RNG_FIX_STAIRS;
			return true;
		}
	}
	return false;
}

static bool RngDoInclineCollisionFixes(RngRuntime& r, PredRuntime* rt, PredPlayerIO& io, const float nrm[3])
{
	const RngSettings& s = r.settings;
	RngPlayerState& st = r.st;
	const int tick = r.tick;
	if (!s.downhill && s.uphill != RNG_UPHILL_LOSS) return false;
	if (st.lastTickPredicted != tick) return false;

	if (st.preCollisionVelocity[2] > 0.0f && s.uphill != RNG_UPHILL_LOSS) return false;

	// A collision was predicted (and not prevented): no fix, unless the old slopefix
	// logic wants it anyway (double boosts).
	if (st.lastCollisionTick == tick && !s.oldSlopefix) return false;

	if (nrm[2] == 1.0f) return false;

	float velocity[3];
	RngCopy(velocity, st.preCollisionVelocity);
	// The old slopefix ignored basevelocity.
	if (s.oldSlopefix)
		for (int i = 0; i < 3; ++i) velocity[i] -= st.lastBaseVelocity[i];

	const float dot = nrm[0] * velocity[0] + nrm[1] * velocity[1];
	if (dot >= 0 && !s.downhill) return false;

	float newVelocity[3];
	RngClipVelocity(velocity, nrm, newVelocity);
	const bool downhillFixIsBeneficial =
		newVelocity[0] * newVelocity[0] + newVelocity[1] * newVelocity[1] > velocity[0] * velocity[0] + velocity[1] * velocity[1];

	if (dot < 0 && !((downhillFixIsBeneficial && s.downhill) || s.uphill == RNG_UPHILL_LOSS)) return false;

	// On the ground, so no FinishGravity either.
	newVelocity[2] = 0.0f;

	if (s.oldSlopefix) {
		// Basevelocity baked into velocity at once (the double boost).
		const bool baseVelFlag = rt ? rt->world.baseVelFlag : (*io.flags & FL_BASEVELOCITY_CLIENT) != 0;
		if (baseVelFlag)
			for (int i = 0; i < 3; ++i) newVelocity[i] += io.baseVelocity[i];
		// TeleportEntity with a velocity clears basevelocity, and nothing restores it here.
		RngCopy(io.velocity, newVelocity);
		io.baseVelocity[0] = io.baseVelocity[1] = io.baseVelocity[2] = 0.0f;
	} else {
		RngSetVelocity(r, io, newVelocity);
	}
	r.fixes |= RNG_FIX_INCLINE;
	return true;
}

static bool RngDoTelehopFix(RngRuntime& r, PredRuntime* rt, PredPlayerIO& io)
{
	RngPlayerState& st = r.st;
	const int tick = r.tick;
	if (!r.settings.telehop) return false;
	if (st.lastTickPredicted != tick) return false;
	if (RngTeleportTick(rt) != tick) return false;
	// Teleported the tick before too: a speed-stopping hub, leave it.
	if (rt->world.rngTeleportSeq) return false;
	// The landing may since have been undone by the teleport.
	if (!(st.lastCollisionTick == tick || st.lastLandTick == tick)) return false;

	float newVelocity[3];
	RngCopy(newVelocity, st.preCollisionVelocity);
	RngFinishGravity(r, io, newVelocity);
	// rngfix's "stuck" trace only picks how the velocity is set; the result is the same.
	RngSetVelocity(r, io, newVelocity);
	r.fixes |= RNG_FIX_TELEHOP;
	return true;
}

static void RngPostThink(RngRuntime& r, PredPlayerIO& io, PredRuntime* rt)
{
	if (!r.inTick || r.postThinkDone || io.tick != r.tick) return;
	r.postThinkDone = true;
	if (!r.settings.Active() || !r.tracer) return;
	if (rt && !rt->map) rt = nullptr;
	if (rt) rt->io = &io;

	const RngSettings& s = r.settings;
	if (!r.in.alive) return;
	// After the move: a ladder grabbed this tick skips the fixes.
	if (r.moveTypePost != PRED_MOVETYPE_WALK) return;
	if (r.waterLevelPost > 1) return;

	bool landed = (*io.flags & FL_ONGROUND) != 0 && !r.st.lastOnGround;

	float origin[3], landingMins[3], landingMaxs[3], nrm[3] = {0, 0, 0}, landingPoint[3] = {0, 0, 0};
	// rngfix reads TR_GetFraction() of whatever trace ran last.
	RngTrace last;

	if (landed && (s.triggerjump || s.downhill || s.uphill == RNG_UPHILL_LOSS)) {
		RngCopy(origin, io.origin);
		RngCopy(landingMins, io.mins);
		RngCopy(landingMaxs, io.maxs);
		const float originBelow[3] = { origin[0], origin[1], origin[2] - RNG_LAND_HEIGHT };

		if (!RngTraceHull(r, origin, originBelow, landingMins, landingMaxs, last)) {
			landed = false;
		} else if (!last.DidHit()) {
			landed = false;
		} else {
			RngCopy(nrm, last.tr.normal);
			if (nrm[2] < RNG_MIN_STANDABLE_ZNRM) {
				// Too steep: find the quadrant the engine landed on.
				bool failed;
				if (RngTracePlayerBBoxForGround(r, origin, originBelow, landingMins, landingMaxs, last, failed))
					RngCopy(nrm, last.tr.normal);
				else
					landed = false;
			}
			RngCopy(landingPoint, last.tr.endpos);
		}
	}

	if (landed && last.tr.fraction > 0.0f) {
		RngDoTriggerjumpFix(r, rt, io, landingPoint, landingMins, landingMaxs);
		// A trigger just touched put us in the air (a teleport, probably).
		if ((*io.flags & FL_ONGROUND) == 0) landed = false;
	}

	// Stair sliding changes the tick too much to also do incline fixes.
	if (RngDoStairsFix(r, rt, io)) return;

	if (landed) RngDoInclineCollisionFixes(r, rt, io, nrm);

	RngDoTelehopFix(r, rt, io);
}

static void RngPostThinkHook(void* user, PredRuntime& rt, PredPlayerIO& io)
{
	RngPostThink(*static_cast<RngRuntime*>(user), io, &rt);
}

static void RngPreTick(RngRuntime& r, PredPlayerIO& io, const RngMoveInput& in, PredRuntime* rt)
{
	RngCommandStart(r, io.tick, PredSnapshotKey(io));
	r.tick = io.tick;
	r.inTick = true;
	r.postThinkDone = false;
	r.fixes = 0;
	r.in = in;
	r.waterLevelPost = in.waterLevel;
	r.moveTypePost = io.moveType;
	r.preOnGround = (*io.flags & FL_ONGROUND) != 0;
	r.frameTime = io.interval * in.laggedMovement;
	if (rt) { rt->postThinkHook = &RngPostThinkHook; rt->postThinkUser = &r; }

	if (!r.settings.Active() || !r.tracer) return;
	const RngSettings& s = r.settings;
	// Triggerjump doesn't need the pre-tick prediction.
	if (!s.uphill && !s.edge && !s.stairs && !s.telehop && !s.downhill) return;
	RngRunPreTickChecks(r, io);
}

// waterLevel and moveType are after the move, for PostThink; -1 keeps the pre-move value.
// io.moveType stays the pre-move one, which is what trigger_sim uses.
static void RngPostMove(RngRuntime& r, PredPlayerIO& io, int waterLevel = -1, int moveType = -1)
{
	if (!r.inTick || io.tick != r.tick) return;
	if (waterLevel >= 0) r.waterLevelPost = waterLevel;
	if (moveType >= 0) r.moveTypePost = moveType;
	if (!r.preOnGround && (*io.flags & FL_ONGROUND) != 0) {
		r.st.lastLandTick = r.tick;
		r.fixes |= RNG_LANDED;
	}
}

// io.mins/maxs must be the hull the command started with: rngfix reads m_vecMins/m_vecMaxs
// in an SDKHook_PostThink pre-hook, before CBasePlayer::PostThink updates the hull.
static void RngEndTick(RngRuntime& r, PredPlayerIO& io, PredRuntime* rt)
{
	if (!r.inTick || io.tick != r.tick) return;
	RngPostThink(r, io, rt);
	RngCommandEnd(r, PredSnapshotKey(io));
	r.inTick = false;
}

static void RngDetach(PredRuntime& rt)
{
	rt.postThinkHook = nullptr;
	rt.postThinkUser = nullptr;
}
