// shavit zones (zones_sim.h) through the trigger sim, with values worked out from shavit-zones,
// shavit-core and shavit-misc 4.0.2a.
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
static const int WALK = 2, LADDER = 9;

static int Bits(float f)
{
	int v;
	memcpy(&v, &f, 4);
	return v;
}

// A box zone as the plugin sends it: corners (x0 y0 z0)-(x1 y1 z1), shrunk by shavit_zones_box_offset 1
// (16 on x and y, 31 on z, while the half size is bigger).
static ZsZone Box(int type, float x0, float y0, float z0, float x1, float y1, float z1, int track = 0, int data = 0)
{
	ZsZone z;
	z.type = type;
	z.track = track;
	z.data = data;
	const float lo[3] = { x0, y0, z0 }, hi[3] = { x1, y1, z1 };
	for (int c = 0; c < 3; ++c) {
		z.origin[c] = (lo[c] + hi[c]) * 0.5f;
		z.half[c] = fabsf(lo[c] - hi[c]) / 2.0f;
		const float sub = c < 2 ? 16.0f : 31.0f;
		if (z.half[c] > sub) z.half[c] -= sub;
	}
	return z;
}

static PredMap* EmptyMap()
{
	PredMap* m = new PredMap();
	BspModel world = {};
	m->models = { world };
	m->modelBrushes = { { 0, 0 } };
	m->loaded = true;
	PredBuildTriggers(*m);
	return m;
}

struct Sim {
	PredRuntime rt;
	float origin[3] = { 0, 0, 0 }, velocity[3] = { 0, 0, 0 }, baseVel[3] = { 0, 0, 0 };
	float gravity = 1.0f;
	int flags = FL_CLIENT_BIT | FL_ONGROUND;
	uint8_t moveFlags = 1;
	int moveType = WALK;
	int cmd = 100;

	Sim(const std::vector<ZsZone>& zones, const ZsSettings& s = ZsSettings())
	{
		PredAdopt(rt, EmptyMap());
		PredZonesSet(rt, zones, true);
		rt.zoneSettings.Add(INT_MIN, s);
	}
	~Sim() { PredReset(rt); }

	void Cmd(float x, float y, float z, bool ducked = false)
	{
		PredPlayerIO io;
		io.origin = origin;
		io.velocity = velocity;
		io.baseVelocity = baseVel;
		io.gravity = &gravity;
		io.flags = &flags;
		io.moveFlags = &moveFlags;
		io.tick = cmd;
		io.command = cmd++;
		io.moveType = moveType;
		io.solid = true;
		io.interval = DT;
		io.curtime = io.tick * DT;
		PredSetHull(io, (flags & FL_DUCKING) != 0);
		PredPreMove(rt, io);
		origin[0] = x;
		origin[1] = y;
		origin[2] = z;
		flags = ducked ? flags | FL_DUCKING : flags & ~FL_DUCKING;
		PredPostMove(rt, io);
	}

	bool In(int k) const { return rt.world.zones.in[k] != 0; }
	const ZsSettings& Settings() const { return rt.zoneSettings.For(INT_MIN); }
};

// (0 0 0)-(128 128 100): centre 64 64 50, half 48 48 19, so the trigger spans 16..112 and 31..69, a unit
// more each way in the partition. A standing hull (32x32x62) touches with its feet from x -1 to 129 and
// z -32 to 70; ducked (45 high) from z -15.
static void TestBoxEdges()
{
	const ZsZone z = Box(ZS_START, 0, 0, 0, 128, 128, 100);
	CHECK(z.half[0] == 48.0f && z.half[2] == 19.0f);
	const float stand[2][3] = { { -16, -16, 0 }, { 16, 16, 62 } };
	const float duck[2][3] = { { -16, -16, 0 }, { 16, 16, 45 } };
	auto at = [&](float x, float zz, bool ducked = false) {
		const float feet[3] = { x, 64, zz };
		return ZsBoxTouches(z, feet, ducked ? duck[0] : stand[0], ducked ? duck[1] : stand[1]);
	};
	CHECK(at(129.0f, 0));
	CHECK(!at(129.01f, 0));
	CHECK(at(-1.0f, 0));
	CHECK(!at(-1.01f, 0));
	CHECK(at(64, 70.0f));
	CHECK(!at(64, 70.01f));
	CHECK(at(64, -32.0f));
	CHECK(!at(64, -32.01f));
	CHECK(at(64, -15.0f, true));
	CHECK(!at(64, -15.01f, true));

	// A thin zone keeps its size: half 4 isn't more than 16.
	const ZsZone thin = Box(ZS_START, 0, 0, 0, 8, 128, 100);
	CHECK(thin.half[0] == 4.0f);
}

