#pragma once

static const float PRED_MPBHOPS_DELAY    = 0.06f;
static const float PRED_MPBHOPS_COOLDOWN = 1.10f;

// IsBoxIntersectingRay, with the box already grown by the ray's extents.
static bool PredSweptBoxHits(const float lo[3], const float hi[3], const float start[3], const float delta[3], float* entry)
{
	float tmin = -1e30f, tmax = 1e30f;
	for (int c = 0; c < 3; ++c) {
		if (fabsf(delta[c]) < 1e-8f) {
			if (start[c] < lo[c] || start[c] > hi[c]) return false;
			continue;
		}
		const float inv = 1.0f / delta[c];
		float t1 = (lo[c] - start[c]) * inv, t2 = (hi[c] - start[c]) * inv;
		if (t1 > t2) std::swap(t1, t2);
		tmin = fmaxf(tmin, t1);
		tmax = fminf(tmax, t2);
		if (tmin > tmax || tmax < 0.0f || tmin > 1.0f) return false;
	}
	*entry = tmin;
	return true;
}

// Earliest hit wins, since the partition's order isn't known.
static int PredMpbhopsFindTeleport(const PredMap& map, const BspModel& m, const float pos1[3], const float pos2[3])
{
	float ext[3], start[3], delta[3];
	for (int c = 0; c < 3; ++c) {
		// m_vecMins/m_vecMaxs: the engine loads model bounds a unit wider each way.
		const float mins = m.mins[c] - 1.0f, maxs = m.maxs[c] + 1.0f;
		ext[c] = (maxs - mins) * 0.5f;
		start[c] = pos1[c] + (mins + maxs) * 0.5f;
		delta[c] = pos2[c] - pos1[c];
	}
	int best = -1;
	float bestT = 0.0f;
	for (int i = 0; i < (int)map.sim.size(); ++i) {
		const PredSimEnt& e = map.sim[i];
		// A disabled trigger has no FSOLID_TRIGGER, so it isn't in the trigger partition.
		if (e.modelIndex < 0 || (size_t)e.modelIndex >= map.models.size() || e.initialDisabled
				|| strcmp(map.entities[e.entity].Get("classname"), "trigger_teleport") != 0)
			continue;
		float lo[3], hi[3], t;
		PredTriggerWorldBounds(map, e, 1.0f, lo, hi);
		for (int c = 0; c < 3; ++c) { lo[c] -= ext[c]; hi[c] += ext[c]; }
		if (PredSweptBoxHits(lo, hi, start, delta, &t) && (best < 0 || t < bestT)) { best = i; bestT = t; }
	}
	return best;
}

static void PredBuildMpbhops(PredMap& map)
{
	map.mpbhopsDoors.clear();
	for (const PredMapEntity& ent : map.entities) {
		const char* cls = ent.Get("classname");
		const bool button = !strcmp(cls, "func_button");
		if (!button && strcmp(cls, "func_door") != 0) continue;
		const int sf = atoi(ent.Get("spawnflags", "0"));
		if (!(sf & (button ? 0x100 : 0x400))) continue;
		// Non-solid to players or passable: no move ever stands on or touches it.
		if (!button && (sf & 0xC)) continue;
		const char* model = ent.Get("model");
		const int mi = model[0] == '*' ? atoi(model + 1) : -1;
		if (mi <= 0 || (size_t)mi >= map.models.size()) continue;
		const BspModel& m = map.models[mi];

		float pos1[3], angles[3], moveAngles[3], dir[3];
		PredParseVec3(ent.Get("origin", "0 0 0"), pos1);
		PredParseVec3(ent.Get("angles", "0 0 0"), angles);
		PredParseVec3(ent.Get("movedir", "0 0 0"), moveAngles);
		PredAngleVectorsForward(moveAngles, dir);
		float speed = (float)atof(ent.Get("speed", "0"));
		float lip = (float)atof(ent.Get("lip", "0"));
		float wait = (float)atof(ent.Get("wait", "0"));
		if (speed == 0.0f) speed = button ? 40.0f : 100.0f;
		if (button && wait == 0.0f) wait = 1.0f;
		if (button && lip == 0.0f) lip = 4.0f;
		// Spawn takes back the 2 units the engine added to the model's size.
		float dist = 0.0f;
		for (int c = 0; c < 3; ++c) dist += fabsf(dir[c] * (((m.maxs[c] + 1.0f) - (m.mins[c] - 1.0f)) - 2.0f));
		dist -= lip;
		float pos2[3];
		for (int c = 0; c < 3; ++c) pos2[c] = pos1[c] + dir[c] * dist;
		if (button) {
			const float d[3] = { pos2[0] - pos1[0], pos2[1] - pos1[1], pos2[2] - pos1[2] };
			if (sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) < 1.0f || (sf & 0x1))
				memcpy(pos2, pos1, sizeof pos2);
		}

		PredMpbhopsDoor door;
		if (pos1[2] < pos2[2]) {
			if (!(wait > 0.0f)) continue;
			door.boost = true;
		} else if (pos1[2] > pos2[2]) {
			door.tele = PredMpbhopsFindTeleport(map, m, pos1, pos2);
			if (door.tele < 0) continue;
		} else {
			continue;
		}
		door.modelIndex = mi;
		door.speed = speed;
		// spawnpos 1 or the old start-open flag moved the door to position2 before it was locked.
		const bool open = !button && (atoi(ent.Get("spawnpos", "0")) == 1 || (sf & 0x1));
		PredAngleMatrix(angles, open ? pos2 : pos1, door.xf);
		map.mpbhopsDoors.push_back(door);
	}
}

