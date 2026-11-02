// shavit-syncstyle's autosync (OnPlayerRunCmd) and the tracker that replays it.
#include "autosync.h"

#include <cstdio>

static int g_fail = 0, g_pass = 0;

#define CHECK(cond) do { if (cond) ++g_pass; else { ++g_fail; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static const int JUMP = AS_IN_JUMP, LEFT = AS_IN_MOVELEFT, RIGHT = AS_IN_MOVERIGHT;
static const AsPlayer AIR = { false, 2, 0 };
static const AsPlayer GROUND = { true, 2, 0 };

static int g_traces;
static bool g_surf;
static bool Surf() { ++g_traces; return g_surf; }

static AsMove Move(float yaw, int buttons = 0, float side = 0.0f)
{
	AsMove m = { { 5.0f, yaw, 0.0f }, side, buttons };
	return m;
}

static AsState State(float yaw, int groundTicks = 0)
{
	AsState st = { { 1.0f, yaw, 0.0f }, groundTicks, true };
	return st;
}

static void TestAngleDiff()
{
	CHECK(AsAngleDiff(10.0f, 5.0f) == 5.0f);
	CHECK(AsAngleDiff(-179.0f, 179.0f) == 2.0f);
	CHECK(AsAngleDiff(179.0f, -179.0f) == -2.0f);
	CHECK(AsAngleDiff(180.0f, 0.0f) == -180.0f);
}

static void TestApply()
{
	// Turning left (yaw up) in the air: A, with the last command's angles.
	AsState st = State(10.0f);
	AsMove m = Move(12.0f, RIGHT, 400.0f);
	CHECK(AsApply(st, m, AIR, 100, Surf));
	CHECK(m.side == -400.0f && m.buttons == LEFT);
	CHECK(m.angles[0] == 1.0f && m.angles[1] == 10.0f);
	CHECK(st.angles[0] == 5.0f && st.angles[1] == 12.0f);

	// Right, across the -180/180 seam.
	st = State(-179.0f);
	m = Move(179.0f, LEFT | JUMP);
	AsApply(st, m, AIR, 100, Surf);
	CHECK(m.side == 400.0f && m.buttons == (RIGHT | JUMP));

	// Too small a turn: angles still swapped, keys left alone.
	st = State(10.0f);
	m = Move(10.005f, 0, 250.0f);
	CHECK(AsApply(st, m, AIR, 100, Surf));
	CHECK(m.side == 250.0f && m.buttons == 0 && m.angles[1] == 10.0f);

	// No angles yet: the command's own, so no turn.
	st = { { 0, 0, 0 }, 0, false };
	m = Move(90.0f);
	AsApply(st, m, AIR, 100, Surf);
	CHECK(m.side == 0.0f && m.angles[1] == 90.0f && st.known);
}

static void TestGround()
{
	// Landing with jump held: the first ground tick counts as air.
	AsState st = State(0.0f);
	AsMove m = Move(5.0f, JUMP);
	AsApply(st, m, GROUND, 100, Surf);
	CHECK(st.groundTicks == 0 && m.side == -400.0f);

	// Without jump it doesn't, nor a jump on the second tick.
	st = State(0.0f);
	m = Move(5.0f);
	AsApply(st, m, GROUND, 100, Surf);
	CHECK(st.groundTicks == 1 && m.side == 0.0f);
	m = Move(10.0f, JUMP);
	AsApply(st, m, GROUND, 100, Surf);
	CHECK(st.groundTicks == 2 && m.side == 0.0f && m.angles[1] == 5.0f);

	// Jump held on the ground keeps it at 0; a tick without it starts the count.
	m = Move(15.0f);
	AsApply(st, m, AIR, 100, Surf);
	CHECK(st.groundTicks == 0);
	m = Move(20.0f, JUMP);
	AsApply(st, m, GROUND, 100, Surf);
	m = Move(25.0f, JUMP);
	AsApply(st, m, GROUND, 100, Surf);
	CHECK(st.groundTicks == 0 && m.side == -400.0f);
	m = Move(30.0f);
	AsApply(st, m, GROUND, 100, Surf);
	CHECK(st.groundTicks == 1 && m.side == 0.0f);
	m = Move(35.0f, JUMP);
	AsApply(st, m, GROUND, 100, Surf);
	CHECK(st.groundTicks == 2 && m.side == 0.0f);
}

static void TestSkips()
{
	const AsPlayer skipped[] = { { false, AS_MOVETYPE_NOCLIP, 0 }, { false, AS_MOVETYPE_LADDER, 0 },
		{ false, AS_MOVETYPE_NONE, 0 }, { false, 2, 2 }, { false, 2, 3 } };
	for (const AsPlayer& p : skipped) {
		AsState st = State(0.0f, 7);
		AsMove m = Move(20.0f);
		CHECK(!AsApply(st, m, p, 100, Surf));
		CHECK(m.angles[1] == 20.0f && m.side == 0.0f && st.angles[1] == 0.0f && st.groundTicks == 7);
	}
	// Feet in water still count.
	AsState st = State(0.0f);
	AsMove m = Move(20.0f);
	CHECK(AsApply(st, m, { false, 2, 1 }, 100, Surf));
	CHECK(m.side == -400.0f);

	// The teleport count and the surf trace, in the plugin's order.
	g_traces = 0;
	g_surf = true;
	st = State(0.0f);
	m = Move(20.0f);
	AsApply(st, m, AIR, 8, Surf);
	CHECK(m.side == 0.0f && m.angles[1] == 0.0f && g_traces == 0);
	m = Move(40.0f);
	AsApply(st, m, AIR, 9, Surf);
	CHECK(m.side == 0.0f && g_traces == 1);
	m = Move(40.0f);
	AsApply(st, m, AIR, 9, Surf);
	CHECK(g_traces == 1);
	g_surf = false;
	m = Move(60.0f);
	AsApply(st, m, AIR, 9, Surf);
	CHECK(m.side == -400.0f && g_traces == 2);
}