// Membership is set on the touch after the move and ends at the frame's untouch.
static void TestMembership()
{
	Sim s({ Box(ZS_START, 0, 0, 0, 128, 128, 100) });
	s.Cmd(-50, 64, 0);
	CHECK(!s.In(0));
	s.Cmd(64, 64, 0);
	CHECK(s.In(0));
	s.Cmd(200, 64, 0);
	CHECK(!s.In(0));
}

// TeleportEntity(destination) on StartTouchPost and again on TouchPost: the origin only, velocity kept,
// ground cleared. The next command, away from the zone, ends the touch.
static void TestTeleport()
{
	ZsZone t = Box(ZS_TELEPORT, 0, 0, 0, 128, 128, 100);
	t.dest[0] = 1000; t.dest[1] = 2000; t.dest[2] = 300;
	Sim s({ t });
	s.velocity[0] = 250;
	s.Cmd(64, 64, 0);
	CHECK(s.origin[0] == 1000 && s.origin[1] == 2000 && s.origin[2] == 300);
	CHECK(s.velocity[0] == 250);
	CHECK(!(s.flags & FL_ONGROUND));
	CHECK(s.rt.zoneTeleports == 2);
	CHECK(s.In(0));
	s.Cmd(1000, 2000, 299);
	CHECK(!s.In(0));
	CHECK(s.rt.zoneTeleports == 2);
}

// Gravity zones in PreThinkPost: the zone's float data while inside; leaving doesn't put it back unless
// the style has its own gravity and the player's is 1, 0, or they were on a ladder.
static void TestGravity()
{
	ZsSettings set;
	Sim s({ Box(ZS_GRAVITY, 0, 0, 0, 128, 128, 100, 0, Bits(0.5f)) }, set);
	s.Cmd(64, 64, 0);
	CHECK(s.gravity == 1.0f); // touched after this command's PreThink
	s.Cmd(64, 64, 0);
	CHECK(s.gravity == 0.5f);
	s.Cmd(300, 64, 0);
	s.Cmd(300, 64, 0);
	CHECK(!s.In(0));
	CHECK(s.gravity == 0.5f);

	set.gravity = 0.8f;
	Sim l({ Box(ZS_GRAVITY, 0, 0, 0, 128, 128, 100, 0, Bits(0.5f)) }, set);
	l.Cmd(64, 64, 0);
	l.Cmd(300, 64, 0);
	l.Cmd(300, 64, 0);
	CHECK(l.gravity == 0.5f);
	l.moveType = LADDER;
	l.Cmd(300, 64, 0);
	l.moveType = WALK;
	l.Cmd(300, 64, 0);
	CHECK(l.gravity == 0.8f);

	// No-timer-gravity leaves it alone even with a gravity zone overlapping. The style's gravity went on
	// before the zones were touched.
	Sim n({ Box(ZS_GRAVITY, 0, 0, 0, 128, 128, 100, 0, Bits(0.5f)), Box(ZS_NOTIMERGRAVITY, 0, 0, 0, 128, 128, 100) }, set);
	n.Cmd(64, 64, 0);
	CHECK(n.gravity == 0.8f);
	n.gravity = 1.0f;
	n.Cmd(64, 64, 0);
	CHECK(n.gravity == 1.0f);
}

// shavit_zones_enforcetracks: a zone past Zone_End on another track never sets membership; start zones
// of every track do.
static void TestTracks()
{
	ZsSettings set;
	set.flags = ZSP_ENFORCE;
	Sim s({ Box(ZS_GRAVITY, 0, 0, 0, 128, 128, 100, 1, Bits(0.5f)), Box(ZS_START, 0, 0, 0, 128, 128, 100, 1) }, set);
	s.Cmd(64, 64, 0);
	s.Cmd(64, 64, 0);
	CHECK(!s.In(0) && s.In(1));
	CHECK(s.gravity == 1.0f);
}

static ZsCmd Cmd(float vx, float vy, float vz, int buttons, bool onGround)
{
	ZsCmd c = { { vx, vy, vz }, buttons, 0.0f, onGround, false, false };
	return c;
}

