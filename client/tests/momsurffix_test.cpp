#include "momsurffix_sim.h"

#include <cstdio>
#include <vector>

static int g_fail = 0, g_pass = 0;

#define CHECK(cond) do { if (cond) ++g_pass; else { ++g_fail; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static bool Near(float a, float b, float tol = 1e-4f) { return fabsf(a - b) <= tol; }

static bool VecIs(const float v[3], float x, float y, float z, float tol = 1e-4f)
{
	return Near(v[0], x, tol) && Near(v[1], y, tol) && Near(v[2], z, tol);
}

// Axis-aligned solid boxes and the CS:S standing hull. Not the engine's collision code: enough
// for the control flow (start solid, hits, clear sweeps), with a DIST_EPSILON backoff.
struct BoxWorld : MsfWorld {
	struct Box { float mins[3], maxs[3]; };
	std::vector<Box> boxes;
	float hullMins[3] = { -16, -16, 0 }, hullMaxs[3] = { 16, 16, 62 };
	int traces = 0, retraces = 0, touches = 0;

	void Trace(const float start[3], const float end[3], const float* subMins, const float* addMaxs,
	           etrace::GameTraceX& tr) override
	{
		++traces;
		if (subMins) ++retraces;
		float mins[3], maxs[3];
		for (int k = 0; k < 3; ++k) {
			mins[k] = hullMins[k] - (subMins ? subMins[k] : 0.0f);
			maxs[k] = hullMaxs[k] + (addMaxs ? addMaxs[k] : 0.0f);
		}
		memset(&tr, 0, sizeof(tr));
		float best = 1.0f;
		int axis = -1;
		float sign = 0.0f;
		bool startSolid = false, endSolid = false;
		for (const Box& b : boxes) {
			float lo[3], hi[3];
			for (int k = 0; k < 3; ++k) { lo[k] = b.mins[k] - maxs[k]; hi[k] = b.maxs[k] - mins[k]; }
			bool in = true, inEnd = true;
			for (int k = 0; k < 3; ++k) {
				in = in && start[k] > lo[k] && start[k] < hi[k];
				inEnd = inEnd && end[k] > lo[k] && end[k] < hi[k];
			}
			if (in) { startSolid = true; endSolid = endSolid || inEnd; continue; }
			float t0 = 0.0f, t1 = 1.0f;
			int a = -1;
			float sg = 0.0f;
			bool miss = false;
			for (int k = 0; k < 3 && !miss; ++k) {
				const float dlt = end[k] - start[k];
				if (dlt == 0.0f) { miss = start[k] <= lo[k] || start[k] >= hi[k]; continue; }
				float ta = (lo[k] - start[k]) / dlt, tb = (hi[k] - start[k]) / dlt;
				float s = -1.0f;
				if (ta > tb) { const float t = ta; ta = tb; tb = t; s = 1.0f; }
				if (ta > t0) { t0 = ta; a = k; sg = s; }
				if (tb < t1) t1 = tb;
				miss = t0 >= t1;
			}
			if (miss || a < 0 || t0 >= best) continue;
			best = t0;
			axis = a;
			sign = sg;
		}
		if (startSolid) {
			tr.startsolid = true;
			tr.allsolid = endSolid;
			tr.fraction = 0.0f;
			MsfCopy(start, tr.endpos);
			return;
		}
		float len = 0.0f;
		for (int k = 0; k < 3; ++k) len += (end[k] - start[k]) * (end[k] - start[k]);
		len = sqrtf(len);
		if (axis >= 0 && len > 0.0f) {
			best -= 0.03125f / len;
			if (best < 0.0f) best = 0.0f;
			tr.plane.normal[axis] = sign;
		}
		tr.fraction = best;
		for (int k = 0; k < 3; ++k) tr.endpos[k] = start[k] + (end[k] - start[k]) * best;
	}

	void AddToTouched(const etrace::GameTraceX&, float*) override { ++touches; }
};

static MsfMove Move(float* origin, float* vel, bool onGround)
{
	MsfMove m;
	m.origin = origin;
	m.velocity = vel;
	m.frametime = 0.01f;
	m.onGround = onGround;
	m.walking = true;
	m.surfaceFriction = 1.0f;
	m.bounce = 0.0f;
	return m;
}

static MsfSettings Known()
{
	MsfSettings s;
	s.known = true;
	return s;
}