// CategorizePosition's ground reaches 2 units down. Goes by the door's brushes because the client
// can't tell which entity index is which door.
static int PredMpbhopsDoorUnder(const PredMap& map, const float origin[3], bool onEntity)
{
	if (!onEntity) return -1;
	static const float mins[3] = { -16.0f, -16.0f, -2.0f }, maxs[3] = { 16.0f, 16.0f, 0.0f };
	const int maskPlayerSolid = 0x1 | 0x2 | 0x8 | 0x4000 | 0x10000 | 0x2000000;
	for (size_t d = 0; d < map.mpbhopsDoors.size(); ++d) {
		const PredMpbhopsDoor& door = map.mpbhopsDoors[d];
		if (PredBoxInModel(map, door.modelIndex, door.xf, origin, mins, maxs, maskPlayerSolid)) return (int)d;
	}
	return -1;
}

// SetGroundEntity puts a non-world ground in the touch list every command and ProcessImpacts
// calls its Touch. Side and underside hits aren't modelled.
static int PredMpbhopsTouchedDoor(const PredRuntime& rt, const PredPlayerIO& io)
{
	if (!rt.mpbhops || rt.map->mpbhopsDoors.empty() || !io.solid) return -1;
	const bool onEntity = io.groundEntity ? *io.groundEntity != 0xFFFFFFFFu && (*io.groundEntity & 0xFFF) != 0
		: (*io.flags & FL_ONGROUND) != 0;
	return PredMpbhopsDoorUnder(*rt.map, io.origin, onEntity);
}

// Block_Touch_Teleport, after the trigger pass like ProcessImpacts. Time is the command's curtime,
// not the plugin's own clock.
static void PredMpbhopsTouch(PredRuntime& rt, PredPlayerIO& io, int door)
{
	if (door < 0) return;
	const PredMap& map = *rt.map;
	const int tele = map.mpbhopsDoors[door].tele;
	if (tele < 0) return;
	PredWorld& w = rt.world;
	const float time = io.curtime;
	const float diff = time - w.mpbhopsPunish;
	if (w.mpbhopsBlock != door || diff > PRED_MPBHOPS_COOLDOWN) {
		w.mpbhopsBlock = door;
		w.mpbhopsPunish = time + PRED_MPBHOPS_DELAY;
		return;
	}
	if (!(diff > PRED_MPBHOPS_DELAY) || !(time > PRED_MPBHOPS_COOLDOWN + PRED_MPBHOPS_DELAY)) return;
	// A killed trigger's reference doesn't resolve, and the block stays set.
	if (w.ents[tele].removed) return;
	w.mpbhopsBlock = -1;
	// A trigger the sim skips (a moving parent) is left to the server.
	if (map.sim[tele].cls != PC_TELEPORT) return;
	// A plain Touch call: the filters, no StartTouch or outputs, and no m_bDisabled check.
	const int teleports = rt.teleports;
	PredTouch(rt, tele);
	// Nothing stamps the trigger's own link after this Touch, so it goes stale with the rest.
	if (rt.teleports != teleports && w.ents[tele].linked) w.ents[tele].stale = 1;
}

// PlayerMove's CategorizePosition only runs under sv_optimizedmovement after game code moved the player.
static void PredMpbhopsJumpGround(PredRuntime& rt, const float origin[3], bool onEntity, bool gameMoved)
{
	if (rt.map && rt.mpbhops && gameMoved) rt.mpbhopsStartDoor = PredMpbhopsDoorUnder(*rt.map, origin, onEntity);
}

static int PredMpbhopsJumpDoor(const PredRuntime& rt, const float origin[3], bool onEntity)
{
	if (!rt.map || !rt.mpbhops) return -1;
	const int d = PredMpbhopsDoorUnder(*rt.map, origin, onEntity);
	return d >= 0 && rt.map->mpbhopsDoors[d].boost ? d : -1;
}

static void PredMpbhopsJump(const PredRuntime& rt, int door, float baseVelocity[3])
{
	if (door >= 0) baseVelocity[2] += rt.map->mpbhopsDoors[door].speed;
}