static void TestNoJump()
{
	ZsSet set;
	set.active = true;
	set.zones = { Box(ZS_NOJUMP, 0, 0, 0, 128, 128, 100), Box(ZS_NOJUMP, 0, 0, 0, 128, 128, 100, 1) };
	ZsSettings s;
	ZsState st;
	ZsCmd c = Cmd(0, 0, 0, ZS_IN_JUMP | 1, true);
	c.upmove = 320;
	ZsPreCmd(set, s, st, c);
	CHECK(c.buttons == (ZS_IN_JUMP | 1));
	st.in[1] = 1; // the bonus's: not this track's
	ZsPreCmd(set, s, st, c);
	CHECK(c.buttons == (ZS_IN_JUMP | 1));
	st.in[0] = 1;
	ZsPreCmd(set, s, st, c);
	CHECK(c.buttons == 1 && c.upmove == 0.0f);
}

// shavit_misc_prespeed 2: landing in the start zone with jump held zeroes the velocity.
static void TestPrespeedLanding()
{
	ZsSet set;
	set.active = true;
	set.zones = { Box(ZS_START, 0, 0, 0, 128, 128, 100) };
	ZsSettings s;
	s.prespeedType = 2;
	ZsState st;
	st.in[0] = 1;
	ZsCmd c = Cmd(300, 40, -5, ZS_IN_JUMP, true);
	ZsPreCmd(set, s, st, c);
	CHECK(c.velocitySet && c.velocity[0] == 0 && c.velocity[1] == 0 && c.velocity[2] == 0);
	c = Cmd(300, 40, -5, 0, true);
	ZsPreCmd(set, s, st, c);
	CHECK(!c.velocitySet && c.velocity[0] == 300);
	st.prevGround = true;
	c = Cmd(300, 40, 0, ZS_IN_JUMP, true);
	ZsPreCmd(set, s, st, c);
	CHECK(!c.velocitySet);
	// Style prespeed allows it.
	st.prevGround = false;
	s.prespeed = 1;
	c = Cmd(300, 40, -5, ZS_IN_JUMP, true);
	ZsPreCmd(set, s, st, c);
	CHECK(!c.velocitySet);
}

// shavit_misc_prespeed 1/3/4/5: the horizontal speed is scaled down to the limit, the start zone's own
// (its data) over runspeed + prestrafelimit, a third of it while moving up for types under 4. Type 5 keeps z.
static void TestPrestrafeLimit()
{
	ZsSet set;
	set.active = true;
	set.zones = { Box(ZS_START, 0, 0, 0, 128, 128, 100) };
	ZsSettings s;
	s.prespeedType = 1;
	s.limit = 290.0f;
	ZsState st;
	st.in[0] = 1;
	ZsCmd c = Cmd(400, 300, 0, 0, true);
	ZsPreCmd(set, s, st, c);
	CHECK_NEAR(c.velocity[0], 400 * (290.0f / 500.0f), 1e-4);
	CHECK_NEAR(c.velocity[1], 300 * (290.0f / 500.0f), 1e-4);

	st.startLimit = 300.0f;
	c = Cmd(400, 300, 0, 0, true);
	ZsPreCmd(set, s, st, c);
	CHECK_NEAR(c.velocity[0], 240, 1e-4);
	CHECK_NEAR(c.velocity[1], 180, 1e-4);

	c = Cmd(400, 300, 100, 0, false);
	ZsPreCmd(set, s, st, c);
	CHECK_NEAR(c.velocity[0], 80, 1e-4);
	CHECK_NEAR(c.velocity[2], 20, 1e-4);

	s.prespeedType = 5;
	c = Cmd(400, 300, 100, 0, false);
	ZsPreCmd(set, s, st, c);
	CHECK_NEAR(c.velocity[0], 240, 1e-4);
	CHECK_NEAR(c.velocity[2], 100, 1e-6);

	c = Cmd(100, 0, 0, 0, true);
	ZsPreCmd(set, s, st, c);
	CHECK(!c.velocitySet);
}

// Entering a start zone of the timer's track sets its limit (Shavit_OnEnterZone), and the zone's
// TouchPost runs StartTimer, whose Shavit_OnStartPre clamps the same way after the move.
static void TestStartTouchClamp()
{
	ZsSettings set;
	set.prespeedType = 1;
	set.limit = 290.0f;
	set.flags = ZSP_TEAM | ZSP_STOPPED;
	Sim s({ Box(ZS_START, 0, 0, 0, 128, 128, 100, 0, 300) }, set);
	s.velocity[0] = 400; s.velocity[1] = 300;
	s.Cmd(64, 64, 0);
	CHECK(s.rt.world.zones.startLimit == 300.0f);
	CHECK_NEAR(s.velocity[0], 240, 1e-4);

	// shavit_core_nozaxisspeed with any z speed: CanStartTimer refuses before the forward.
	set.noZAxisSpeed = 1;
	Sim z({ Box(ZS_START, 0, 0, 0, 128, 128, 100, 0, 300) }, set);
	z.velocity[0] = 400; z.velocity[2] = -10;
	z.Cmd(64, 64, 0);
	CHECK(z.velocity[0] == 400);
}

