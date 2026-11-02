// shavit-core's key blocking (OnPlayerRunCmd), with the styles as shavit-styles.cfg sets them.
#include "style_keys.h"

#include <cstdio>

static int g_fail = 0, g_pass = 0;

#define CHECK(cond) do { if (cond) ++g_pass; else { ++g_fail; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static const int SW     = SK_BLOCK_A | SK_BLOCK_D;
static const int WONLY  = SK_BLOCK_A | SK_BLOCK_S | SK_BLOCK_D;
static const int ADONLY = SK_AD_ONLY | SK_BLOCK_W | SK_BLOCK_S;
static const int SCROLL = SK_PLEFT | SK_PRIGHT;

static const int W = SK_IN_FORWARD, S = SK_IN_BACK, A = SK_IN_MOVELEFT, D = SK_IN_MOVERIGHT;
static const int WALK = 2, LADDER = SK_MOVETYPE_LADDER, NOCLIP = SK_MOVETYPE_NOCLIP;

static SkMove Keys(int buttons)
{
	SkMove m = { 0.0f, 0.0f, buttons };
	if (buttons & W) m.forward += 400.0f;
	if (buttons & S) m.forward -= 400.0f;
	if (buttons & D) m.side += 400.0f;
	if (buttons & A) m.side -= 400.0f;
	return m;
}

static SkMove Run(int flags, int buttons, bool onGround = false, int moveType = WALK, SkState* st = nullptr)
{
	SkState local = { false, -1 };
	SkMove m = Keys(buttons);
	SkApply(flags, st ? *st : local, m, onGround, moveType);
	return m;
}

static bool Is(const SkMove& m, float forward, float side, int buttons)
{
	return m.forward == forward && m.side == side && m.buttons == buttons;
}

static void TestSideways()
{
	CHECK(Is(Run(SW, W | A), 400, 0, W));
	CHECK(Is(Run(SW, S | D), -400, 0, S));
	CHECK(Is(Run(SW, W | S), 0, 0, W | S));
	CHECK(Is(Run(SW, A | D), 0, 0, 0));
	// Only in the air, unless the style forces it on the ground.
	CHECK(Is(Run(SW, W | A, true), 400, -400, W | A));
	CHECK(Is(Run(SW | SK_GROUNDKEYS, W | A, true), 400, 0, W));
	// A sidemove without the button (+strafe, a joystick) is blocked too.
	SkMove m = { 0.0f, 250.0f, 0 };
	SkState st = { false, -1 };
	SkApply(SW, st, m, false, WALK);
	CHECK(m.side == 0.0f);
	CHECK(Is(Run(SW, A | SK_IN_LEFT | (1 << 1)), 0, 0, SK_IN_LEFT | (1 << 1)));
}

static void TestWOnly()
{
	CHECK(Is(Run(WONLY, W), 400, 0, W));
	CHECK(Is(Run(WONLY, W | A), 400, 0, W));
	CHECK(Is(Run(WONLY, S), 0, 0, 0));
	CHECK(Is(Run(WONLY, W | S | A | D), 0, 0, W));
	// W and S held: forwardmove is 0, so block_s only clears IN_BACK.
	CHECK(Is(Run(WONLY, W | S), 0, 0, W));
}

static void TestADOnly()
{
	SkState st = { false, -1 };
	// W and S never; the first side pressed in the air is the one allowed.
	CHECK(Is(Run(ADONLY, W | A, false, WALK, &st), 0, -400, A));
	CHECK(st.combo == 0);
	CHECK(Is(Run(ADONLY, D, false, WALK, &st), 0, 0, 0));
	CHECK(Is(Run(ADONLY, A, false, WALK, &st), 0, -400, A));
	// Both held: sidemove 0 picks nothing and blocks nothing.
	CHECK(Is(Run(ADONLY, A | D, false, WALK, &st), 0, 0, A | D));
	// Nothing is decided on the ground.
	st = { false, -1 };
	CHECK(Is(Run(ADONLY, D, true, WALK, &st), 0, 400, D));
	CHECK(st.combo == -1);
	CHECK(Is(Run(ADONLY, D, false, WALK, &st), 0, 400, D));
	CHECK(st.combo == 1);
	CHECK(Is(Run(ADONLY, A, true, WALK, &st), 0, -400, A));
	CHECK(Is(Run(ADONLY, A, false, WALK, &st), 0, 0, 0));
}