static void TestClipVelocity()
{
	const float up[3] = { 0, 0, 1 }, wall[3] = { 1, 0, 0 };
	float out[3];
	// The 2025 engine: 1/32 off the plane instead of flush with it.
	float in[3] = { 0, 30, -8 };
	CHECK(MsfClipVelocityEngine(in, up, out, 1.0f) == 1 && out[0] == 0.0f && out[1] == 30.0f && out[2] == 0.03125f);
	CHECK(MsfClipVelocityPlugin(in, up, out, 1.0f) == 1 && out[0] == 0.0f && out[1] == 30.0f && out[2] == 0.0f);
	const float in2[3] = { 260, 0, -4 };
	MsfClipVelocityEngine(in2, up, out, 1.0f);
	CHECK(out[0] == 260.0f && out[1] == 0.0f && out[2] == 0.03125f);
	// Moving away isn't clipped any more, only pushed.
	const float away[3] = { 0, 0, 10 };
	MsfClipVelocityEngine(away, up, out, 1.0f);
	CHECK(out[2] == 10.03125f);
	MsfClipVelocityPlugin(away, up, out, 1.0f);
	CHECK(out[2] == 0.0f);
	// A wall: blocked 2, and in may be out.
	float w[3] = { -100, 50, 0 };
	CHECK(MsfClipVelocityEngine(w, wall, w, 1.0f) == 2 && w[0] == 0.03125f && w[1] == 50.0f && w[2] == 0.0f);
	// Overbounce scales only the part into the plane.
	const float in3[3] = { 0, 0, -10 };
	MsfClipVelocityEngine(in3, up, out, 1.5f);
	CHECK(out[2] == 5.03125f);
	MsfSettings s;
	s.windowsServer = true;
	CHECK(MsfClipVelocity(s, in, up, out, 1.0f) == 1 && out[2] == 0.0f);
}

static void TestVectorNatives()
{
	float v[3] = { 0, 0, 9 };
	MsfNormalize(v, v);
	CHECK(VecIs(v, 0, 0, 1, 1e-6f));
	float w[3] = { 3, 4, 0 };
	MsfNormalize(w, w);
	CHECK(VecIs(w, 0.6f, 0.8f, 0, 1e-6f));
	const float a[3] = { 1, 0, 0 }, b[3] = { 0, 1, 0 };
	float c[3];
	MsfCross(a, b, c);
	CHECK(c[0] == 0.0f && c[1] == 0.0f && c[2] == 1.0f);
	MsfCross(b, a, c);
	CHECK(c[2] == -1.0f);
}

