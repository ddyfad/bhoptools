#define _USE_MATH_DEFINES
#include "prediction.h"

#include <cstdio>
#include <cmath>
#include <vector>

static int g_fail = 0, g_pass = 0;

#define CHECK(cond) do { if (cond) ++g_pass; else { ++g_fail; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_NEAR(a, b, eps) do { const double a_ = (a), b_ = (b); if (fabs(a_ - b_) <= (eps)) ++g_pass; else { ++g_fail; \
	printf("FAIL %s:%d: %s = %.6f, expected %.6f\n", __FILE__, __LINE__, #a, a_, b_); } } while (0)

static const float DT = 0.01f;
static const float BOOST = -750.0f * 1.005f; // CheckMovingGround at 100 tick
static const float IN_Z = 1.0f, OUT_Z = 9.5f; // feet in the 4-unit slab, and 8.5 higher (a duck in the air)

static PredMapEntity Ent(std::initializer_list<PredKv> kv)
{
	PredMapEntity e;
	e.kv = kv;
	return e;
}

// bhop_coolkist_s2's booster: a 256x256x4 trigger_multiple floor slab, wait 1, clients only.
static PredMap* SlabMap()
{
	PredMap* m = new PredMap();
	const float lo[3] = { -128, -128, 0 }, hi[3] = { 128, 128, 4 };
	PredBrushSpan b = {};
	b.contents = 1;
	b.isBox = true;
	b.planeCount = 6;
	for (int c = 0; c < 3; ++c) {
		BspPlane up = {}, dn = {};
		up.normal[c] = 1; up.dist = hi[c]; up.type = c;
		dn.normal[c] = -1; dn.dist = -lo[c]; dn.type = c;
		m->brushPlanes.push_back(up);
		m->brushPlanes.push_back(dn);
		b.boxMins[c] = lo[c];
		b.boxMaxs[c] = hi[c];
	}
	m->brushes.push_back(b);
	BspModel world = {}, trig = {};
	for (int c = 0; c < 3; ++c) { trig.mins[c] = lo[c]; trig.maxs[c] = hi[c]; world.mins[c] = -4096; world.maxs[c] = 4096; }
	m->models = { world, trig };
	m->modelBrushes = { { 0, 0 }, { 0, 1 } };
	m->entities.push_back(Ent({ { "classname", "worldspawn" } }));
	m->entities.push_back(Ent({ { "classname", "trigger_multiple" }, { "model", "*1" }, { "origin", "0 0 2" },
		{ "wait", "1" }, { "spawnflags", "1" }, { "hammerid", "1" },
		{ "OnEndTouch", "!activator,AddOutput,basevelocity 0 -750 0,0,-1" } }));
	m->loaded = true;
	PredBuildTriggers(*m);
	return m;
}

struct Counts { int starts = 0, ends = 0; std::vector<int> endTicks; };

struct Player {
	float origin[3] = { 0, 0, 0 }, velocity[3] = { 0, 0, 0 }, baseVel[3] = { 0, 0, 0 };
	float gravity = 1.0f;
	int flags = FL_CLIENT_BIT;
	uint8_t moveFlags = 1;
};

struct Sim {
	PredRuntime rt;
	Player p;
	Counts counts;
	int tick = 1000;
	Sim() { PredAdopt(rt, SlabMap()); }
	~Sim() { PredReset(rt); }

	// One command: the move puts the feet at z, ducked or not, with FL_DUCKING from the last command.
	void Cmd(float z, bool frameEnd, bool ducked = false)
	{
		PredPlayerIO io;
		io.origin = p.origin;
		io.velocity = p.velocity;
		io.baseVelocity = p.baseVel;
		io.gravity = &p.gravity;
		io.flags = &p.flags;
		io.moveFlags = &p.moveFlags;
		io.tick = tick++;
		io.moveType = PRED_MOVETYPE_WALK;
		io.solid = true;
		io.interval = DT;
		io.curtime = io.tick * DT;
		io.frameEnd = frameEnd;
		PredSetHull(io, (p.flags & FL_DUCKING) != 0);
		PredPreMove(rt, io);
		p.origin[2] = z;
		p.flags = ducked ? p.flags | FL_DUCKING : p.flags & ~FL_DUCKING;
		const bool was = Linked();
		PredPostMove(rt, io);
		if (!was && Linked()) ++counts.starts;
		if (was && !Linked()) { ++counts.ends; counts.endTicks.push_back(io.tick); }
	}
	bool Linked() const { return rt.world.ents[0].linked != 0; }
};

// The server: PhysicsTouchTriggers stamps each command's touches, PhysicsCheckForEntityUntouch
// ends the stale links once per frame.
struct ServerModel {
	bool linked = false;
	int stamp = 0, linkStamp = 0, starts = 0, ends = 0;
	void Cmd(bool overlap)
	{
		++stamp;
		if (!overlap) return;
		if (!linked) { linked = true; ++starts; }
		linkStamp = stamp;
	}
	void FrameEnd() { if (linked && linkStamp != stamp) { linked = false; ++ends; } }
};

static void TestEveryCommandAFrame()
{
	Sim s;
	s.Cmd(IN_Z, true);
	s.Cmd(OUT_Z, true);
	CHECK(s.counts.ends == 1);
	s.Cmd(IN_Z, true);
	CHECK_NEAR(s.p.velocity[1], BOOST, 1e-3);
	s.Cmd(OUT_Z, true);
	s.Cmd(OUT_Z, true);
	CHECK(s.counts.starts == 2 && s.counts.ends == 2);
	CHECK_NEAR(s.p.velocity[1], -1507.5, 1e-3);
}

static void TestBatchedFrame()
{
	Sim s;
	s.Cmd(IN_Z, true);
	s.Cmd(OUT_Z, false);
	CHECK(s.Linked());      // rngfix's g_bTouchingTrigger holds until the untouch too
	s.Cmd(IN_Z, false);
	CHECK(s.counts.starts == 1 && s.counts.ends == 0);
	s.Cmd(OUT_Z, true);
	CHECK(!s.Linked());
	CHECK(s.counts.starts == 1 && s.counts.ends == 1);
	CHECK(s.counts.endTicks.size() == 1 && s.counts.endTicks[0] == 1003);
	CHECK(s.p.velocity[1] == 0.0f);
	s.Cmd(OUT_Z, true);     // applied at the start of the next command
	CHECK_NEAR(s.p.velocity[1], BOOST, 1e-3);
	s.Cmd(OUT_Z, true);
	CHECK_NEAR(s.p.velocity[1], BOOST, 1e-3);
}

static void TestInOutOneFrame()
{
	Sim s;
	s.Cmd(OUT_Z, true);
	s.Cmd(IN_Z, false);
	CHECK(s.counts.starts == 1);
	s.Cmd(OUT_Z, true);
	CHECK(s.counts.starts == 1 && s.counts.ends == 1);
	CHECK(s.counts.endTicks.size() == 1 && s.counts.endTicks[0] == 1002);
	s.Cmd(OUT_Z, true);
	CHECK_NEAR(s.p.velocity[1], BOOST, 1e-3);
}

// Scroll-spammed duck over the slab (a duck in the air lifts the feet out of it) with the
// commands batched into frames of `frame` commands; the sim has to agree with the server.
// perCommand: with bhoptools' CAPS_UNTOUCH the server untouches after every command whatever the
// frames, and the client passes frameEnd on every command.
static void SpamCase(int frame, int phase, int every, int ticks, bool perCommand = false)
{
	Sim s;
	ServerModel sv;
	s.Cmd(IN_Z, true);
	sv.Cmd(true);
	sv.FrameEnd();
	for (int t = 0; t < ticks; ++t) {
		const bool ducked = ((t + phase) / every) % 2 == 1;
		const bool end = (t + 1) % frame == 0 || t == ticks - 1;
		s.Cmd(ducked ? OUT_Z : IN_Z, end || perCommand, ducked);
		sv.Cmd(!ducked);
		if (end || perCommand) sv.FrameEnd();
	}
	for (int i = 0; i < 2; ++i) {
		s.Cmd(OUT_Z, true);
		sv.Cmd(false);
		sv.FrameEnd();
	}
	CHECK(s.counts.starts == sv.starts && s.counts.ends == sv.ends);
	CHECK_NEAR(s.p.velocity[1], BOOST * sv.ends, 1e-2);
}

static void TestScrollSpam()
{
	for (int frame = 1; frame <= 3; ++frame)
		for (int phase = 0; phase < frame; ++phase)
			SpamCase(frame, phase, 1, 10);
	SpamCase(2, 0, 2, 12);
	SpamCase(2, 1, 2, 12);
	SpamCase(3, 0, 2, 12);
	for (int frame = 1; frame <= 3; ++frame)
		for (int phase = 0; phase < frame; ++phase)
			SpamCase(frame, phase, 1, 10, true);
}

// Callers that don't know the packet (tests, unknown commands) keep one command per frame.
static void TestDefaultFrameEnd()
{
	PredPlayerIO io;
	CHECK(io.frameEnd);
}

// CAPS_UNTOUCH: two commands merged into one server frame. The link ends after the command that
// left, so the boost comes with the next one, not a command later.
static void TestUntouchEachCommand()
{
	Sim s;
	s.Cmd(IN_Z, true);
	s.Cmd(OUT_Z, true);
	CHECK(s.counts.ends == 1 && s.counts.endTicks[0] == 1001);
	s.Cmd(OUT_Z, true);
	CHECK_NEAR(s.p.velocity[1], BOOST, 1e-3);
}

struct Box { float lo[3], hi[3]; };

// ents[0] must be worldspawn; brush entity "*k" is boxes[k-1], in world coordinates.
static PredMap* BoxMap(const std::vector<Box>& boxes, const std::vector<PredMapEntity>& ents)
{
	PredMap* m = new PredMap();
	BspModel world = {};
	for (int c = 0; c < 3; ++c) { world.mins[c] = -8192; world.maxs[c] = 8192; }
	m->models.push_back(world);
	m->modelBrushes.push_back({ 0, 0 });
	for (const Box& bx : boxes) {
		PredBrushSpan b = {};
		b.firstPlane = (int)m->brushPlanes.size();
		b.contents = 1;
		b.isBox = true;
		b.planeCount = 6;
		BspModel trig = {};
		for (int c = 0; c < 3; ++c) {
			BspPlane up = {}, dn = {};
			up.normal[c] = 1; up.dist = bx.hi[c]; up.type = c;
			dn.normal[c] = -1; dn.dist = -bx.lo[c]; dn.type = c;
			m->brushPlanes.push_back(up);
			m->brushPlanes.push_back(dn);
			b.boxMins[c] = trig.mins[c] = bx.lo[c];
			b.boxMaxs[c] = trig.maxs[c] = bx.hi[c];
		}
		m->modelBrushes.push_back({ (int)m->brushes.size(), 1 });
		m->brushes.push_back(b);
		m->models.push_back(trig);
	}
	m->entities = ents;
	m->loaded = true;
	PredBuildTriggers(*m);
	return m;
}

// ON_TRIGGER: OnTrigger fired, seen as a trigger_multiple's wait starting or it being spent.
enum { LINK_START, LINK_END, ON_TRIGGER };
struct LinkEv { int kind, sim, tick; };

struct World {
	PredRuntime rt;
	Player p;
	uint32_t ground = 0xFFFFFFFFu;
	std::vector<LinkEv> log;
	int tick = 1000, command = 5000;
	int buttons = 0, lastButtons = 0;
	float view[3] = { 0, 0, 0 };
	explicit World(PredMap* m, bool eventQueueFix = true)
	{
		rt.eventQueueFix = eventQueueFix;
		PredAdopt(rt, m);
	}
	~World() { PredReset(rt); }

	// The sim has one entry per entity of a class it models, in entity order.
	int Sim(const char* name) const
	{
		int i = 0;
		for (const PredMapEntity& e : rt.map->entities) {
			if (PredClassify(e.Get("classname")) == PC_NONE) continue;
			if (!strcmp(e.Get("targetname"), name)) return i;
			++i;
		}
		return -1;
	}

	// One command: the move puts the feet at `to`, then the touch pass. `jump`: CheckJumpButton runs
	// where the move started, on `ground`.
	void Cmd(const float to[3], bool frameEnd = true, bool jump = false)
	{
		PredPlayerIO io;
		io.origin = p.origin;
		io.velocity = p.velocity;
		io.baseVelocity = p.baseVel;
		io.gravity = &p.gravity;
		io.flags = &p.flags;
		io.groundEntity = &ground;
		io.moveFlags = &p.moveFlags;
		io.tick = tick++;
		io.command = command++;
		io.moveType = PRED_MOVETYPE_WALK;
		io.solid = true;
		io.interval = DT;
		io.curtime = io.tick * DT;
		io.frameEnd = frameEnd;
		io.buttons = buttons;
		io.oldButtons = lastButtons;
		lastButtons = buttons;
		memcpy(io.viewAngles, view, sizeof view);
		PredSetHull(io, false);
		PredPreMove(rt, io);
		if (jump) PredMpbhopsJumpGround(rt, p.origin, ground != 0xFFFFFFFFu && (ground & 0xFFF) != 0, (p.moveFlags & 2) != 0);
		memcpy(p.origin, to, sizeof p.origin);
		const std::vector<PredEntState> was = rt.world.ents;
		PredPostMove(rt, io);
		for (size_t i = 0; i < was.size(); ++i) {
			const PredEntState& now = rt.world.ents[i];
			// Links a command both starts and ends (a push once, one a teleport makes stale) don't show.
			if (was[i].linked != now.linked) log.push_back({ was[i].linked ? LINK_END : LINK_START, (int)i, io.tick });
			if (was[i].multiReadyTick != now.multiReadyTick || was[i].multiWaitUntil != now.multiWaitUntil
					|| (!was[i].spent && now.spent))
				log.push_back({ ON_TRIGGER, (int)i, io.tick });
		}
	}

	int Count(int kind, int sim) const
	{
		int n = 0;
		for (const LinkEv& e : log) n += e.kind == kind && e.sim == sim;
		return n;
	}
	int LastTick(int kind, int sim) const
	{
		int t = -1;
		for (const LinkEv& e : log) if (e.kind == kind && e.sim == sim) t = e.tick;
		return t;
	}
};

static PredMapEntity WorldEnt() { return Ent({ { "classname", "worldspawn" } }); }
static const Box kBox = { { -64, -64, 0 }, { 64, 64, 96 } };
static const float kIn[3] = { 0, 0, 1 };

// The server's SetGroundEntity(NULL) clears m_hGroundEntity too; without that the client's
// CategorizePosition finds the same ground, keeps FL_ONGROUND clear and the player hovers.
static void TestGroundHandleCleared()
{
	{
		World w(BoxMap({ kBox }, { WorldEnt(),
			Ent({ { "classname", "trigger_push" }, { "model", "*1" }, { "pushdir", "-90 0 0" }, { "speed", "400" },
				{ "spawnflags", "1" } }) }));
		memcpy(w.p.origin, kIn, sizeof kIn);
		w.p.flags |= FL_ONGROUND;
		w.ground = 0;
		w.Cmd(kIn);
		CHECK(w.ground == 0xFFFFFFFFu && !(w.p.flags & FL_ONGROUND));
		CHECK_NEAR(w.p.origin[2], 2.0, 1e-6);
		CHECK_NEAR(w.p.baseVel[2], 400.0, 1e-3);
	}
	{
		World w(BoxMap({ kBox }, { WorldEnt(),
			Ent({ { "classname", "trigger_push" }, { "model", "*1" }, { "pushdir", "-90 0 0" }, { "speed", "400" },
				{ "spawnflags", "129" } }) }));
		w.p.flags |= FL_ONGROUND;
		w.ground = 0;
		w.Cmd(kIn);
		CHECK(w.ground == 0xFFFFFFFFu && !(w.p.flags & FL_ONGROUND));
		CHECK(w.rt.world.ents[0].removed && !w.rt.world.ents[0].linked);
	}
	{
		World w(BoxMap({ kBox }, { WorldEnt(),
			Ent({ { "classname", "trigger_teleport" }, { "model", "*1" }, { "target", "dest" }, { "spawnflags", "1" } }),
			Ent({ { "classname", "info_teleport_destination" }, { "targetname", "dest" }, { "origin", "1000 0 0" } }) }));
		w.p.flags |= FL_ONGROUND;
		w.ground = 0;
		w.Cmd(kIn);
		CHECK(w.ground == 0xFFFFFFFFu && !(w.p.flags & FL_ONGROUND));
		CHECK_NEAR(w.p.origin[0], 1000.0, 1e-6);
	}
}

// Teleport sets FSOLID_NOT_SOLID on the player, which bumps its touchStamp: links touched before
// it in the pass go stale (the reset trigger ends on the teleport's command), the teleport's own
// is stamped after its Touch and ends a command later. Teleports run last in the pass.
static void TestTeleportStaleLinks()
{
	World w(BoxMap({ { { -32, -32, 0 }, { 32, 32, 64 } }, { { -512, -512, 0 }, { 512, 512, 128 } } }, { WorldEnt(),
		Ent({ { "classname", "trigger_teleport" }, { "model", "*1" }, { "target", "dest" }, { "spawnflags", "1" } }),
		Ent({ { "classname", "trigger_multiple" }, { "model", "*2" }, { "targetname", "reset" }, { "spawnflags", "1" } }),
		Ent({ { "classname", "info_teleport_destination" }, { "targetname", "dest" }, { "origin", "5000 0 0" } }) }));
	const int tele = 0, reset = w.Sim("reset");
	const float outside[3] = { -100, 0, 1 }, away[3] = { 5002, 0, 0 };
	w.Cmd(outside);
	w.Cmd(kIn);
	const int teleTick = w.tick - 1;
	CHECK_NEAR(w.p.origin[0], 5000.0, 1e-6);
	CHECK(w.LastTick(LINK_END, reset) == teleTick);
	CHECK(w.Count(LINK_END, tele) == 0 && w.rt.world.ents[tele].linked);
	w.Cmd(away);
	CHECK(w.LastTick(LINK_END, tele) == teleTick + 1);
}

static void TestBranchToggle()
{
	CHECK(PredInputId(PC_BRANCH, "Toggle") == IN_BR_TOGGLE);
	CHECK(PredInputId(PC_BRANCH, "toggletest") == IN_BR_TOGGLETEST);
	CHECK(PredInputId(PC_RELAY, "Toggle") == IN_TOGGLE);
	CHECK(PredInputId(PC_MULTIPLE, "Toggle") == IN_TOGGLE);
	CHECK(PredInputId(PC_BRANCH, "Kill") == IN_KILL);
	World w(BoxMap({ kBox }, { WorldEnt(),
		Ent({ { "classname", "trigger_multiple" }, { "model", "*1" }, { "spawnflags", "1" },
			{ "OnStartTouch", "br,Toggle,,0,-1" } }),
		Ent({ { "classname", "logic_branch" }, { "targetname", "br" } }) }));
	w.Cmd(kIn);
	w.Cmd(kIn);
	const PredEntState& st = w.rt.world.ents[w.Sim("br")];
	CHECK(st.branchValue == 1 && st.disabled == 0);
}

// A removed trigger lives until the frame's CleanupDeleteList, which drops its links without the
// trigger's EndTouch; an untouch before that fires it like any stale link.
static void TestKillEndsTouch()
{
	{
		// trigger_once: SUB_Remove 0.1 s after firing, after a touch pass that still found the player.
		World w(BoxMap({ kBox }, { WorldEnt(),
			Ent({ { "classname", "trigger_once" }, { "model", "*1" }, { "spawnflags", "1" },
				{ "OnEndTouch", "!activator,AddOutput,gravity 0.5,0,-1" } }) }));
		for (int i = 0; i < 14; ++i) w.Cmd(kIn);
		CHECK(w.rt.world.ents[0].removed && !w.rt.world.ents[0].linked);
		CHECK(w.Count(LINK_END, 0) == 1);
		w.Cmd(kIn);
		CHECK_NEAR(w.p.gravity, 1.0, 1e-6);
	}
	{
		// Left within the frame SUB_Remove runs in: its untouch ends the stale link, OnEndTouch included.
		World w(BoxMap({ kBox }, { WorldEnt(),
			Ent({ { "classname", "trigger_once" }, { "model", "*1" }, { "spawnflags", "1" },
				{ "OnEndTouch", "!activator,AddOutput,gravity 0.5,0,-1" } }) }));
		const float outside[3] = { 200, 0, 1 };
		for (int i = 0; i < 9; ++i) w.Cmd(kIn);
		for (int i = 0; i < 4; ++i) w.Cmd(outside, false);
		CHECK(w.rt.world.ents[0].removed && w.rt.world.ents[0].linked);
		w.Cmd(outside);
		CHECK(!w.rt.world.ents[0].linked && w.Count(LINK_END, 0) == 1);
		w.Cmd(outside);
		CHECK_NEAR(w.p.gravity, 0.5, 1e-6);
	}
	{
		// eventqueuefix: the Kill runs at the next command's start, before its touch pass.
		World w(BoxMap({ kBox }, { WorldEnt(),
			Ent({ { "classname", "trigger_multiple" }, { "model", "*1" }, { "spawnflags", "1" },
				{ "OnStartTouch", "!self,Kill,,0,-1" }, { "OnEndTouch", "!activator,AddOutput,gravity 0.25,0,-1" } }) }));
		w.Cmd(kIn);
		w.Cmd(kIn, false);
		CHECK(w.rt.world.ents[0].removed && w.rt.world.ents[0].linked);
		w.Cmd(kIn);
		CHECK(!w.rt.world.ents[0].linked);
		CHECK_NEAR(w.p.gravity, 1.0, 1e-6);
		w.Cmd(kIn);
		CHECK_NEAR(w.p.gravity, 0.25, 1e-6);
	}
	{
		// Stock queue: ServiceEventQueue runs after the frame's untouch, then the entity is deleted.
		World w(BoxMap({ kBox }, { WorldEnt(),
			Ent({ { "classname", "trigger_multiple" }, { "model", "*1" }, { "spawnflags", "1" },
				{ "OnStartTouch", "!self,Kill,,0,-1" }, { "OnEndTouch", "!activator,AddOutput,gravity 0.25,0,-1" } }) }),
			false);
		for (int i = 0; i < 4; ++i) w.Cmd(kIn);
		CHECK(w.rt.world.ents[0].removed && !w.rt.world.ents[0].linked);
		w.Cmd(kIn);
		CHECK_NEAR(w.p.gravity, 1.0, 1e-6);
	}
}

static void TestCounterDivideByZero()
{
	World w(BoxMap({ kBox }, { WorldEnt(),
		Ent({ { "classname", "trigger_multiple" }, { "model", "*1" }, { "spawnflags", "1" },
			{ "OnStartTouch", "cnt,Divide,0,0,-1" } }),
		Ent({ { "classname", "math_counter" }, { "targetname", "cnt" }, { "startvalue", "6" },
			{ "OutValue", "!activator,AddOutput,gravity 0.5,0,-1" } }) }));
	w.Cmd(kIn);
	w.Cmd(kIn);
	const int cnt = w.Sim("cnt");
	CHECK_NEAR(w.p.gravity, 0.5, 1e-6);
	CHECK(w.rt.world.ents[cnt].counterValue == 6.0f);
}

// The fgd's "Disallow entities that match criteria" isn't a number, so the filter isn't negated.
static void TestFilterNegatedText()
{
	CHECK(!PredFilterNegated("Disallow entities that match criteria") && PredFilterNegated("1") && !PredFilterNegated("0"));
	World w(BoxMap({ kBox }, { WorldEnt(),
		Ent({ { "classname", "filter_activator_name" }, { "targetname", "filt" }, { "filtername", "someone" },
			{ "Negated", "Disallow entities that match criteria" } }),
		Ent({ { "classname", "trigger_multiple" }, { "model", "*1" }, { "spawnflags", "1" }, { "filtername", "filt" },
			{ "OnStartTouch", "!activator,AddOutput,gravity 0.5,0,-1" } }) }));
	w.Cmd(kIn);
	w.Cmd(kIn);
	CHECK_NEAR(w.p.gravity, 1.0, 1e-6);
}

// A target that names nothing: Touch returns before teleporting, but the touch outputs fire.
static void TestTeleportWithoutDestination()
{
	World w(BoxMap({ kBox }, { WorldEnt(),
		Ent({ { "classname", "trigger_teleport" }, { "model", "*1" }, { "target", "nothing" }, { "spawnflags", "1" },
			{ "OnStartTouch", "!activator,AddOutput,gravity 0.75,0,-1" } }) }));
	w.p.flags |= FL_ONGROUND;
	w.ground = 0;
	w.Cmd(kIn);
	w.Cmd(kIn);
	CHECK(w.rt.map->sim[0].cls == PC_TELEPORT);
	CHECK_NEAR(w.p.gravity, 0.75, 1e-6);
	CHECK_NEAR(w.p.origin[0], 0.0, 1e-6);
	CHECK(w.ground == 0 && (w.p.flags & FL_ONGROUND));
}

// The server can move our tickbase between a first run and a replay; the state before a replayed
// command is found by command number.
static void TestSnapshotByCommand()
{
	World w(BoxMap({ kBox }, { WorldEnt(),
		Ent({ { "classname", "trigger_multiple" }, { "model", "*1" }, { "spawnflags", "1" },
			{ "OnStartTouch", "!activator,AddOutput,gravity 0.5,0,-1" } }) }));
	w.Cmd(kIn);
	w.Cmd(kIn);
	CHECK_NEAR(w.p.gravity, 0.5, 1e-6);
	w.p.gravity = 1.0f;
	w.command--;
	w.tick += 1;
	w.Cmd(kIn);
	CHECK_NEAR(w.p.gravity, 0.5, 1e-6);
}

// mpbhops_but_working: a sinking door over a teleport, a rising boost door, one with nothing under
// its travel and one with wait 0 (neither hooked). Door origins are 0, so position1 is 0 too.
static PredMap* MpbhopsMap()
{
	return BoxMap({ { { -64, -64, -16 }, { 64, 64, 0 } }, { { -64, -64, -128 }, { 64, 64, -48 } },
		{ { 200, -64, -16 }, { 328, 64, 0 } }, { { -64, 300, -16 }, { 64, 428, 0 } }, { { 400, -64, -16 }, { 528, 64, 0 } } },
		{ WorldEnt(),
		Ent({ { "classname", "func_door" }, { "model", "*1" }, { "spawnflags", "1024" }, { "movedir", "90 0 0" },
			{ "lip", "-112" }, { "wait", "-1" } }),
		Ent({ { "classname", "trigger_teleport" }, { "model", "*2" }, { "targetname", "tele" }, { "target", "dest" },
			{ "spawnflags", "1" } }),
		Ent({ { "classname", "info_teleport_destination" }, { "targetname", "dest" }, { "origin", "1000 0 0" } }),
		Ent({ { "classname", "func_door" }, { "model", "*3" }, { "spawnflags", "1024" }, { "movedir", "-90 0 0" },
			{ "speed", "400" }, { "wait", "1" } }),
		Ent({ { "classname", "func_door" }, { "model", "*4" }, { "spawnflags", "1024" }, { "movedir", "90 0 0" },
			{ "lip", "-112" } }),
		Ent({ { "classname", "func_door" }, { "model", "*5" }, { "spawnflags", "1024" }, { "movedir", "-90 0 0" },
			{ "speed", "400" } }) });
}

static const float kOnDoor[3] = { 0, 0, 0.03125f }, kOnBoost[3] = { 264, 0, 0.03125f }, kInAir[3] = { 0, 0, 40 };
static const uint32_t kDoorHandle = 70;

// Stands on the sinking door (or jumps above it) until teleported; the tick it happened on, or -1.
static int MpbhopsStand(World& w, int commands, const float* at = kOnDoor)
{
	for (int i = 0; i < commands; ++i) {
		w.ground = at == kInAir ? 0xFFFFFFFFu : kDoorHandle;
		w.Cmd(at);
		if (w.p.origin[0] == 1000.0f) return w.tick - 1;
	}
	return -1;
}

// The server's float math: teleported once curtime - (first touch + 0.06) > 0.06.
static int MpbhopsExpected(int firstTick)
{
	const float punish = firstTick * DT + PRED_MPBHOPS_DELAY;
	int k = firstTick + 1;
	while (!(k * DT - punish > PRED_MPBHOPS_DELAY)) ++k;
	return k;
}

static void TestMpbhopsDoors()
{
	World w(MpbhopsMap());
	const auto& doors = w.rt.map->mpbhopsDoors;
	CHECK(doors.size() == 2);
	CHECK(doors.size() == 2 && doors[0].tele == w.Sim("tele") && !doors[0].boost && doors[0].modelIndex == 1);
	CHECK(doors.size() == 2 && doors[1].boost && doors[1].tele < 0 && doors[1].speed == 400.0f && doors[1].modelIndex == 3);
}

static void TestMpbhopsSinkingDelay()
{
	World w(MpbhopsMap());
	w.rt.mpbhops = true;
	const int first = w.tick, firstCmd = w.command;
	const int at = MpbhopsStand(w, 100);
	CHECK(at == MpbhopsExpected(first));
	CHECK(at - first == 12 || at - first == 13);
	CHECK_NEAR(w.p.origin[1], 0.0, 1e-6);
	CHECK(w.ground == 0xFFFFFFFFu && w.rt.world.mpbhopsBlock == -1);
	// A plain Touch: no StartTouch, so no link and no OnStartTouch.
	CHECK(w.Count(LINK_START, w.Sim("tele")) == 0 && !w.rt.world.ents[w.Sim("tele")].linked);

	// Re-predicting from the command before the teleport comes back to it.
	const int teleCmd = firstCmd + (at - first);
	w.command = teleCmd;
	w.tick = at;
	memcpy(w.p.origin, kOnDoor, sizeof kOnDoor);
	CHECK(MpbhopsStand(w, 1) == at);
}

static void TestMpbhopsCooldown()
{
	{
		// Back on the same door inside the window: teleported on the first touch.
		World w(MpbhopsMap());
		w.rt.mpbhops = true;
		MpbhopsStand(w, 1);
		CHECK(MpbhopsStand(w, 49, kInAir) < 0);
		CHECK(MpbhopsStand(w, 1) == w.tick - 1);
	}
	{
		// Past the cooldown (1.1 s after the first touch's +0.06) it starts over.
		World w(MpbhopsMap());
		w.rt.mpbhops = true;
		MpbhopsStand(w, 1);
		CHECK(MpbhopsStand(w, 119, kInAir) < 0);
		const int back = w.tick;
		const int at = MpbhopsStand(w, 100);
		CHECK(at == MpbhopsExpected(back));
	}
	{
		// Only the ground counts, and only a ground that isn't the world.
		World w(MpbhopsMap());
		w.rt.mpbhops = true;
		for (int i = 0; i < 50; ++i) { w.ground = 0; w.Cmd(kOnDoor); }
		CHECK(w.p.origin[0] == 0.0f && w.rt.world.mpbhopsBlock == -1);
	}
}

static void TestMpbhopsBoost()
{
	World w(MpbhopsMap());
	w.rt.mpbhops = true;
	const int d = PredMpbhopsJumpDoor(w.rt, kOnBoost, true);
	CHECK(d == 1);
	CHECK(PredMpbhopsJumpDoor(w.rt, kOnBoost, false) < 0);
	CHECK(PredMpbhopsJumpDoor(w.rt, kOnDoor, true) < 0);
	const float onWaitZero[3] = { 464, 0, 0.03125f }, above[3] = { 264, 0, 2.5f };
	CHECK(PredMpbhopsJumpDoor(w.rt, onWaitZero, true) < 0);
	CHECK(PredMpbhopsJumpDoor(w.rt, above, true) < 0);

	// SetGroundEntity(NULL) left basevelocity z at the door's 0; the boost adds to it.
	w.p.baseVel[0] = 5.0f;
	PredMpbhopsJump(w.rt, d, w.p.baseVel);
	CHECK_NEAR(w.p.baseVel[2], 400.0, 1e-6);
	CHECK_NEAR(w.p.baseVel[0], 5.0, 1e-6);
	// Not FL_BASEVELOCITY: the next command's CheckMovingGround turns it into velocity.
	const float air[3] = { 264, 0, 10 };
	w.Cmd(air);
	CHECK_NEAR(w.p.velocity[2], 400.0 * 1.005, 1e-3);
	CHECK(w.p.baseVel[2] == 0.0f);
}

// Teleported onto the sinking door, the next command's PlayerMove CategorizePosition
// (game code moved the player) puts it on the door and it jumps straight off. That touch counts,
// so landing back 74 commands later teleports.
static void TestMpbhopsJumpAfterTeleport()
{
	const float landed[3] = { 0, 0, 1 };
	for (int moved = 0; moved < 2; ++moved) {
		World w(MpbhopsMap());
		w.rt.mpbhops = true;
		memcpy(w.p.origin, landed, sizeof landed);
		w.p.moveFlags = moved ? 1 | 2 : 1;
		w.ground = kDoorHandle;
		w.Cmd(kInAir, true, true);
		w.p.moveFlags = 1;
		// Unmoved, the ground at the jump is last command's, touched then.
		CHECK(w.rt.world.mpbhopsBlock == (moved ? 0 : -1));
		CHECK(MpbhopsStand(w, 73, kInAir) < 0);
		const int landTick = w.tick;
		CHECK(MpbhopsStand(w, 1) == (moved ? landTick : -1));
	}
	{
		// Found before and after the move, it's still one touch list entry: the teleport doesn't
		// start the door over.
		World w(MpbhopsMap());
		w.rt.mpbhops = true;
		MpbhopsStand(w, 1);
		MpbhopsStand(w, 20, kInAir);
		memcpy(w.p.origin, kOnDoor, sizeof kOnDoor);
		w.p.moveFlags = 1 | 2;
		w.ground = kDoorHandle;
		w.Cmd(kOnDoor, true, true);
		CHECK(w.p.origin[0] == 1000.0f && w.rt.world.mpbhopsBlock == -1);
	}
}

static void TestMpbhopsOff()
{
	World w(MpbhopsMap());
	CHECK(MpbhopsStand(w, 200) < 0);
	CHECK(w.rt.world.mpbhopsBlock == -1);
	CHECK(PredMpbhopsJumpDoor(w.rt, kOnBoost, true) < 0);
}

// State sync: A a trigger_multiple, B adds to the counter and renames the player, C a
// trigger_once, and two relays sharing a hammerid.
static PredMap* SyncMap()
{
	return BoxMap({ kBox, { { 200, -64, 0 }, { 328, 64, 96 } }, { { 400, -64, 0 }, { 528, 64, 96 } } }, { WorldEnt(),
		Ent({ { "classname", "trigger_multiple" }, { "model", "*1" }, { "spawnflags", "1" }, { "wait", "1" },
			{ "hammerid", "11" }, { "targetname", "a" }, { "OnEndTouch", "!activator,AddOutput,gravity 0.75,0,-1" } }),
		Ent({ { "classname", "trigger_multiple" }, { "model", "*2" }, { "spawnflags", "1" }, { "hammerid", "12" },
			{ "OnStartTouch", "cnt,Add,1,0,-1" }, { "OnStartTouch", "!activator,AddOutput,targetname bob,0,-1" } }),
		Ent({ { "classname", "math_counter" }, { "targetname", "cnt" }, { "hammerid", "13" } }),
		Ent({ { "classname", "trigger_once" }, { "model", "*3" }, { "spawnflags", "1" }, { "hammerid", "14" },
			{ "targetname", "once" } }),
		Ent({ { "classname", "logic_relay" }, { "hammerid", "20" } }),
		Ent({ { "classname", "logic_relay" }, { "hammerid", "20" } }) });
}

static const float kAway[3] = { -500, 0, 1 }, kInB[3] = { 264, 0, 1 }, kInC[3] = { 464, 0, 1 };

struct SyncWorld : World {
	SyncWorld() : World(SyncMap()) { rt.stateSync = true; }
	const PredWorld* Snap(int cmd) const
	{
		const int slot = ((cmd % PRED_RING_SIZE) + PRED_RING_SIZE) % PRED_RING_SIZE;
		return rt.ringKey[slot] == cmd ? &rt.ring[slot] : nullptr;
	}
	// Re-predicts from `cmd`, the game having restored the player to `origin`.
	void Replay(int cmd, const float origin[3])
	{
		command = cmd;
		memcpy(p.origin, origin, sizeof p.origin);
	}
};

// The server disabled A after 5001 (something the sim doesn't model). The link we predicted ends
// without OnEndTouch, since the server ended it when it happened.
static void TestSyncDisable()
{
	SyncWorld w;
	const int a = w.Sim("a");
	for (int i = 0; i < 5; ++i) w.Cmd(kIn); // 5000..5004
	CHECK(w.rt.world.ents[a].linked && w.Count(ON_TRIGGER, a) == 1);
	CHECK(PredSyncEntity(w.rt, 5001, 11, PRED_SYNC_DISABLED, 0.0f, -1) == 1);
	CHECK(w.Snap(5000)->ents[a].disabled == 0 && w.Snap(5000)->ents[a].linked);
	for (int c = 5001; c <= 5004; ++c) CHECK(w.Snap(c)->ents[a].disabled == 1 && !w.Snap(c)->ents[a].linked);
	CHECK(w.rt.world.ents[a].disabled == 1 && !w.rt.world.ents[a].linked && w.rt.world.linkedList.empty());
	CHECK(PredSyncEntity(w.rt, 5001, 11, PRED_SYNC_DISABLED, 0.0f, -1) == 0);
	w.Replay(5003, kIn);
	w.Cmd(kIn);
	w.Cmd(kIn);
	CHECK(w.rt.world.ents[a].disabled == 1 && !w.rt.world.ents[a].linked);
	CHECK(w.Count(ON_TRIGGER, a) == 1);
	// No OnEndTouch: its gravity would show from the next command.
	w.Cmd(kIn);
	CHECK_NEAR(w.p.gravity, 1.0, 1e-6);
}

static void TestSyncRemovedOnce()
{
	SyncWorld w;
	const int c = w.Sim("once");
	for (int i = 0; i < 4; ++i) w.Cmd(kAway);
	// The other bits of a removed entity mean nothing.
	CHECK(PredSyncEntity(w.rt, 5002, 14, PRED_SYNC_REMOVED | PRED_SYNC_DISABLED, 0.0f, -1) == 1);
	CHECK(!w.Snap(5001)->ents[c].removed && w.Snap(5002)->ents[c].removed && w.Snap(5003)->ents[c].removed);
	CHECK(w.rt.world.ents[c].removed && !w.rt.world.ents[c].disabled);
	w.Cmd(kInC);
	w.Cmd(kInC);
	CHECK(w.Count(LINK_START, c) == 0 && w.Count(ON_TRIGGER, c) == 0);
	// Respawned by a round restart.
	CHECK(PredSyncEntity(w.rt, w.command - 1, 14, 0, 0.0f, -1) == 1);
	w.Cmd(kInC);
	CHECK(w.Count(LINK_START, c) == 1 && w.Count(ON_TRIGGER, c) == 1);
}

// The counter was 5 on the server after 5001. Our own touch of B at 5003 added 1 from our 0 at
// 5004, which is left alone; re-predicting 5003 from the corrected snapshot gets 6.
static void TestSyncCounter()
{
	SyncWorld w;
	const int cnt = w.Sim("cnt");
	w.Cmd(kAway);
	w.Cmd(kAway);
	w.Cmd(kAway);
	w.Cmd(kInB);  // 5003
	w.Cmd(kInB);  // 5004: the Add runs
	CHECK(w.rt.world.ents[cnt].counterValue == 1.0f);
	CHECK(PredSyncEntity(w.rt, 5001, 13, 0, 5.0f, -1) == 1);
	CHECK(w.Snap(5000)->ents[cnt].counterValue == 0.0f);
	CHECK(w.Snap(5001)->ents[cnt].counterValue == 5.0f && w.Snap(5003)->ents[cnt].counterValue == 5.0f);
	CHECK(w.Snap(5004)->ents[cnt].counterValue == 1.0f && w.rt.world.ents[cnt].counterValue == 1.0f);
	w.Replay(5003, kAway);
	w.Cmd(kInB);
	w.Cmd(kInB);
	CHECK(w.rt.world.ents[cnt].counterValue == 6.0f);
}

// The server's targetname, classname, FL_BASEVELOCITY and gravity after 5001. Our rename at 5004 is
// kept. The gravity is set again at the start of the next command, since the game restores its own.
static void TestSyncPlayer()
{
	SyncWorld w;
	w.Cmd(kAway);
	w.Cmd(kAway);
	w.Cmd(kAway);
	w.Cmd(kInB);  // 5003
	w.Cmd(kInB);  // 5004: renamed bob
	CHECK(w.rt.world.playerName == "bob");
	CHECK(PredSyncPlayer(w.rt, 5001, 0.5f, true, "alice", "player_x") == 4);
	CHECK(w.Snap(5000)->playerName.empty() && w.Snap(5000)->gravity == 1.0f && !w.Snap(5000)->gravityFix);
	for (int c = 5001; c <= 5003; ++c) {
		const PredWorld& s = *w.Snap(c);
		CHECK(s.playerName == "alice" && s.playerClass == "player_x" && s.baseVelFlag && s.gravity == 0.5f && s.gravityFix);
	}
	CHECK(w.Snap(5004)->playerName == "bob" && w.rt.world.playerName == "bob" && w.rt.world.playerClass == "player_x");
	CHECK(PredSyncPlayer(w.rt, 5001, 0.5f, true, "alice", "player_x") == 0);

	w.Cmd(kInB);  // 5005, no replay: the live world's gravity
	CHECK_NEAR(w.p.gravity, 0.5, 1e-6);
	CHECK(w.Snap(5005)->gravity == 0.5f && !w.Snap(5005)->gravityFix);
	w.Replay(5002, kAway);
	w.p.gravity = 1.0f;
	w.Cmd(kAway);
	CHECK_NEAR(w.p.gravity, 0.5, 1e-6);
	// A checkpoint restore sets its own gravity for that command.
	w.Replay(5002, kAway);
	w.p.gravity = 0.8f;
	w.rt.cpTeleportPending = true;
	w.Cmd(kAway);
	CHECK_NEAR(w.p.gravity, 0.8, 1e-6);
}

// eventqueuefix: 3 more commands after 5001 can't activate A, so 5005 is the first that fires it.
static void TestSyncMultiWait()
{
	SyncWorld w;
	const int a = w.Sim("a");
	w.Cmd(kAway);
	w.Cmd(kAway);
	CHECK(PredSyncEntity(w.rt, 5001, 11, 0, 0.0f, 0) == 0);
	CHECK(PredSyncEntity(w.rt, 5001, 11, 0, 0.0f, 3) == 1);
	CHECK(w.rt.world.ents[a].multiReadyTick == 5005);
	int fired = -1;
	for (int i = 0; i < 4 && fired < 0; ++i) {
		w.Cmd(kIn);
		if (w.Count(ON_TRIGGER, a)) fired = w.command - 1;
	}
	CHECK(fired == 5005);
}

// A command older than the ring patches the oldest snapshot and carries on from there.
static void TestSyncOlderThanRing()
{
	SyncWorld w;
	const int cnt = w.Sim("cnt");
	for (int i = 0; i < PRED_RING_SIZE + 44; ++i) w.Cmd(kAway);
	const int latest = w.command - 1, oldest = latest - PRED_RING_SIZE + 1;
	CHECK(w.Snap(oldest) && !w.Snap(oldest - 1));
	CHECK(PredSyncEntity(w.rt, 5000, 13, 0, 2.0f, -1) == 1);
	CHECK(w.Snap(oldest)->ents[cnt].counterValue == 2.0f && w.Snap(latest)->ents[cnt].counterValue == 2.0f);
	CHECK(w.rt.world.ents[cnt].counterValue == 2.0f);
	// Ahead of the sim: only the live world.
	CHECK(PredSyncEntity(w.rt, latest + 10, 13, 0, 3.0f, -1) == 1);
	CHECK(w.Snap(latest)->ents[cnt].counterValue == 2.0f && w.rt.world.ents[cnt].counterValue == 3.0f);
}

// Hammerids the sim doesn't know (none, unknown, or shared) and a server without CAPS_STATE
// change nothing.
static void TestSyncIgnored()
{
	SyncWorld w;
	const int a = w.Sim("a");
	w.Cmd(kAway);
	w.Cmd(kAway);
	CHECK(PredSyncEntity(w.rt, 5001, 999, PRED_SYNC_DISABLED, 0.0f, -1) == -1);
	CHECK(PredSyncEntity(w.rt, 5001, 0, PRED_SYNC_DISABLED, 0.0f, -1) == -1);
	CHECK(PredSyncEntity(w.rt, 5001, 20, PRED_SYNC_DISABLED, 0.0f, -1) == -1);
	w.rt.stateSync = false;
	CHECK(PredSyncEntity(w.rt, 5001, 11, PRED_SYNC_DISABLED, 0.0f, -1) == -1);
	CHECK(PredSyncPlayer(w.rt, 5001, 0.5f, true, "alice", "x") == -1);
	for (const PredWorld* s : { w.Snap(5000), w.Snap(5001), (const PredWorld*)&w.rt.world })
		for (const PredEntState& st : s->ents) CHECK(!st.disabled);
	CHECK(w.rt.world.playerName.empty() && w.rt.world.gravity == 1.0f && !w.rt.world.gravityFix);
	w.Cmd(kIn);
	CHECK(w.Count(ON_TRIGGER, a) == 1);
}

// bhop_murder: a trigger opens the door, which turns off the teleport behind it once fully open
// and back on as it starts closing. 128 units at 128/s is 100 commands at 100 tick, wait 0.5 is 50.
static PredMap* DoorMap(const char* doorFlags = "0")
{
	return BoxMap({ kBox, { { 1000, -64, 0 }, { 1128, 64, 96 } }, { { 2000, -64, 0 }, { 2128, 64, 96 } } }, { WorldEnt(),
		Ent({ { "classname", "trigger_multiple" }, { "model", "*1" }, { "spawnflags", "1" }, { "wait", "-1" },
			{ "OnStartTouch", "d1,Open,,0,-1" } }),
		Ent({ { "classname", "func_door" }, { "model", "*2" }, { "targetname", "d1" }, { "hammerid", "30" },
			{ "spawnflags", doorFlags }, { "movedir", "0 0 0" }, { "speed", "128" }, { "wait", "0.5" },
			{ "OnFullyOpen", "d1_t,Disable,,0,-1" }, { "OnClose", "d1_t,Enable,,0,-1" } }),
		Ent({ { "classname", "trigger_teleport" }, { "model", "*3" }, { "targetname", "d1_t" }, { "spawnflags", "1" } }) });
}

static void TestDoorOpensAndReturns()
{
	World w(DoorMap());
	const int d = w.Sim("d1"), t = w.Sim("d1_t");
	w.Cmd(kIn); // 5000; the Open runs at the start of 5001
	w.Cmd(kAway);
	CHECK(w.rt.world.ents[d].doorState == PDS_UP && w.rt.world.ents[d].doorDoneTick == 5101);
	while (w.command < 5101) w.Cmd(kAway);
	CHECK(w.rt.world.ents[d].doorState == PDS_UP && !w.rt.world.ents[t].disabled);
	w.Cmd(kAway); // 5101
	CHECK(w.rt.world.ents[d].doorState == PDS_TOP && w.rt.world.ents[t].disabled);
	while (w.command < 5151) w.Cmd(kAway);
	CHECK(w.rt.world.ents[d].doorState == PDS_TOP && w.rt.world.ents[t].disabled);
	w.Cmd(kAway); // 5151
	CHECK(w.rt.world.ents[d].doorState == PDS_DOWN && !w.rt.world.ents[t].disabled);
	CHECK(w.rt.world.ents[d].doorDoneTick == 5251);
}

static void TestDoorLocked()
{
	World w(DoorMap("2048"));
	const int d = w.Sim("d1");
	w.Cmd(kIn);
	w.Cmd(kAway);
	CHECK(w.rt.world.ents[d].disabled && w.rt.world.ents[d].doorState == PDS_BOTTOM);
}

// +use isn't predicted: the server says the door started opening in 5002's frame and has 10 frames to go.
// "use opens": +use going down in front of the door opens it on that command, before its move. The
// door's face is at x 1000, the eye 64 up.
static bool g_blockUse;
static bool BlockedUseTrace(const float*, const float*, float, float* fraction)
{
	*fraction = 0.01f;
	return g_blockUse;
}

static void TestDoorUse()
{
	const int IN_USE = 1 << 5;
	static const float kNear[3] = { 950, 0, 0 }, kFar[3] = { 900, 0, 0 };
	{
		World w(DoorMap("256"));
		const int d = w.Sim("d1"), t = w.Sim("d1_t");
		memcpy(w.p.origin, kNear, sizeof kNear);
		w.buttons = IN_USE;
		w.Cmd(kNear); // 5000
		CHECK(w.rt.world.ents[d].doorState == PDS_UP && w.rt.world.ents[d].doorDoneTick == 5100);
		// Held: nothing more, and the door opens on time.
		while (w.command < 5100) w.Cmd(kNear);
		CHECK(w.rt.world.ents[d].doorState == PDS_UP && !w.rt.world.ents[t].disabled);
		w.Cmd(kNear); // 5100
		CHECK(w.rt.world.ents[d].doorState == PDS_TOP && w.rt.world.ents[t].disabled);
	}
	// Too far for the trace (100 units) and the radius search.
	{
		World w(DoorMap("256"));
		memcpy(w.p.origin, kFar, sizeof kFar);
		w.buttons = IN_USE;
		w.Cmd(kFar);
		CHECK(w.rt.world.ents[w.Sim("d1")].doorState == PDS_BOTTOM);
	}
	// Facing away, and 90 degrees off: the radius search wants a dot of 0.8.
	for (float yaw : { 180.0f, 90.0f }) {
		World w(DoorMap("256"));
		memcpy(w.p.origin, kNear, sizeof kNear);
		w.view[1] = yaw;
		w.buttons = IN_USE;
		w.Cmd(kNear);
		CHECK(w.rt.world.ents[w.Sim("d1")].doorState == PDS_BOTTOM);
	}
	// 30 degrees off the view is still the radius search's.
	{
		World w(DoorMap("256"));
		memcpy(w.p.origin, kNear, sizeof kNear);
		w.view[1] = 30.0f;
		w.view[0] = 40.0f;
		w.buttons = IN_USE;
		w.Cmd(kNear);
		CHECK(w.rt.world.ents[w.Sim("d1")].doorState == PDS_UP);
	}
	// Not a use door, locked, or behind something.
	for (const char* flags : { "0", "2304" }) {
		World w(DoorMap(flags));
		memcpy(w.p.origin, kNear, sizeof kNear);
		w.buttons = IN_USE;
		w.Cmd(kNear);
		CHECK(w.rt.world.ents[w.Sim("d1")].doorState == PDS_BOTTOM);
	}
	{
		World w(DoorMap("256"));
		w.rt.useTrace = BlockedUseTrace;
		g_blockUse = true;
		memcpy(w.p.origin, kNear, sizeof kNear);
		w.buttons = IN_USE;
		w.Cmd(kNear);
		CHECK(w.rt.world.ents[w.Sim("d1")].doorState == PDS_BOTTOM);
		g_blockUse = false;
	}
	// Already held when the command starts.
	{
		World w(DoorMap("256"));
		memcpy(w.p.origin, kNear, sizeof kNear);
		w.buttons = w.lastButtons = IN_USE;
		w.Cmd(kNear);
		CHECK(w.rt.world.ents[w.Sim("d1")].doorState == PDS_BOTTOM);
	}
}

static void TestDoorSync()
{
	World w(DoorMap());
	w.rt.stateSync = true;
	const int d = w.Sim("d1"), t = w.Sim("d1_t");
	for (int i = 0; i < 5; ++i) w.Cmd(kAway); // 5000..5004
	CHECK(PredSyncEntity(w.rt, 5002, 30, PDS_UP << PRED_SYNC_DOOR_SHIFT, 0.0f, 10) == 2);
	CHECK(w.rt.world.ents[d].doorState == PDS_UP && w.rt.world.ents[d].doorDoneTick == 5012);
	while (w.command < 5012) w.Cmd(kAway);
	CHECK(!w.rt.world.ents[t].disabled);
	w.Cmd(kAway);
	CHECK(w.rt.world.ents[d].doorState == PDS_TOP && w.rt.world.ents[t].disabled);
	// Locked at the bottom, nothing pending.
	CHECK(PredSyncEntity(w.rt, w.command - 1, 30, PRED_SYNC_DISABLED | (PDS_BOTTOM << PRED_SYNC_DOOR_SHIFT), 0.0f, -1) == 3);
	CHECK(w.rt.world.ents[d].disabled && w.rt.world.ents[d].doorDoneTick == INT_MAX);
}

// The SourceJump landfix's boosters, touched from the unlifted position while a lift lasts.
static void TestSjBoosters()
{
	const char* boost = "!activator,AddOutput,basevelocity 0 0 500,0,-1";
	BspModel thin = {}, thick = {};
	thin.maxs[2] = 2.0f;
	thick.maxs[2] = 4.0f;  // 6 between m_vecMins and m_vecMaxs
	const PredMapEntity multi = Ent({ { "classname", "trigger_multiple" }, { "OnStartTouch", boost } });
	CHECK(PredSjBooster(multi, PC_MULTIPLE, thin));
	CHECK(!PredSjBooster(multi, PC_MULTIPLE, thick));
	CHECK(!PredSjBooster(Ent({ { "OnStartTouch", boost }, { "parentname", "train" } }), PC_MULTIPLE, thin));
	CHECK(PredSjBooster(Ent({ { "OnEndTouch", "!activator\x1b" "AddOutput\x1b" " basevelocity 0 400 0 \x1b" "0\x1b" "-1" } }), PC_MULTIPLE, thin));
	CHECK(!PredSjBooster(Ent({ { "OnStartTouch", "!activator,AddOutput,basevelocity 0 0 500,0,1" } }), PC_MULTIPLE, thin));
	CHECK(!PredSjBooster(Ent({ { "OnStartTouch", "!activator,AddOutput,BASEVELOCITY 0 0 0,0,-1" } }), PC_MULTIPLE, thin));
	CHECK(!PredSjBooster(Ent({ { "OnTrigger", boost } }), PC_MULTIPLE, thin));
	CHECK(PredSjBooster(Ent({ { "pushdir", "0 90 0" } }), PC_PUSH, thin));
	CHECK(PredSjBooster(Ent({ { "pushdir", "90 0 0" } }), PC_PUSH, thin));     // down
	CHECK(!PredSjBooster(Ent({ { "pushdir", "-90 0 0" } }), PC_PUSH, thin));   // up keeps the lifted touches
	CHECK(!PredSjBooster(Ent({ { "pushdir", "-30 0 0" } }), PC_PUSH, thin));
	CHECK(!PredSjBooster(Ent({ { "pushdir", "0 0 0" }, { "spawnflags", "129" } }), PC_PUSH, thin));  // once

	// Feet at 2.5 lifted 1.5: a floor booster under them is touched, one at head height isn't; a thick
	// trigger is touched where the player is.
	const std::vector<Box> boxes = {
		{ { -64, -64, 0 }, { 64, 64, 2 } },
		{ { -64, -64, 63.5f }, { 64, 64, 65.5f } },
		{ { -64, -64, 0 }, { 64, 64, 4 } },
	};
	const std::vector<PredMapEntity> ents = {
		WorldEnt(),
		Ent({ { "classname", "trigger_multiple" }, { "model", "*1" }, { "targetname", "floor" }, { "spawnflags", "1" }, { "OnStartTouch", boost } }),
		Ent({ { "classname", "trigger_push" }, { "model", "*2" }, { "targetname", "head" }, { "spawnflags", "1" }, { "pushdir", "0 0 0" } }),
		Ent({ { "classname", "trigger_multiple" }, { "model", "*3" }, { "targetname", "thick" }, { "spawnflags", "1" }, { "OnStartTouch", boost } }),
	};
	const float feet[3] = { 0, 0, 2.5f };
	World w(BoxMap(boxes, ents));
	const int fl = w.Sim("floor"), hd = w.Sim("head"), th = w.Sim("thick");
	CHECK(w.rt.map->sim[fl].sjBooster && w.rt.map->sim[hd].sjBooster && !w.rt.map->sim[th].sjBooster);
	w.rt.world.landfixLift = 1.5f;
	w.Cmd(feet);
	CHECK(w.rt.world.ents[fl].linked && !w.rt.world.ents[hd].linked && w.rt.world.ents[th].linked);
	// The lift ends after the command's touches.
	w.rt.landfixEndsLift = true;
	w.Cmd(feet);
	CHECK(w.rt.world.landfixLift == 0.0f && w.rt.world.ents[fl].linked);
	w.rt.landfixEndsLift = false;
	w.Cmd(feet);
	CHECK(!w.rt.world.ents[fl].linked && w.rt.world.ents[hd].linked);
}

int main()
{
	TestSjBoosters();
	TestEveryCommandAFrame();
	TestBatchedFrame();
	TestInOutOneFrame();
	TestScrollSpam();
	TestDefaultFrameEnd();
	TestUntouchEachCommand();
	TestGroundHandleCleared();
	TestTeleportStaleLinks();
	TestBranchToggle();
	TestKillEndsTouch();
	TestCounterDivideByZero();
	TestTeleportWithoutDestination();
	TestFilterNegatedText();
	TestSnapshotByCommand();
	TestMpbhopsDoors();
	TestMpbhopsSinkingDelay();
	TestMpbhopsCooldown();
	TestMpbhopsBoost();
	TestMpbhopsJumpAfterTeleport();
	TestMpbhopsOff();
	TestSyncDisable();
	TestSyncRemovedOnce();
	TestSyncCounter();
	TestSyncPlayer();
	TestSyncMultiWait();
	TestSyncOlderThanRing();
	TestSyncIgnored();
	TestDoorOpensAndReturns();
	TestDoorLocked();
	TestDoorSync();
	TestDoorUse();
	printf("trigger: %d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