static void TestHSW()
{
	const int HSW = SK_HSW;
	CHECK(Is(Run(HSW, W | A), 400, -400, W | A));
	CHECK(Is(Run(HSW, W | D), 400, 400, W | D));
	CHECK(Is(Run(HSW, W), 0, 0, 0));
	CHECK(Is(Run(HSW, A), 0, 0, 0));
	CHECK(Is(Run(HSW, D), 0, 0, 0));
	CHECK(Is(Run(HSW, S | A), 0, 0, 0));
	CHECK(Is(Run(HSW, S), -400, 0, S));
	CHECK(Is(Run(HSW, W, true), 400, 0, W));
}

static void TestSurfHSW()
{
	const int SHSW = SK_HSW | SK_SHSW;
	SkState st = { false, -1 };
	// Nothing valid yet: everything goes.
	CHECK(Is(Run(SHSW, W, false, WALK, &st), 0, 0, 0));
	CHECK(st.combo == -1);
	CHECK(Is(Run(SHSW, S | D, false, WALK, &st), -400, 400, S | D));
	CHECK(st.combo == 0);
	CHECK(Is(Run(SHSW, W | A, false, WALK, &st), 400, -400, W | A));
	CHECK(Is(Run(SHSW, W | D, false, WALK, &st), 0, 0, 0));
	CHECK(Is(Run(SHSW, A, false, WALK, &st), 0, 0, 0));
	// In the start zone it's plain HSW and picks nothing.
	st = { false, -1 };
	CHECK(Is(Run(SHSW | SK_INSTART, W | D, false, WALK, &st), 400, 400, W | D));
	CHECK(st.combo == -1);
	CHECK(Is(Run(SHSW | SK_INSTART, S, false, WALK, &st), -400, 0, S));
}

static void TestLeftRight()
{
	const int ON = SCROLL | SK_TIMER;
	CHECK(Is(Run(ON, W | A | SK_IN_LEFT), 0, 0, W | A | SK_IN_LEFT));
	CHECK(Is(Run(ON, W | SK_IN_RIGHT, true), 0, 0, W | SK_IN_RIGHT));
	CHECK(Is(Run(ON, W | A), 400, -400, W | A));
	// Not in the start zone, not without a running timer, not for a style that allows it.
	CHECK(Is(Run(ON | SK_INSTART, W | SK_IN_LEFT), 400, 0, W | SK_IN_LEFT));
	CHECK(Is(Run(SCROLL, W | SK_IN_LEFT), 400, 0, W | SK_IN_LEFT));
	CHECK(Is(Run(SK_PLEFT | SK_TIMER, W | SK_IN_RIGHT), 400, 0, W | SK_IN_RIGHT));
	// Without shavit-zones every move is zeroed while the timer runs.
	CHECK(Is(Run(SK_TIMER | SK_NOZONES, W | A), 0, 0, W | A));
	// Before the key blocks, which still clear the buttons.
	CHECK(Is(Run(SW | SK_PLEFT | SK_TIMER, W | A | SK_IN_LEFT), 0, 0, W | SK_IN_LEFT));
}

static void TestExemptions()
{
	CHECK(Is(Run(SW, W | A, false, NOCLIP), 400, -400, W | A));
	CHECK(Is(Run(SW, W | A, false, LADDER), 400, -400, W | A));
	CHECK(Is(Run(SW | SK_FREESTYLE, W | A), 400, -400, W | A));
	CHECK(Is(Run(SW | SK_PAUSED | SK_TIMER | SCROLL, W | A | SK_IN_LEFT), 400, -400, W | A | SK_IN_LEFT));

	// simplerladders: a ladder allows every key until the ground.
	SkState st = { false, -1 };
	Run(SW | SK_SIMPLERLADDERS, A, false, LADDER, &st);
	CHECK(st.allKeys);
	CHECK(Is(Run(SW | SK_SIMPLERLADDERS, W | A, false, WALK, &st), 400, -400, W | A));
	Run(SW | SK_SIMPLERLADDERS | SK_GROUNDKEYS, 0, true, WALK, &st);
	CHECK(!st.allKeys);
	CHECK(Is(Run(SW | SK_SIMPLERLADDERS, W | A, false, WALK, &st), 400, 0, W));
	st = { false, -1 };
	Run(SW, A, false, LADDER, &st);
	CHECK(!st.allKeys);
	// Paused, nothing is updated.
	st = { true, -1 };
	Run(SW | SK_PAUSED, 0, true, WALK, &st);
	CHECK(st.allKeys);
}