static void TestScanners()
{
	// client.dll x64 CGameMovement::TryPlayerMove: the gpGlobals load (no call after it), then
	// MoveHelper()->AddToTouched: mov rcx,[rip+55A233h] ... call [rax+10h].
	const uint8_t win[] = {
		0x48, 0x8B, 0x05, 0xDB, 0xFC, 0x48, 0x00, 0x41, 0x0F, 0x29, 0x7B, 0xA8, 0xF3, 0x44, 0x0F, 0x11,
		0x7C, 0x24, 0x30, 0xF3, 0x44, 0x0F, 0x11, 0x6C, 0x24, 0x34, 0xF3, 0x44, 0x0F, 0x10, 0x60, 0x10,
		0xF3, 0x0F, 0x11, 0x44, 0x24, 0x50, 0xF3, 0x0F, 0x11, 0x44, 0x24, 0x38, 0x4C, 0x89, 0x64, 0x24,
		0x40, 0x44, 0x89, 0x64, 0x24, 0x48,
		0x48, 0x8B, 0x0D, 0x33, 0xA2, 0x55, 0x00, 0x48, 0x8D, 0x54, 0x24, 0x70, 0x4C, 0x8B, 0x47, 0x10,
		0x49, 0x83, 0xC0, 0x44, 0x48, 0x8B, 0x01, 0xFF, 0x50, 0x10, 0xF3, 0x0F, 0x10, 0x55, 0x90,
	};
	ptrdiff_t t = 0;
	CHECK(MsfFindMoveHelperRef(win, sizeof(win), &t) && t == 54 + 7 + 0x55A233);

	// GCC (the Linux x64 server's build of the same function): lea rax,[rip+7AF233h]; ...;
	// mov r8,[rax]; mov rax,[r8]; mov rdi,r8; call [rax+10h]. The gpGlobals lea before it isn't it.
	const uint8_t gcc[] = {
		0x48, 0x8D, 0x05, 0xD0, 0x90, 0x7A, 0x00, 0x48, 0x89, 0xB5, 0x50, 0xFE, 0xFF, 0xFF, 0x48, 0x8B,
		0x7F, 0x10, 0x48, 0xC7, 0x85, 0x98, 0xFE, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0xC7, 0x85, 0x60,
		0xFE, 0xFF, 0xFF, 0x04, 0x00, 0x00, 0x00, 0x48, 0x8B, 0x00, 0xF3, 0x0F, 0x11, 0x9D, 0x78, 0xFE,
		0xFF, 0xFF,
		0x48, 0x8D, 0x05, 0x33, 0xF2, 0x7A, 0x00, 0x48, 0x8D, 0x57, 0x44, 0x48, 0x8D, 0xB5, 0xF0, 0xFE,
		0xFF, 0xFF, 0x4C, 0x8B, 0x00, 0x49, 0x8B, 0x00, 0x4C, 0x89, 0xC7, 0xFF, 0x50, 0x10, 0x44, 0x89,
	};
	CHECK(MsfFindMoveHelperRef(gcc, sizeof(gcc), &t) && t == 50 + 7 + 0x7AF233);

	// Another call first, or another vtable slot: not it.
	const uint8_t other[] = { 0x48, 0x8B, 0x0D, 0x10, 0x00, 0x00, 0x00, 0xE8, 0x00, 0x00, 0x00, 0x00, 0x48, 0x8B, 0x01, 0xFF, 0x50, 0x10 };
	CHECK(!MsfFindMoveHelperRef(other, sizeof(other), &t));
	const uint8_t slot3[] = { 0x48, 0x8B, 0x0D, 0x10, 0x00, 0x00, 0x00, 0x48, 0x8B, 0x01, 0xFF, 0x50, 0x18 };
	CHECK(!MsfFindMoveHelperRef(slot3, sizeof(slot3), &t));
	CHECK(!MsfFindMoveHelperRef(win, 60, &t));

	// client.dll TracePlayerBBox: call [rax+48h] (GetPlayerMaxs), call [r10+40h] (GetPlayerMins).
	const uint8_t tpbb[] = { 0x48, 0x8B, 0x01, 0x48, 0x8B, 0xF2, 0xFF, 0x50, 0x48, 0x4D, 0x8B, 0x16, 0x41, 0xFF, 0x52, 0x40 };
	CHECK(MsfHasVcall(tpbb, sizeof(tpbb), 0x48) && MsfHasVcall(tpbb, sizeof(tpbb), 0x40));
	CHECK(!MsfHasVcall(tpbb, sizeof(tpbb), 0x50));
	// GCC: call [rax+0E0h], call [rax+0D8h]; and a SIB form.
	const uint8_t gtp[] = { 0xFF, 0x90, 0xE0, 0x00, 0x00, 0x00, 0x48, 0x8B, 0x03, 0xFF, 0x90, 0xD8, 0x00, 0x00, 0x00 };
	CHECK(MsfHasVcall(gtp, sizeof(gtp), 0xE0) && MsfHasVcall(gtp, sizeof(gtp), 0xD8) && !MsfHasVcall(gtp, sizeof(gtp), 0x48));
	const uint8_t sib[] = { 0x41, 0xFF, 0x94, 0x24, 0xE0, 0x00, 0x00, 0x00 };
	CHECK(MsfHasVcall(sib, sizeof(sib), 0xE0));
}

// The bench's teleport cases: the destination leaves the hull 0.1 inside the floor.
static BoxWorld SunkFloor()
{
	BoxWorld w;
	w.boxes.push_back({ { -100000, -100000, -64 }, { 100000, 100000, 0.1f } });
	return w;
}