// With eventqueuefix, a start zone touched within 4 ticks of a teleport removes the touch list instead
// of starting the timer, so membership doesn't hold until the fifth command after.
static void TestStartAfterTeleport()
{
	ZsZone t = Box(ZS_TELEPORT, 0, 0, 0, 128, 128, 100);
	t.dest[0] = 1064; t.dest[1] = 64; t.dest[2] = 0;
	Sim s({ t, Box(ZS_START, 1000, 0, 0, 1128, 128, 100) });
	s.rt.eventQueueFix = true;
	s.Cmd(64, 64, 0);
	for (int i = 0; i < 4; ++i) {
		s.Cmd(1064, 64, 0);
		CHECK(!s.In(1));
	}
	s.Cmd(1064, 64, 0);
	CHECK(s.In(1));
}

static void TestCheckpointClears()
{
	Sim s({ Box(ZS_START, 0, 0, 0, 128, 128, 100) });
	s.Cmd(64, 64, 0);
	CHECK(s.In(0));
	s.rt.cpTeleportPending = true;
	s.Cmd(500, 500, 0);
	CHECK(!s.In(0));
}

static void TestReplay()
{
	Sim s({ Box(ZS_START, 0, 0, 0, 128, 128, 100) });
	s.Cmd(-50, 64, 0);
	const int before = s.cmd;
	s.Cmd(64, 64, 0);
	s.Cmd(64, 64, 0);
	CHECK(PredWorldBefore(s.rt, before + 1).zones.in[0] == 1);
	CHECK(PredWorldBefore(s.rt, before).zones.in[0] == 0);
}

static void TestParse()
{
	const char* f[] = { "7", "1", "0", "300", "0", "42800000", "-42800000", "0", "40000000", "40000000", "3f800000",
		"447a0000", "0", "-40c00000" };
	ZsZone z;
	CHECK(ZsParseZone(f, 14, z));
	CHECK(z.type == ZS_TELEPORT && z.track == 1 && z.data == 300);
	CHECK(z.origin[0] == 64.0f && z.half[0] == 2.0f && z.dest[0] == 1000.0f);
	CHECK(z.origin[1] == -0.0625f && z.dest[2] == -0.75f);
	CHECK(!ZsParseZone(f, 13, z));
}

static void TestParseList()
{
	const char* line = "7 1 0 300 0 42800000 -42800000 0 40000000 40000000 3f800000 447a0000 0 -40c00000";
	std::vector<ZsZone> zones;
	std::string text = std::string(line) + "\n" + "0 0 0 0 0 0 0 0 40000000 40000000 40000000 0 0 0" + "\n";
	CHECK(ZsParseList(text.data(), text.size(), zones));
	CHECK(zones.size() == 2 && zones[0].type == ZS_TELEPORT && zones[0].dest[0] == 1000.0f && zones[1].type == ZS_START);
	CHECK(ZsParseList("", 0, zones) && zones.empty());
	text += "7 1 0\n";
	CHECK(!ZsParseList(text.data(), text.size(), zones) && zones.empty());
	text = std::string(line) + "\n" + "99 0 0 0 0 0 0 0 0 0 0 0 0 0\n";
	CHECK(!ZsParseList(text.data(), text.size(), zones) && zones.empty());
	text.clear();
	for (int i = 0; i <= ZS_MAX; ++i) text += std::string(line) + "\n";
	CHECK(!ZsParseList(text.data(), text.size(), zones));
	text.resize(text.size() - strlen(line) - 1);
	CHECK(ZsParseList(text.data(), text.size(), zones) && (int)zones.size() == ZS_MAX);
}

int main()
{
	TestBoxEdges();
	TestMembership();
	TestTeleport();
	TestGravity();
	TestTracks();
	TestNoJump();
	TestPrespeedLanding();
	TestPrestrafeLimit();
	TestStartTouchClamp();
	TestStartAfterTeleport();
	TestCheckpointClears();
	TestReplay();
	TestParse();
	TestParseList();
	printf("zones: %d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