// Runs commands from..to turning left 1 degree each in the air; returns how many got the sidemove.
static int Turn(AsTracker& t, int from, int to, const AsPlayer& p = AIR)
{
	int forced = 0;
	for (int c = from; c <= to; ++c) {
		AsMove m = Move((float)c);
		t.Apply(c, m, p, INT_MIN, Surf);
		if (m.side == -400.0f) ++forced;
	}
	return forced;
}

static bool Forced(AsTracker& t, int cmd, int zeroNow = INT_MIN)
{
	AsMove m = Move((float)cmd);
	t.Apply(cmd, m, AIR, zeroNow, Surf);
	return m.side == -400.0f;
}

static void TestTracker()
{
	g_surf = false;
	AsTracker t;
	CHECK(t.OrderFor(5) == AS_BEFORE_BHOPTOOLS);
	CHECK(Turn(t, 1, 10) == 0);  // no report yet

	t.AddReport(10, true, AS_AFTER_BHOPTOOLS, 0);
	CHECK(t.OrderFor(10) == AS_BEFORE_BHOPTOOLS && t.OrderFor(11) == AS_AFTER_BHOPTOOLS);
	AsMove m = Move(11.0f);
	t.Apply(11, m, AIR, INT_MIN, Surf);
	CHECK(m.side == 0.0f && m.angles[1] == 11.0f);  // off until now: no angles of its own yet
	CHECK(Turn(t, 12, 20) == 9);

	// A replay gives the same angles.
	m = Move(16.0f);
	t.Apply(16, m, AIR, INT_MIN, Surf);
	CHECK(m.angles[1] == 15.0f);

	// The server's teleport: 25 is the count's 0, so 25..33 don't get it and 34 does.
	t.AddReport(24, true, AS_AFTER_BHOPTOOLS, 25);
	CHECK(Turn(t, 25, 33) == 0);
	CHECK(Forced(t, 34));
	CHECK(Turn(t, 35, 39) == 5);
	// A ladder command in the window doesn't count.
	t.AddReport(39, true, AS_AFTER_BHOPTOOLS, 40);
	CHECK(Turn(t, 40, 44) == 0);
	CHECK(Turn(t, 45, 45, { false, AS_MOVETYPE_LADDER, 0 }) == 0);
	CHECK(Turn(t, 46, 49) == 0);
	CHECK(Forced(t, 50));

	// A teleport predicted during 60's move, after the report.
	CHECK(Turn(t, 51, 60) == 10);
	t.NoteTeleport(60);
	CHECK(Turn(t, 61, 69) == 0);
	CHECK(Forced(t, 70));
	// Replays of the window still see it; a report after it has the server's word instead.
	CHECK(!Forced(t, 65));
	t.AddReport(60, true, AS_AFTER_BHOPTOOLS, 40);
	CHECK(Forced(t, 65));

	// One before the plugin on the same command (bhoptools' teleports when it runs after them).
	CHECK(!Forced(t, 80, 80));
	CHECK(Turn(t, 81, 88) == 0);
	CHECK(Forced(t, 89));

	// Turned off: angles pass through, and the state waits for it to come back.
	t.AddReport(90, false, AS_AFTER_BHOPTOOLS, 40);
	m = Move(500.0f);
	t.Apply(91, m, AIR, INT_MIN, Surf);
	CHECK(m.angles[1] == 500.0f && m.side == 0.0f);
	t.AddReport(91, true, AS_BEFORE_CORE, 40);
	m = Move(92.0f);
	t.Apply(92, m, AIR, INT_MIN, Surf);
	CHECK(m.angles[1] == 89.0f);  // 89's, the last it ran on
	CHECK(t.OrderFor(92) == AS_BEFORE_CORE);

	// A late report turning it on rebuilds what came after it.
	AsTracker late;
	CHECK(Turn(late, 1, 20) == 0);
	late.AddReport(5, true, AS_BEFORE_BHOPTOOLS, 0);
	m = Move(21.0f);
	late.Apply(21, m, AIR, INT_MIN, Surf);
	CHECK(m.angles[1] == 20.0f && m.side == -400.0f);
	// ...and turning it off from 15 stops the state at 15.
	late.AddReport(15, false, AS_BEFORE_BHOPTOOLS, 0);
	late.AddReport(18, true, AS_BEFORE_BHOPTOOLS, 0);
	m = Move(19.0f);
	late.Apply(19, m, AIR, INT_MIN, Surf);
	CHECK(m.angles[1] == 15.0f);
}

int main()
{
	TestAngleDiff();
	TestApply();
	TestGround();
	TestSkips();
	TestTracker();
	printf("autosync: %d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