static void TestUnstick()
{
	// Falling faster than 6.25: the retrace finds the floor, the velocity is clipped off it and
	// the move starts 0.2 above: the server's +0.2 z and 30 u/s after bench case 1's teleport.
	{
		BoxWorld w = SunkFloor();
		float origin[3] = { -14184, -14600, 0 }, vel[3] = { 0, 30, -8 };
		const MsfMove m = Move(origin, vel, false);
		MsfTryPlayerMove(w, Known(), m, nullptr, nullptr);
		CHECK(VecIs(vel, 0, 30, 0.03125f, 1e-6f));
		CHECK(VecIs(origin, -14184, -14599.7f, 0.2003125f, 1e-3f));
		CHECK(w.retraces == 27);
	}
	// Slower than that with the workaround on: no retrace, stuck, velocity zeroed like stock.
	{
		BoxWorld w = SunkFloor();
		float origin[3] = { 0, 0, 0 }, vel[3] = { 260, 0, -4 };
		MsfTryPlayerMove(w, Known(), Move(origin, vel, false), nullptr, nullptr);
		CHECK(VecIs(vel, 0, 0, 0, 0) && VecIs(origin, 0, 0, 0, 0));
		CHECK(w.retraces == 0);
		// Traces on bumps 0, 1, 3, 5 and 7: every other bump drops the stuck state.
		CHECK(w.traces == 5);
	}
	{
		BoxWorld w = SunkFloor();
		float origin[3] = { 0, 0, 0 }, vel[3] = { 260, 0, -4 };
		MsfSettings s = Known();
		s.bumpcount = 4;
		MsfTryPlayerMove(w, s, Move(origin, vel, false), nullptr, nullptr);
		CHECK(w.traces == 3);
	}
	// The workaround off retraces anyway.
	{
		BoxWorld w = SunkFloor();
		float origin[3] = { 0, 0, 0 }, vel[3] = { 260, 0, -4 };
		MsfSettings s = Known();
		s.noclipWorkaround = false;
		MsfTryPlayerMove(w, s, Move(origin, vel, false), nullptr, nullptr);
		CHECK(w.retraces == 27);
		CHECK(Near(origin[2], 0.2f + 0.03125f * 0.01f, 1e-3f) && Near(origin[0], 2.6f, 1e-3f));
		CHECK(VecIs(vel, 260, 0, 0.03125f, 1e-4f));
	}
	// Moving up is never held back.
	{
		BoxWorld w = SunkFloor();
		float origin[3] = { 0, 0, 0 }, vel[3] = { 0, 0, 1 };
		MsfTryPlayerMove(w, Known(), Move(origin, vel, false), nullptr, nullptr);
		CHECK(w.retraces == 27);
		CHECK(origin[2] > 0.19f);
	}
	// The longer retrace length offsets the traces by 2 * bump * length and lifts by it.
	{
		BoxWorld w = SunkFloor();
		float origin[3] = { 0, 0, 0 }, vel[3] = { 0, 30, -8 };
		MsfSettings s = Known();
		s.retrace = 1.0f;
		MsfTryPlayerMove(w, s, Move(origin, vel, false), nullptr, nullptr);
		CHECK(Near(origin[2], 1.0003125f, 1e-3f));
	}
}

static void TestMoves()
{
	{
		BoxWorld w;
		float origin[3] = { 0, 0, 100 }, vel[3] = { 100, 0, -50 };
		const int blocked = MsfTryPlayerMove(w, Known(), Move(origin, vel, false), nullptr, nullptr);
		CHECK(blocked == 0 && VecIs(origin, 1, 0, 99.5f, 1e-5f) && VecIs(vel, 100, 0, -50, 0));
		// Bump 0's clear sweep is checked for a stuck end, once.
		CHECK(w.traces == 2 && w.touches == 0);
	}
	{
		BoxWorld w;
		w.boxes.push_back({ { -1000, -1000, -64 }, { 1000, 1000, 0 } });
		float origin[3] = { 0, 0, 0.5f }, vel[3] = { 100, 0, -100 };
		const int blocked = MsfTryPlayerMove(w, Known(), Move(origin, vel, false), nullptr, nullptr);
		CHECK(blocked == 1 && w.touches == 1);
		CHECK(VecIs(vel, 100, 0, 0.03125f, 1e-5f));
		CHECK(origin[2] > 0.0f && origin[2] < 0.5f && origin[0] > 0.9f);
	}
	// Straight into a wall in the air: the clipped velocity points back, so it's zeroed.
	{
		BoxWorld w;
		w.boxes.push_back({ { 20, -1000, -1000 }, { 100, 1000, 1000 } });
		float origin[3] = { 0, 0, 0 }, vel[3] = { 100, 0, 0 };
		MsfMove m = Move(origin, vel, false);
		m.frametime = 0.1f;
		const int blocked = MsfTryPlayerMove(w, Known(), m, nullptr, nullptr);
		CHECK(blocked == 2 && VecIs(vel, 0, 0, 0, 0));
		CHECK(Near(origin[0], 4.0f - 0.03125f, 1e-3f));
	}
	// Along a wall on the ground (one plane, walking): overbounce from sv_bounce and friction.
	{
		BoxWorld w;
		w.boxes.push_back({ { 20, -1000, -1000 }, { 100, 1000, 1000 } });
		float origin[3] = { 0, 0, 0 }, vel[3] = { 100, 100, 0 };
		MsfMove m = Move(origin, vel, true);
		m.frametime = 0.1f;
		m.bounce = 1.0f;
		m.surfaceFriction = 0.5f;
		MsfTryPlayerMove(w, Known(), m, nullptr, nullptr);
		// Overbounce 1.5 on the part into the wall, plus the 1/32 push: x = 100 - 150.03125.
		CHECK(Near(vel[0], -50.03125f, 1e-4f) && Near(vel[1], 100.0f, 1e-4f));
	}
	// WalkMove's first trace: an end equal to firstDest takes it without tracing.
	{
		BoxWorld w;
		float origin[3] = { 0, 0, 0 }, vel[3] = { 100, 0, 0 };
		const float dest[3] = { 1, 0, 0 };
		etrace::GameTraceX first;
		memset(&first, 0, sizeof(first));
		first.fraction = 1.0f;
		first.endpos[0] = 1.0f;
		MsfTryPlayerMove(w, Known(), Move(origin, vel, true), dest, &first);
		CHECK(w.traces == 1 && VecIs(origin, 1, 0, 0, 0));  // only the on-ground stuck check traced
	}
	// Into a corner in the air: each wall clips in turn, leaving the 1/32 pushes and the climb.
	{
		BoxWorld w;
		w.boxes.push_back({ { 20, -1000, -1000 }, { 100, 1000, 1000 } });
		w.boxes.push_back({ { -1000, 20, -1000 }, { 1000, 100, 1000 } });
		float origin[3] = { 0, 0, 0 }, vel[3] = { 100, 100, 50 };
		MsfMove m = Move(origin, vel, false);
		m.frametime = 0.1f;
		MsfTryPlayerMove(w, Known(), m, nullptr, nullptr);
		CHECK(VecIs(vel, -0.03125f, -0.03125f, 50.0f, 1e-4f));
	}
	{
		BoxWorld w;
		float origin[3] = { 5, 5, 5 }, vel[3] = { 0, 0, 0 };
		MsfTryPlayerMove(w, Known(), Move(origin, vel, false), nullptr, nullptr);
		CHECK(w.traces == 0 && VecIs(origin, 5, 5, 5, 0));
	}
}