static void TestTracker()
{
	SkTracker t;
	SkMove m = Keys(W | A);
	// Nothing until the server reports.
	t.Apply(10, m, false, WALK);
	CHECK(Is(m, 400, -400, W | A));

	t.AddReport(10, SW | SK_SIMPLERLADDERS, -1);
	m = Keys(W | A);
	t.Apply(11, m, false, LADDER);
	CHECK(Is(m, 400, -400, W | A));
	m = Keys(W | A);
	t.Apply(12, m, false, WALK);
	CHECK(Is(m, 400, -400, W | A));  // the ladder at 11 allowed every key
	m = Keys(W | A);
	t.Apply(13, m, true, WALK);
	m = Keys(W | A);
	t.Apply(14, m, false, WALK);
	CHECK(Is(m, 400, 0, W));  // until the ground at 13

	// Replays of 12 to 14 give the same.
	m = Keys(W | A);
	t.Apply(12, m, false, WALK);
	CHECK(Is(m, 400, -400, W | A));
	m = Keys(W | A);
	t.Apply(13, m, true, WALK);
	m = Keys(W | A);
	t.Apply(14, m, false, WALK);
	CHECK(Is(m, 400, 0, W));

	// A/D-only: the server says, after 20, that A was picked; commands already predicted follow it.
	t.Clear();
	t.AddReport(20, ADONLY, -1);
	m = Keys(D);
	t.Apply(21, m, false, WALK);
	CHECK(Is(m, 0, 400, D));
	m = Keys(D);
	t.Apply(22, m, false, WALK);
	CHECK(Is(m, 0, 400, D));
	t.AddReport(20, ADONLY, 0);
	m = Keys(D);
	t.Apply(23, m, false, WALK);
	CHECK(Is(m, 0, 0, 0));
	// A later report for 21 (the start zone reset it): 22 picks D again.
	t.AddReport(21, ADONLY, -1);
	m = Keys(D);
	t.Apply(23, m, false, WALK);
	CHECK(Is(m, 0, 400, D));
	// A replay of 21 still uses the report from before it.
	m = Keys(D);
	t.Apply(21, m, false, WALK);
	CHECK(Is(m, 0, 0, 0));

	// The report's own state, for the command right after it.
	t.AddReport(30, SW | SK_ALLKEYS, -1);
	m = Keys(W | A);
	t.Apply(31, m, false, WALK);
	CHECK(Is(m, 400, -400, W | A));

	// A report older than everything kept gives no prediction.
	t.Clear();
	t.AddReport(100, SW, -1);
	m = Keys(A);
	t.Apply(50, m, false, WALK);
	CHECK(Is(m, 0, -400, A));

	// Far past the log: only the commands still in it are carried.
	t.Clear();
	t.AddReport(0, SW | SK_SIMPLERLADDERS, -1);
	for (int c = 1; c < 1000; ++c) {
		m = Keys(0);
		t.Apply(c, m, c < 500, c == 900 ? LADDER : WALK);
	}
	t.AddReport(0, SW | SK_SIMPLERLADDERS, -1);  // new sequence: rebuilt from the log
	m = Keys(A);
	t.Apply(1000, m, false, WALK);
	CHECK(Is(m, 0, -400, A));  // 900's ladder is in the last 128
	t.Clear();
	t.AddReport(0, SW | SK_SIMPLERLADDERS, -1);
	for (int c = 1; c < 1000; ++c) {
		m = Keys(0);
		t.Apply(c, m, false, c == 800 ? LADDER : WALK);
	}
	t.AddReport(0, SW | SK_SIMPLERLADDERS, -1);
	m = Keys(A);
	t.Apply(1000, m, false, WALK);
	CHECK(Is(m, 0, 0, 0));  // 800's isn't
}

int main()
{
	TestSideways();
	TestWOnly();
	TestADOnly();
	TestHSW();
	TestSurfHSW();
	TestLeftRight();
	TestExemptions();
	TestTracker();
	printf("style keys: %d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
