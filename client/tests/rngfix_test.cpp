#define _USE_MATH_DEFINES
#include "rngfix_sim.h"
#include "rngfix_trace_bsp.h"
#include "engine_trace_layout.h"
#include "rngfix_predict.h"
#include "menu_hash.h"
#include "noclip_predict.h"

#include <cstdio>
#include <cmath>

static int g_fail = 0, g_pass = 0;

#define CHECK(cond) do { if (cond) ++g_pass; else { ++g_fail; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_NEAR(a, b, eps) do { const double a_ = (a), b_ = (b); if (fabs(a_ - b_) <= (eps)) ++g_pass; else { ++g_fail; \
	printf("FAIL %s:%d: %s = %.6f, expected %.6f\n", __FILE__, __LINE__, #a, a_, b_); } } while (0)

static const float S2 = 0.70710678f;

static BspPlane Plane(float nx, float ny, float nz, float dist)
{
	BspPlane p;
	p.normal[0] = nx; p.normal[1] = ny; p.normal[2] = nz;
	p.dist = dist;
	p.type = (nx == 1.0f || nx == -1.0f) ? 0 : (ny == 1.0f || ny == -1.0f) ? 1 : (nz == 1.0f || nz == -1.0f) ? 2 : 3;
	return p;
}

static void AddBrush(PredMap& map, const std::vector<BspPlane>& planes, int contents = 1)
{
	PredBrushSpan b = {};
	b.firstPlane = (int)map.brushPlanes.size();
	b.planeCount = (int)planes.size();
	b.contents = contents;
	for (const BspPlane& p : planes) map.brushPlanes.push_back(p);
	map.brushes.push_back(b);
}

static std::vector<BspPlane> BoxPlanes(float x0, float y0, float z0, float x1, float y1, float z1)
{
	return { Plane(1, 0, 0, x1), Plane(-1, 0, 0, -x0), Plane(0, 1, 0, y1), Plane(0, -1, 0, -y0),
		Plane(0, 0, 1, z1), Plane(0, 0, -1, -z0) };
}

static std::vector<BspPlane> RampPlanes(float x0, float x1, float y0, float y1, float degrees)
{
	const float rad = degrees * (float)M_PI / 180.0f;
	const float sn = sinf(rad), cs = cosf(rad);
	const float top = (x1 - x0) * tanf(rad);
	std::vector<BspPlane> p = BoxPlanes(x0, y0, 0.0f, x1, y1, top);
	p.push_back(Plane(-sn, 0.0f, cs, -sn * x0));
	return p;
}

// World (model 0), plus models 1 and 2: 1-unit teleport slabs on the floor.
static const float TRIG_X0 = -200, TRIG_X1 = -100, TRIG_Y0 = -600, TRIG_Y1 = -500;
static const float TRIG2_Y0 = -400, TRIG2_Y1 = -300;
static const float TELE_DEST[3] = { 600.0f, -600.0f, 64.0f };

static PredMap* BuildMap()
{
	PredMap* m = new PredMap();
	AddBrush(*m, BoxPlanes(-1024, -1024, -64, 1024, 1024, 0));
	AddBrush(*m, RampPlanes(256, 512, -256, 256, 45.0f));
	AddBrush(*m, RampPlanes(256, 512, 512, 768, 30.0f));
	AddBrush(*m, BoxPlanes(-512, -256, 0, -256, 256, 16));
	AddBrush(*m, BoxPlanes(-1024, 512, 200, -768, 768, 208));
	AddBrush(*m, BoxPlanes(900, -1024, 0, 1024, 1024, 512));
	AddBrush(*m, BoxPlanes(-900, -900, 0, -800, -800, 100), 0x20); // CONTENTS_WATER: not player solid
	const int worldCount = (int)m->brushes.size();

	AddBrush(*m, BoxPlanes(TRIG_X0, TRIG_Y0, 0, TRIG_X1, TRIG_Y1, 1));
	AddBrush(*m, BoxPlanes(TRIG_X0, TRIG2_Y0, 0, TRIG_X1, TRIG2_Y1, 1));
	BspModel world = {}, trig = {};
	for (int c = 0; c < 3; ++c) { world.mins[c] = -1024; world.maxs[c] = 1024; }
	trig.mins[0] = TRIG_X0; trig.mins[1] = TRIG_Y0; trig.mins[2] = 0;
	trig.maxs[0] = TRIG_X1; trig.maxs[1] = TRIG_Y1; trig.maxs[2] = 1;
	BspModel trig2 = trig;
	trig2.mins[1] = TRIG2_Y0; trig2.maxs[1] = TRIG2_Y1;
	m->models = { world, trig, trig2 };
	m->modelBrushes = { { 0, worldCount }, { worldCount, 1 }, { worldCount + 1, 1 } };

	PredSimEnt tp;
	tp.cls = PC_TELEPORT;
	tp.modelIndex = 1;
	const float zero[3] = { 0, 0, 0 };
	PredAngleMatrix(zero, zero, tp.xf);
	tp.spawnflags = 1; // clients
	tp.destValid = true;
	memcpy(tp.destOrigin, TELE_DEST, sizeof TELE_DEST);
	m->sim.push_back(tp);
	tp.modelIndex = 2;
	m->sim.push_back(tp);
	PredBuildGrid(*m);
	m->loaded = true;
	return m;
}

struct Player {
	float origin[3] = {0, 0, 0}, velocity[3] = {0, 0, 0}, baseVel[3] = {0, 0, 0};
	float gravity = 1.0f;
	int flags = FL_CLIENT_BIT;
	uint8_t moveFlags = 1;
	PredPlayerIO io;

	PredPlayerIO& IO(int tick)
	{
		io.origin = origin;
		io.velocity = velocity;
		io.baseVelocity = baseVel;
		io.gravity = &gravity;
		io.flags = &flags;
		io.moveFlags = &moveFlags;
		io.tick = tick;
		io.moveType = PRED_MOVETYPE_WALK;
		io.solid = true;
		io.interval = 1.0f / 64.0f;
		io.curtime = tick * io.interval;
		PredSetHull(io, (flags & FL_DUCKING) != 0);
		return io;
	}
};

static const float DT = 1.0f / 64.0f;

struct Fixture {
	PredMap* map;
	RngBspTracer tracer;
	RngRuntime rng;
	PredRuntime rt;

	Fixture() : map(BuildMap()), tracer(map)
	{
		rng.settings.known = true;
		rng.tracer = &tracer;
		rng.env.airAccelerate = 150.0f;
		PredAdopt(rt, map);
	}
	~Fixture() { PredReset(rt); }
};

// m_MoveType after the original ProcessMovement, as the hook reads it (-1: unchanged).
static int g_postMoveType = -1;

template <class Move>
static unsigned RunTick(Fixture& f, Player& p, int tick, const RngMoveInput& in, Move move, bool withTriggers = true)
{
	PredPlayerIO& io = p.IO(tick);
	PredRuntime* rt = withTriggers ? &f.rt : nullptr;
	if (rt) PredPreMove(*rt, io);
	RngPreTick(f.rng, io, in, rt);
	move(p);
	RngPostMove(f.rng, io, -1, g_postMoveType);
	if (rt) PredPostMove(*rt, io);
	RngEndTick(f.rng, io, rt);
	return f.rng.fixes;
}

static void NoMove(Player&) {}

static const float MINS[3] = { -16, -16, 0 }, MAXS[3] = { 16, 16, 62 };

static void TestTracer()
{
	PredMap* map = BuildMap();
	RngBspTracer t(map);
	RngTraceResult r;

	// Straight down onto the floor: centre 131 -> -69 against z <= 31 (plane pushed up by 31).
	{
		const float s[3] = { 0, 0, 100 }, e[3] = { 0, 0, -100 };
		CHECK(t.TraceHull(s, e, MINS, MAXS, r));
		CHECK_NEAR(r.fraction, (100.0 - 0.03125) / 200.0, 1e-6);
		CHECK_NEAR(r.normal[2], 1.0, 0);
		CHECK_NEAR(r.endpos[2], 0.03125, 1e-4);
		CHECK(!r.startSolid && !r.allSolid);
	}
	// Into the wall: -x >= -900 pushed out to -884.
	{
		const float s[3] = { 800, 0, 10 }, e[3] = { 1000, 0, 10 };
		CHECK(t.TraceHull(s, e, MINS, MAXS, r));
		CHECK_NEAR(r.fraction, (84.0 - 0.03125) / 200.0, 1e-6);
		CHECK_NEAR(r.normal[0], -1.0, 0);
		CHECK_NEAR(r.endpos[0], 884.0 - 0.03125, 1e-3);
	}
	{
		const float s[3] = { 0, 0, 10 }, e[3] = { 100, 50, 60 };
		CHECK(t.TraceHull(s, e, MINS, MAXS, r));
		CHECK(r.fraction == 1.0f && !r.startSolid);
		CHECK_NEAR(r.endpos[0], 100, 1e-4);
		CHECK_NEAR(r.endpos[1], 50, 1e-4);
		CHECK_NEAR(r.endpos[2], 60, 1e-4);
	}
	{
		const float s[3] = { -400, 0, 5 };
		CHECK(t.TraceHull(s, s, MINS, MAXS, r));
		CHECK(r.startSolid && r.allSolid);
		CHECK(r.fraction == 0.0f);
	}
	{
		const float s[3] = { -400, 0, 5 }, e[3] = { -400, 0, 50 };
		CHECK(t.TraceHull(s, e, MINS, MAXS, r));
		CHECK(r.startSolid && !r.allSolid);
		CHECK(r.fraction == 1.0f);
	}
	// Down onto the 45 degree ramp: the slope plane pushed out by 16*s + 31*s.
	{
		const float s[3] = { 384, 0, 300 }, e[3] = { 384, 0, 0 };
		CHECK(t.TraceHull(s, e, MINS, MAXS, r));
		const double sn = S2, dist = -sn * 256 + sn * 16 + sn * 31;
		const double d1 = -sn * 384 + sn * 331 - dist, d2 = -sn * 384 + sn * 31 - dist;
		CHECK_NEAR(r.fraction, (d1 - 0.03125) / (d1 - d2), 1e-5);
		CHECK_NEAR(r.normal[0], -S2, 1e-6);
		CHECK_NEAR(r.normal[2], S2, 1e-6);
	}
	{
		const float s[3] = { 384, 640, 300 }, e[3] = { 384, 640, 0 };
		CHECK(t.TraceHull(s, e, MINS, MAXS, r));
		CHECK_NEAR(r.normal[2], cos(M_PI / 6), 1e-5);
		const double sn = 0.5, cs = cos(M_PI / 6);
		const double dist = -sn * 256 + sn * 16 + cs * 31;
		const double d1 = -sn * 384 + cs * 331 - dist, d2 = -sn * 384 + cs * 31 - dist;
		CHECK_NEAR(r.fraction, (d1 - 0.03125) / (d1 - d2), 1e-5);
	}
	// Water isn't MASK_PLAYERSOLID.
	{
		const float s[3] = { -850, -850, 50 };
		CHECK(t.TraceHull(s, s, MINS, MAXS, r));
		CHECK(!r.startSolid && r.fraction == 1.0f);
	}
	// A different hull size (ducked) moves the floor plane by its own extents.
	{
		const float mx[3] = { 16, 16, 45 };
		const float s[3] = { 0, 0, 10 }, e[3] = { 0, 0, -10 };
		CHECK(t.TraceHull(s, e, MINS, mx, r));
		CHECK_NEAR(r.fraction, (10.0 - 0.03125) / 20.0, 1e-6);
	}
	{
		PredMap empty;
		RngBspTracer none(&empty);
		const float s[3] = { 0, 0, 0 };
		CHECK(!none.TraceHull(s, s, MINS, MAXS, r));
	}
	delete map;
}

static RngMoveInput Input()
{
	RngMoveInput in;
	in.maxSpeed = 250.0f;
	return in;
}

// Pre-collision velocity (-300, 0, -406.25) clipped by (-s, 0, s) gives (-353.125, 0, -353.125).
static void DownhillScenario(Fixture& f, Player& p, int tick)
{
	// Hull's +x edge at x=416, where the slope is at z=160; 3 units above it.
	p.origin[0] = 400; p.origin[1] = 0; p.origin[2] = 163;
	p.velocity[0] = -300; p.velocity[1] = 0; p.velocity[2] = -400;
	p.flags = FL_CLIENT_BIT;
	RunTick(f, p, tick, Input(), [](Player& q) {
		// What the engine does: moves, finds ground within 2 units, zeroes Z velocity.
		q.origin[0] += -300 * DT;
		q.origin[2] += -406.25f * DT;
		q.velocity[2] = 0;
		q.flags |= FL_ONGROUND;
	});
}

static void TestDownhill()
{
	{
		Fixture f;
		Player p;
		RunTick(f, p, 99, Input(), NoMove);
		DownhillScenario(f, p, 100);
		CHECK(f.rng.fixes & RNG_PREDICTED);
		CHECK(!(f.rng.fixes & RNG_COLLISION));
		CHECK(f.rng.fixes & RNG_FIX_INCLINE);
		CHECK_NEAR(p.velocity[0], -353.125, 1e-2);
		CHECK_NEAR(p.velocity[1], 0, 1e-4);
		CHECK_NEAR(p.velocity[2], 0, 0);
	}
	{
		Fixture f;
		f.rng.settings.downhill = 0;
		Player p;
		RunTick(f, p, 99, Input(), NoMove);
		DownhillScenario(f, p, 100);
		CHECK(!(f.rng.fixes & RNG_FIX_INCLINE));
		CHECK_NEAR(p.velocity[0], -300, 0);
	}
	{
		Fixture f;
		f.rng.settings.known = false;
		Player p;
		RunTick(f, p, 99, Input(), NoMove);
		DownhillScenario(f, p, 100);
		CHECK(f.rng.fixes == 0 || f.rng.fixes == RNG_LANDED);
		CHECK_NEAR(p.velocity[0], -300, 0);
	}
	// Grabbed a ladder during the move: rngfix's PostThink sees MOVETYPE_LADDER and skips.
	{
		Fixture f;
		Player p;
		RunTick(f, p, 99, Input(), NoMove);
		g_postMoveType = 9; // MOVETYPE_LADDER
		DownhillScenario(f, p, 100);
		g_postMoveType = -1;
		CHECK(f.rng.fixes & RNG_PREDICTED);
		CHECK(!(f.rng.fixes & RNG_FIX_INCLINE));
		CHECK_NEAR(p.velocity[0], -300, 0);
	}
	// The old slopefix logic bakes basevelocity in and clears it.
	{
		Fixture f;
		f.rng.settings.oldSlopefix = 1;
		Player p;
		RunTick(f, p, 99, Input(), NoMove);
		DownhillScenario(f, p, 100);
		CHECK(f.rng.fixes & RNG_FIX_INCLINE);
		CHECK_NEAR(p.velocity[0], -353.125, 1e-2);
	}
}

static void UphillScenario(Fixture& f, Player& p, int tick, float gap, void (*move)(Player&))
{
	// Hull's +x edge at x=316, slope z=60 there.
	p.origin[0] = 300; p.origin[1] = 0; p.origin[2] = 60 + gap;
	p.velocity[0] = 300; p.velocity[1] = 0; p.velocity[2] = -100;
	p.flags = FL_CLIENT_BIT;
	RunTick(f, p, tick, Input(), move);
}

static void TestUphill()
{
	// uphill 1: the origin is moved back so the tick ends 0.1 above the collision point.
	{
		Fixture f;
		Player p;
		RunTick(f, p, 99, Input(), NoMove);
		UphillScenario(f, p, 100, 2.0f, [](Player&) {});
		float before[3];
		memcpy(before, p.origin, sizeof before);
		CHECK(f.rng.fixes & RNG_COLLISION);
		CHECK(f.rng.fixes & RNG_FIX_UPHILL);

		const float o[3] = { 300, 0, 62 };
		const float vt[3] = { 300 * DT, 0, (-100 - 6.25f) * DT };
		const float e[3] = { o[0] + vt[0], o[1], o[2] + vt[2] };
		RngTraceResult tr;
		f.tracer.TraceHull(o, e, MINS, MAXS, tr);
		CHECK(tr.fraction < 1.0f);
		CHECK_NEAR(before[0], tr.endpos[0] - vt[0], 1e-4);
		CHECK_NEAR(before[2], tr.endpos[2] - vt[2] + 0.1f, 1e-4);
		// From there the tick's movement no longer reaches the ramp.
		const float e2[3] = { before[0] + vt[0], 0, before[2] + vt[2] };
		f.tracer.TraceHull(before, e2, MINS, MAXS, tr);
		CHECK(tr.fraction == 1.0f);
	}
	{
		Fixture f;
		f.rng.settings.uphill = 0;
		Player p;
		RunTick(f, p, 99, Input(), NoMove);
		UphillScenario(f, p, 100, 2.0f, [](Player&) {});
		CHECK(f.rng.fixes & RNG_COLLISION);
		CHECK(!(f.rng.fixes & (RNG_FIX_UPHILL | RNG_FIX_EDGE)));
		CHECK_NEAR(p.origin[0], 300, 0);
		CHECK_NEAR(p.origin[2], 62, 0);
	}
	// uphill -1: a landing without collision gets the collision's loss:
	// (300, 0, -106.25) clipped by (-s, 0, s) = (96.875, 0, 96.875), Z zeroed.
	{
		Fixture f;
		f.rng.settings.uphill = -1;
		Player p;
		RunTick(f, p, 99, Input(), NoMove);
		UphillScenario(f, p, 100, 7.0f, [](Player& q) {
			q.origin[0] += 300 * DT;
			q.origin[2] += -106.25f * DT;
			q.velocity[2] = 0;
			q.flags |= FL_ONGROUND;
		});
		CHECK(!(f.rng.fixes & RNG_COLLISION));
		CHECK(f.rng.fixes & RNG_FIX_INCLINE);
		CHECK_NEAR(p.velocity[0], 96.875, 1e-2);
		CHECK_NEAR(p.velocity[2], 0, 0);
	}
	// uphill 1, same landing: no incline fix (it isn't beneficial).
	{
		Fixture f;
		Player p;
		RunTick(f, p, 99, Input(), NoMove);
		UphillScenario(f, p, 100, 7.0f, [](Player& q) {
			q.origin[0] += 300 * DT;
			q.origin[2] += -106.25f * DT;
			q.velocity[2] = 0;
			q.flags |= FL_ONGROUND;
		});
		CHECK(!(f.rng.fixes & RNG_FIX_INCLINE));
		CHECK_NEAR(p.velocity[0], 300, 0);
	}
}

static void EdgeScenario(Fixture& f, Player& p, int tick)
{
	p.origin[0] = -760; p.origin[1] = 640; p.origin[2] = 212;
	p.velocity[0] = 1000; p.velocity[1] = 0; p.velocity[2] = -500;
	p.flags = FL_CLIENT_BIT;
	RunTick(f, p, tick, Input(), NoMove);
}

static void TestEdge()
{
	{
		Fixture f;
		Player p;
		RunTick(f, p, 99, Input(), NoMove);
		EdgeScenario(f, p, 100);
		CHECK(f.rng.fixes & RNG_COLLISION);
		CHECK(f.rng.fixes & RNG_FIX_EDGE);
		// End of tick (origin + tick movement) is 0.1 above the ledge's top.
		const float vz = (-500 - 6.25f) * DT;
		CHECK_NEAR(p.origin[2] + vz, 208.03125 + 0.1, 1e-3);
	}
	{
		Fixture f;
		f.rng.settings.edge = 0;
		Player p;
		RunTick(f, p, 99, Input(), NoMove);
		EdgeScenario(f, p, 100);
		CHECK(!(f.rng.fixes & RNG_FIX_EDGE));
		CHECK_NEAR(p.origin[2], 212, 0);
	}
	{
		Fixture f;
		Player p;
		RunTick(f, p, 99, Input(), NoMove);
		p.origin[0] = -900; p.origin[1] = 640; p.origin[2] = 212;
		p.velocity[0] = 300; p.velocity[1] = 0; p.velocity[2] = -500;
		p.flags = FL_CLIENT_BIT;
		RunTick(f, p, 100, Input(), NoMove);
		CHECK(f.rng.fixes & RNG_COLLISION);
		CHECK(!(f.rng.fixes & RNG_FIX_EDGE));
	}
}

// Trigger jump + telehop: lands 1.5 above the floor over a 1-unit teleport slab the
// normal touch pass misses.
static void TriggerjumpScenario(Fixture& f, Player& p, int tick)
{
	p.origin[0] = -150; p.origin[1] = -550; p.origin[2] = 1.5f + 400 * DT + 6.25f * DT;
	p.velocity[0] = 400; p.velocity[1] = 0; p.velocity[2] = -400;
	p.flags = FL_CLIENT_BIT;
	RunTick(f, p, tick, Input(), [](Player& q) {
		q.origin[0] += 400 * DT;
		q.origin[2] = 1.5f;
		q.velocity[2] = 0;
		q.flags |= FL_ONGROUND;
	});
}

static void TestTriggerjumpTelehop()
{
	{
		Fixture f;
		Player p;
		RunTick(f, p, 99, Input(), NoMove);
		const int tp0 = f.rt.teleports;
		TriggerjumpScenario(f, p, 100);
		CHECK(f.rng.fixes & RNG_FIX_TRIGGERJUMP);
		CHECK(f.rt.teleports == tp0 + 1);
		CHECK_NEAR(p.origin[0], TELE_DEST[0], 0);
		CHECK_NEAR(p.origin[2], TELE_DEST[2], 0);
		CHECK(!(p.flags & FL_ONGROUND));
		CHECK(f.rng.fixes & RNG_FIX_TELEHOP);
		// (400, 0, -400) - half tick gravity (6.25), + second half (6.25).
		CHECK_NEAR(p.velocity[0], 400, 1e-4);
		CHECK_NEAR(p.velocity[2], -412.5, 1e-4);
	}
	{
		Fixture f;
		f.rng.settings.triggerjump = 0;
		Player p;
		RunTick(f, p, 99, Input(), NoMove);
		const int tp0 = f.rt.teleports;
		TriggerjumpScenario(f, p, 100);
		CHECK(f.rt.teleports == tp0);
		CHECK_NEAR(p.origin[2], 1.5, 0);
		CHECK(!(f.rng.fixes & (RNG_FIX_TRIGGERJUMP | RNG_FIX_TELEHOP)));
	}
	{
		Fixture f;
		f.rng.settings.telehop = 0;
		Player p;
		RunTick(f, p, 99, Input(), NoMove);
		TriggerjumpScenario(f, p, 100);
		CHECK(f.rng.fixes & RNG_FIX_TRIGGERJUMP);
		CHECK(!(f.rng.fixes & RNG_FIX_TELEHOP));
		CHECK_NEAR(p.velocity[2], 0, 0);
	}
	// Teleported the tick before as well: no telehop (a speed-stopping hub).
	{
		Fixture f;
		Player p;
		RunTick(f, p, 99, Input(), NoMove);
		// Standing in the other slab: the touch pass teleports.
		p.origin[0] = -150; p.origin[1] = -350; p.origin[2] = 0.5f;
		p.velocity[0] = 0; p.velocity[1] = 0; p.velocity[2] = 0;
		p.flags = FL_CLIENT_BIT;
		RunTick(f, p, 100, Input(), NoMove);
		CHECK(f.rt.world.rngTeleportTick == 100);
		TriggerjumpScenario(f, p, 101);
		CHECK(f.rng.fixes & RNG_FIX_TRIGGERJUMP);
		CHECK(f.rt.world.rngTeleportSeq);
		CHECK(!(f.rng.fixes & RNG_FIX_TELEHOP));
		CHECK_NEAR(p.velocity[2], 0, 0);
	}
	// Teleported by the same slab the tick before: that link only ends at the server's frame end,
	// so g_bTouchingTrigger is still set and the trigger jump fix leaves the slab alone.
	{
		Fixture f;
		Player p;
		RunTick(f, p, 99, Input(), NoMove);
		p.origin[0] = -150; p.origin[1] = -550; p.origin[2] = 0.5f;
		p.flags = FL_CLIENT_BIT;
		RunTick(f, p, 100, Input(), NoMove);
		const int tp0 = f.rt.teleports;
		TriggerjumpScenario(f, p, 101);
		CHECK(!(f.rng.fixes & RNG_FIX_TRIGGERJUMP));
		CHECK(f.rt.teleports == tp0);
		CHECK(!f.rt.world.ents[0].linked);
	}
	// No trigger_sim: trigger jump can't run, the rest still does.
	{
		Fixture f;
		Player q;
		RunTick(f, q, 199, Input(), NoMove, false);
		q.origin[0] = 400; q.origin[2] = 163; q.velocity[0] = -300; q.velocity[2] = -400;
		RunTick(f, q, 200, Input(), [](Player& z) {
			z.origin[0] += -300 * DT; z.origin[2] += -406.25f * DT; z.velocity[2] = 0; z.flags |= FL_ONGROUND;
		}, false);
		CHECK(f.rng.fixes & RNG_FIX_INCLINE);
		CHECK_NEAR(q.velocity[0], -353.125, 1e-2);
	}
}

// Stairs (surf only): sliding into the 16 unit step's face ends up on top of it.
static void StairsScenario(Fixture& f, Player& p, int tick)
{
	p.origin[0] = -230; p.origin[1] = 0; p.origin[2] = 5;
	p.velocity[0] = -1000; p.velocity[1] = 0; p.velocity[2] = 0;
	p.flags = FL_CLIENT_BIT;
	RunTick(f, p, tick, Input(), [](Player& q) {
		q.origin[0] = -240 + 0.03125f;
		q.velocity[0] = 0;
		q.velocity[2] = -12.5f;
	});
}

static void TestStairs()
{
	{
		Fixture f;
		RngSetMapName(f.rng.env, "maps/surf_test.bsp");
		CHECK(f.rng.env.surfMap);
		Player p;
		RunTick(f, p, 99, Input(), NoMove);
		StairsScenario(f, p, 100);
		CHECK(f.rng.fixes & RNG_COLLISION);
		CHECK(f.rng.fixes & RNG_FIX_STAIRS);
		CHECK_NEAR(p.origin[2], 16.03125, 1e-3);
		CHECK(p.origin[0] < -240);
		CHECK_NEAR(p.velocity[0], -1000, 1e-3);
		CHECK_NEAR(p.velocity[2], -6.25, 1e-4);
		// The fix moved the player: trigger_sim marks the next command as game-moved.
		CHECK(f.rt.world.movedLast);
	}
	{
		Fixture f;
		RngSetMapName(f.rng.env, "bhop_test");
		CHECK(!f.rng.env.surfMap);
		Player p;
		RunTick(f, p, 99, Input(), NoMove);
		StairsScenario(f, p, 100);
		CHECK(!(f.rng.fixes & RNG_FIX_STAIRS));
		CHECK_NEAR(p.origin[2], 5, 0);
	}
	{
		Fixture f;
		RngSetMapName(f.rng.env, "surf_test");
		f.rng.settings.stairs = 0;
		Player p;
		RunTick(f, p, 99, Input(), NoMove);
		StairsScenario(f, p, 100);
		CHECK(!(f.rng.fixes & RNG_FIX_STAIRS));
	}
}

// On the ground without a fresh jump (by last predicted tick's buttons): not predicted.
static void TestGate()
{
	Fixture f;
	Player p;
	p.origin[2] = 0.03125f;
	p.flags = FL_CLIENT_BIT | FL_ONGROUND;
	RunTick(f, p, 100, Input(), NoMove);
	CHECK(!(f.rng.fixes & RNG_PREDICTED));
	// A jump is only seen once the stored buttons allow it: rngfix reads them stale.
	RngMoveInput jump = Input();
	jump.buttons = RNG_IN_JUMP;
	RunTick(f, p, 101, jump, NoMove);
	CHECK(!(f.rng.fixes & RNG_PREDICTED));
	p.flags = FL_CLIENT_BIT;
	p.origin[2] = 50;
	RunTick(f, p, 102, jump, NoMove);
	CHECK(f.rng.fixes & RNG_PREDICTED);
	// buttons were stored in the air, so this grounded tick is predicted (IN_JUMP not in oldbuttons).
	p.flags = FL_CLIENT_BIT | FL_ONGROUND;
	p.origin[2] = 0.03125f;
	RunTick(f, p, 103, jump, NoMove);
	CHECK(f.rng.fixes & RNG_PREDICTED);
	// Jumped: the impulse minus two half ticks of gravity.
	CHECK_NEAR(f.rng.st.preCollisionVelocity[2], RNG_DEFAULT_JUMP_IMPULSE - 12.5f, 1e-3);
}

static void TestShavitAutobhop()
{
	RngMoveInput held = Input();
	held.buttons = held.oldButtons = RNG_IN_JUMP | RNG_IN_DUCK;
	RngMoveInput a = held;
	RngShavitAutobhop(a, false, PRED_MOVETYPE_WALK);
	CHECK(a.oldButtons == (RNG_IN_JUMP | RNG_IN_DUCK));
	a = held;
	RngShavitAutobhop(a, true, PRED_MOVETYPE_WALK);
	CHECK(a.oldButtons == RNG_IN_DUCK);
	a = held;
	a.buttons = RNG_IN_DUCK;
	RngShavitAutobhop(a, true, PRED_MOVETYPE_WALK);
	CHECK(a.oldButtons == (RNG_IN_JUMP | RNG_IN_DUCK));
	a = held;
	RngShavitAutobhop(a, true, 9);  // ladder
	CHECK(a.oldButtons == (RNG_IN_JUMP | RNG_IN_DUCK));
	a = held;
	a.waterLevel = 2;
	RngShavitAutobhop(a, true, PRED_MOVETYPE_WALK);
	CHECK(a.oldButtons == (RNG_IN_JUMP | RNG_IN_DUCK));
	a = held;
	a.waterLevel = 1;
	RngShavitAutobhop(a, true, PRED_MOVETYPE_WALK);
	CHECK(a.oldButtons == RNG_IN_DUCK);

	// Jump held through a landing: with autobhop the grounded tick is a hop rngfix predicts.
	for (int autobhop = 0; autobhop < 2; ++autobhop) {
		Fixture f;
		RngPredict p;
		p.rng.settings.known = true;
		p.rng.settings.autobhop = autobhop != 0;
		p.rng.tracer = &f.tracer;
		p.rng.env.airAccelerate = 150.0f;
		RngMoveInput jump = Input();
		jump.buttons = jump.oldButtons = RNG_IN_JUMP;
		Player pl;
		pl.origin[2] = 50;
		for (int tick = 100; tick < 102; ++tick) {
			if (tick == 101) { pl.flags = FL_CLIENT_BIT | FL_ONGROUND; pl.origin[2] = 0.03125f; }
			PredPlayerIO& io = pl.IO(tick);
			RngPredictPreTick(p, io, jump, nullptr);
			RngPostMove(p.rng, io, -1, -1);
			RngEndTick(p.rng, io, nullptr);
		}
		CHECK(!(p.rng.st.oldButtons & RNG_IN_JUMP) == (autobhop != 0));
		CHECK(!!(p.rng.fixes & RNG_PREDICTED) == (autobhop != 0));
		if (autobhop) CHECK_NEAR(p.rng.st.preCollisionVelocity[2], RNG_DEFAULT_JUMP_IMPULSE - 12.5f, 1e-3);
	}
}

// Expected values are from bhoptools.sp's HashString, printed as a signed int.
static void TestMenuHash()
{
	CHECK(MenuHash("", 0) == 0x811c9dc5u);
	CHECK(MenuHash("a", 1) == 0xe40c292cu);
	CHECK(MenuHash("foobar", 6) == 0xbf9cf968u);
	const char menu[] = "Checkpoints\nWarning \xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80 \xff\x80\n \n->1. Save checkpoint (1/1000)\n"
		"2. Teleport to checkpoint #1\n3. Previous\n4. Next\n \n";
	CHECK(sizeof(menu) - 1 == 116);
	CHECK((int32_t)MenuHash(menu, 116) == 1808677158);
	CHECK((int32_t)MenuHash("ab\n", 3) == 826805568);
	CHECK(MenuHash("ab\nrest of the menu", 3) == MenuHash("ab\n", 3));
}

// Replays: the same command run again gives the same result, and state from a later
// run of a tick doesn't leak into an earlier replay of it.
static void TestReplay()
{
	struct Out { float o[3], v[3]; unsigned fixes; };
	auto snap = [](const Player& p, unsigned fx) { Out o; memcpy(o.o, p.origin, 12); memcpy(o.v, p.velocity, 12); o.fixes = fx; return o; };
	auto same = [](const Out& a, const Out& b) { return !memcmp(a.o, b.o, 12) && !memcmp(a.v, b.v, 12) && a.fixes == b.fixes; };

	Fixture f;
	Player p;
	std::vector<Out> first;
	std::vector<Player> starts;
	auto runAll = [&](int from, std::vector<Out>& out) {
		for (int tick = from; tick <= 104; ++tick) {
			Player& s = starts[(size_t)(tick - 100)];
			p = s;
			unsigned fx = 0;
			switch (tick) {
			case 100: fx = RunTick(f, p, tick, Input(), NoMove); break;
			case 101: TriggerjumpScenario(f, p, tick); fx = f.rng.fixes; break;
			case 102: UphillScenario(f, p, tick, 2.0f, [](Player&) {}); fx = f.rng.fixes; break;
			case 103: DownhillScenario(f, p, tick); fx = f.rng.fixes; break;
			default:  EdgeScenario(f, p, tick); fx = f.rng.fixes; break;
			}
			out.push_back(snap(p, fx));
		}
	};
	starts.assign(5, Player());
	runAll(100, first);
	CHECK(first[1].fixes & RNG_FIX_TELEHOP);
	CHECK(first[2].fixes & RNG_FIX_UPHILL);
	CHECK(first[3].fixes & RNG_FIX_INCLINE);
	CHECK(first[4].fixes & RNG_FIX_EDGE);

	for (int pass = 0; pass < 2; ++pass) {
		std::vector<Out> again;
		runAll(101, again);
		bool ok = true;
		for (size_t i = 0; i < again.size(); ++i) ok = ok && same(again[i], first[i + 1]);
		CHECK(ok);
	}

	// Stale state: tick 200 first run collides (uphill 0 keeps the collision recorded),
	// then 201 runs, then 200 is replayed as a no-collision downhill landing. Without
	// the snapshot, lastCollisionTick == 200 would block the downhill fix.
	Fixture g;
	g.rng.settings.uphill = 0;
	Player q;
	RunTick(g, q, 199, Input(), NoMove);
	const Player at199 = q;
	UphillScenario(g, q, 200, 2.0f, [](Player&) {});
	CHECK(g.rng.st.lastCollisionTick == 200);
	RunTick(g, q, 201, Input(), NoMove);
	q = at199;
	DownhillScenario(g, q, 200);
	CHECK(g.rng.fixes & RNG_FIX_INCLINE);
	CHECK_NEAR(q.velocity[0], -353.125, 1e-2);
}

// A flat floor 3 units below every trace start; knobs for the failure cases.
class FakeEngineTracer : public etrace::EngineTracer {
public:
	bool  fault = false;
	bool  ownStartSolid = false; // zero-length traces start solid, no player in the partition
	float sweptFraction = 3.0f / 64.0f;

protected:
	bool RawTrace(const etrace::RayX& ray, unsigned int, etrace::PlayerFilter&, etrace::TraceBufferX& buf) override
	{
		if (fault) return false;
		etrace::GameTraceX& t = buf.tr;
		const float start[3] = { ray.start.x + ray.startOffset.x, ray.start.y + ray.startOffset.y,
			ray.start.z + ray.startOffset.z };
		const float delta[3] = { ray.delta.x, ray.delta.y, ray.delta.z };
		t.fraction = ray.isSwept ? sweptFraction : 1.0f;
		t.startsolid = !ray.isSwept && ownStartSolid;
		for (int i = 0; i < 3; ++i) t.endpos[i] = start[i] + t.fraction * delta[i];
		t.plane.normal[2] = 1.0f;
		return true;
	}
	bool InGame() override { return true; }
	int  MaxClients() override { return 32; }
	void Log(const char*) override {}
	etrace::EntityHandleFn HandleFn() override { return nullptr; }
};

static void TestEngineTracer()
{
	const float origin[3] = { 0, 0, 0 }, end[3] = { 0, 0, -64 };
	RngTraceResult r;
	FakeEngineTracer e;

	// Own box starts solid with nothing filtered: inconclusive, not failed.
	e.ownStartSolid = true;
	CHECK(e.Validate(origin, true) == etrace::kValidatePending);
	CHECK(!e.Failed());
	e.ownStartSolid = false;
	CHECK(e.Validate(origin, true) == etrace::kValidateOk);
	CHECK(e.TraceHull(origin, end, MINS, MAXS, r));

	e.sweptFraction = NAN;
	CHECK(!e.TraceHull(origin, end, MINS, MAXS, r));
	CHECK(e.Ready());
	e.sweptFraction = 3.0f / 64.0f;
	CHECK(e.TraceHull(origin, end, MINS, MAXS, r));

	e.Reset();
	CHECK(!e.Ready() && !e.Failed());
	CHECK(e.Validate(origin, true) == etrace::kValidateOk);

	// A faulting engine call is for good, across levels too.
	e.fault = true;
	CHECK(!e.TraceHull(origin, end, MINS, MAXS, r));
	CHECK(e.Failed());
	e.fault = false;
	e.Reset();
	CHECK(e.Failed());
	CHECK(e.Validate(origin, true) == etrace::kValidateFailed);
}

static void TestNoclip()
{
	const int W = NC_MOVETYPE_WALK, N = NC_MOVETYPE_NOCLIP, LADDER = 9;
	CHECK(NcResult(NC_IMPULSE_TOGGLE, W, NC_ALLOW_ALL) == N);
	CHECK(NcResult(NC_IMPULSE_TOGGLE, LADDER, NC_ALLOW_ALL) == N);
	CHECK(NcResult(NC_IMPULSE_TOGGLE, N, NC_ALLOW_ALL) == W);
	CHECK(NcResult(NC_IMPULSE_TOGGLE, W, NC_ALLOW_LEAVE | NC_ALLOW_PLUS) == -1);
	CHECK(NcResult(NC_IMPULSE_TOGGLE, N, NC_ALLOW_ENTER | NC_ALLOW_PLUS) == -1);
	CHECK(NcResult(NC_IMPULSE_ON, W, NC_ALLOW_ALL) == N);
	CHECK(NcResult(NC_IMPULSE_ON, N, NC_ALLOW_ALL) == -1);
	CHECK(NcResult(NC_IMPULSE_ON, W, NC_ALLOW_ENTER | NC_ALLOW_LEAVE) == -1);
	CHECK(NcResult(NC_IMPULSE_OFF, N, 0) == W);
	CHECK(NcResult(NC_IMPULSE_OFF, W, NC_ALLOW_ALL) == -1);
	CHECK(NcResult(150, W, NC_ALLOW_ALL) == -1);
	CHECK(!NcIsImpulse(153) && NcIsImpulse(154) && NcIsImpulse(156) && !NcIsImpulse(157));

	// The server runs command n on the movetype after n-1 with its flags then (allow[n]); the client
	// learns the state after command t-rs (snapshot) and the acks and flags up to t-ra, and replays
	// every command since the snapshot. Each command's result must match the server's once the
	// client has the server's answer for it, and always while the flags don't change.
	const int kCmds = 600;
	static int imp[kCmds + 1], allow[kCmds + 1], sv[kCmds + 1];
	uint32_t seed = 12345;
	auto rnd = [&seed](int n) { seed = seed * 1103515245u + 12345u; return (int)((seed >> 16) % (uint32_t)n); };
	int mismatchesKnown = 0, mismatchesSteady = 0, applied = 0, replays = 0, refused = 0;
	for (int trial = 0; trial < 200; ++trial) {
		const bool steady = trial % 2 == 0;
		const int rs = 1 + rnd(8);
		const int ra = 1 + rnd(rs);
		int a = NC_ALLOW_ALL;
		sv[0] = W;
		imp[0] = 0;
		allow[0] = a;
		for (int n = 1; n <= kCmds; ++n) {
			if (!steady && rnd(20) == 0) a = rnd(8);
			allow[n] = a;
			const int r = rnd(10);
			imp[n] = r == 0 ? NC_IMPULSE_TOGGLE : r == 1 ? NC_IMPULSE_ON : r == 2 ? NC_IMPULSE_OFF : r == 3 ? 150 : 0;
			const int to = NcIsImpulse(imp[n]) ? NcResult(imp[n], sv[n - 1], allow[n]) : -1;
			sv[n] = to >= 0 ? to : sv[n - 1];
			if (NcIsImpulse(imp[n]) && to < 0) ++refused;
		}
		NcLog log;
		int acked = 0;
		for (int t = 1; t <= kCmds; ++t) {
			for (; acked < t - ra; ++acked)
				if (NcIsImpulse(imp[acked + 1])) NcAck(log, acked + 1, sv[acked + 1] != sv[acked]);
			const int hint = allow[t - ra > 0 ? t - ra : 0];
			const int base = t - rs > 0 ? t - rs : 0;
			int mt = sv[base];
			for (int n = base + 1; n <= t; ++n) {
				if (NcIsImpulse(imp[n])) {
					const int before = mt;
					mt = NcPredict(log, n, imp[n], mt, true, hint);
					if (mt != before) ++applied;
				}
				++replays;
				if (n <= t - ra && mt != sv[n]) ++mismatchesKnown;
				if (steady && mt != sv[n]) ++mismatchesSteady;
			}
		}
	}
	CHECK(mismatchesKnown == 0);
	CHECK(mismatchesSteady == 0);
	CHECK(applied > 1000 && replays > 100000 && refused > 1000);

	NcLog log;
	CHECK(NcPredict(log, 7, NC_IMPULSE_TOGGLE, W, false, NC_ALLOW_ALL) == W);
	NcAck(log, 7, true);
	CHECK(NcPredict(log, 7, NC_IMPULSE_TOGGLE, W, false, NC_ALLOW_ALL) == N);
	// A refusal undoes a predicted toggle on replays that still run it.
	CHECK(NcPredict(log, 9, NC_IMPULSE_TOGGLE, N, true, NC_ALLOW_ALL) == W);
	NcAck(log, 9, false);
	CHECK(NcPredict(log, 9, NC_IMPULSE_TOGGLE, N, true, NC_ALLOW_ALL) == N);
}

int main()
{
	TestTracer();
	TestDownhill();
	TestUphill();
	TestEdge();
	TestTriggerjumpTelehop();
	TestStairs();
	TestGate();
	TestReplay();
	TestShavitAutobhop();
	TestMenuHash();
	TestNoclip();
	TestEngineTracer();
	printf("rngfix: %d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