struct StuckBoxWorld : MsfStuckWorld {
	BoxWorld w;
	int touches = 0;
	bool Blocked(const float pos[3], etrace::GameTraceX& tr) override
	{
		w.Trace(pos, pos, nullptr, nullptr, tr);
		return tr.startsolid;
	}
	void AddToTouched(const etrace::GameTraceX&, float*) override { ++touches; }
};

static void TestCheckStuck()
{
	float o[3];
	MsfStuckOffset(0, o);
	CHECK(VecIs(o, 0, 0, -0.125f, 0));
	MsfStuckOffset(2, o);
	CHECK(VecIs(o, 0, 0, 0.125f, 0));
	MsfStuckOffset(9, o);
	CHECK(VecIs(o, -0.125f, -0.125f, -0.125f, 0));
	MsfStuckOffset(16, o);
	CHECK(VecIs(o, 0.125f, 0.125f, 0.125f, 0));
	MsfStuckOffset(19, o);
	CHECK(VecIs(o, 0, 0, 6.0f, 0));
	MsfStuckOffset(26, o);
	CHECK(VecIs(o, -2.0f, -2.0f, 0, 0));
	MsfStuckOffset(52, o);
	CHECK(VecIs(o, 2.0f, 2.0f, 6.0f, 0));
	MsfStuckOffset(53, o);
	CHECK(VecIs(o, 0, 0, 0, 0));

	// Sunk 0.1 into the floor: the nudges -0.125 and 0 fail, +0.125 frees it, one per 50 ms, and
	// PlayerMove skips the move (1) until then.
	StuckBoxWorld sw;
	sw.w = SunkFloor();
	MsfStuckState s;
	float origin[3] = { 0, 0, 0 }, vel[3] = { 0, 0, -8 };
	int freedAt = -1, tries = 0;
	for (int t = 100; t < 130 && freedAt < 0; ++t) {
		const int before = sw.touches;
		if (MsfCheckStuck(sw, s, origin, vel, (float)t * 0.01f) == 0) freedAt = t;
		tries += sw.touches - before;
	}
	CHECK(tries == 3 && freedAt >= 110 && freedAt <= 112);
	CHECK(VecIs(origin, 0, 0, 0.125f, 0) && s.last == 0);
	// Not stuck: nothing moves and the nudge count restarts.
	s.last = 7;
	CHECK(MsfCheckStuck(sw, s, origin, vel, 2.0f) == 0 && s.last == 0 && VecIs(origin, 0, 0, 0.125f, 0));
}

int main()
{
	TestCheckStuck();
	TestClipVelocity();
	TestVectorNatives();
	TestScanners();
	TestUnstick();
	TestMoves();
	printf("momsurffix: %d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
