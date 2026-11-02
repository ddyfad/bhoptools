#pragma once

#include "rngfix_sim.h"
#include "rngfix_trace_bsp.h"
#include "engine_trace_layout.h"

struct RngPredict {
	RngRuntime            rng;
	RngBspTracer          bsp;
	etrace::EngineTracer* engine = nullptr;
	bool                  running = false;
};

// Let go of the old BSP before the trigger sim frees it.
static void RngPredictNewLevel(RngPredict& p, const char* map)
{
	RngReset(p.rng);
	RngSetMapName(p.rng.env, map);
	p.bsp.SetMap(nullptr);
	if (p.engine) p.engine->Reset();
}

static void RngPredictSetBsp(RngPredict& p, const PredMap* map) { p.bsp.SetMap(map); }

static RngTracer* RngPredictTracer(RngPredict& p, const float origin[3], bool onGround, bool firstRun)
{
	RngTracer* t = nullptr;
	if (p.engine) {
		if (firstRun) p.engine->Validate(origin, onGround);
		if (p.engine->Ready()) t = p.engine;
	}
	if (!t && p.bsp.Ready()) t = &p.bsp;
	return t;
}

static void RngPredictStop(RngPredict& p, PredRuntime& trig)
{
	if (trig.postThinkHook) RngDetach(trig);
	p.running = false;
}

static bool RngPredictBegin(RngPredict& p, const RngSettings& s, const RngEnv& env,
	const float origin[3], bool onGround, bool firstRun, PredRuntime& trig)
{
	RngTracer* t = nullptr;
	if (s.Active()) t = RngPredictTracer(p, origin, onGround, firstRun);

	if (!t) {
		RngPredictStop(p, trig);
		return false;
	}
	// History from before a pause would be stale.
	if (!p.running) RngReset(p.rng);
	p.running = true;

	p.rng.settings = s;
	p.rng.tracer = t;
	const bool surf = p.rng.env.surfMap;  // set per level, not per command
	p.rng.env = env;
	p.rng.env.surfMap = surf;
	return true;
}

static void RngPredictPreTick(RngPredict& p, PredPlayerIO& io, const RngMoveInput& in, PredRuntime* rt)
{
	RngMoveInput shavit = in;
	RngShavitAutobhop(shavit, p.rng.settings.autobhop, io.moveType);
	RngPreTick(p.rng, io, shavit, rt);
}

// waterLevel, moveType: after the move, -1 if unknown.
static void RngPredictPostMove(RngPredict& p, PredPlayerIO& io, int waterLevel, int moveType)
{
	RngPostMove(p.rng, io, waterLevel, moveType);
}

static void RngPredictEndTick(RngPredict& p, PredPlayerIO& io, PredRuntime* rt)
{
	RngEndTick(p.rng, io, rt);
}
