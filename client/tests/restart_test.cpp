#include "restart_predict.h"

#include <cstdio>

static int g_fail = 0, g_pass = 0;

#define CHECK(cond) do { if (cond) ++g_pass; else { ++g_fail; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static RsDest ZoneStart()
{
	RsDest d;
	d.flags = RS_TELEPORT | RS_START | RS_STAMINA | RS_WALK | RS_SS_GROUND | RS_SS_AIR;
	d.origin[0] = 100.0f; d.origin[1] = 200.0f; d.origin[2] = 65.0f;
	d.gravity = 1.0f;
	d.lagged = 1.0f;
	return d;
}

static void TestSetStartOverlay()
{
	RsPredictor p;
	p.SetState(10, ZoneStart());

	const float ground[3] = { 150.0f, 250.0f, 64.03125f };
	p.SetStart(12, true, ground, 5.0f, 90.0f);
	// Before the setstart the server's state holds; after it, 1 unit above where it was saved.
	RsDest d = p.Destination(12);
	CHECK(d.origin[0] == 100.0f && !(d.flags & RS_ANGLES) && (d.flags & RS_START));
	d = p.Destination(13);
	CHECK((d.flags & RS_TELEPORT) && d.origin[0] == 150.0f && d.origin[2] == 65.03125f);
	CHECK((d.flags & RS_ANGLES) && d.pitch == 5.0f && d.yaw == 90.0f);
	CHECK(!(d.flags & RS_START) && (d.flags & RS_STAMINA));

	// An angles-only setstart after a positioned one needs the zone centre nobody knows.
	const float air[3] = { 0.0f, 0.0f, 500.0f };
	p.SetStart(14, false, air, -10.0f, 45.0f);
	CHECK(!(p.Destination(15).flags & RS_TELEPORT));

	p.SetState(14, ZoneStart());
	CHECK(p.ssCount == 0);
	CHECK(p.Destination(15).origin[0] == 100.0f);

	// In the air at the zone centre only the angles change.
	p.SetStart(16, false, air, -10.0f, 45.0f);
	d = p.Destination(17);
	CHECK((d.flags & RS_TELEPORT) && (d.flags & RS_ANGLES) && (d.flags & RS_START));
	CHECK(d.origin[2] == 65.0f && d.pitch == -10.0f && d.yaw == 45.0f);

	// A replay of the setstart command updates it.
	p.SetStart(16, false, air, -20.0f, 45.0f);
	CHECK(p.ssCount == 1 && p.Destination(17).pitch == -20.0f);
}

static void TestSetStartOtherTrack()
{
	RsPredictor p;
	RsDest s = ZoneStart();
	s.flags = (s.flags & ~(RS_SS_GROUND | RS_SS_AIR)) | RS_SS_GROUND_KEEP | RS_SS_AIR_KEEP;
	p.SetState(0, s);
	const float o[3] = { 1.0f, 2.0f, 3.0f };
	p.SetStart(1, true, o, 0.0f, 0.0f);
	p.SetStart(2, false, o, 0.0f, 0.0f);
	const RsDest d = p.Destination(3);
	CHECK((d.flags & RS_TELEPORT) && d.origin[0] == 100.0f && !(d.flags & RS_ANGLES));

	// A custom spawn keeps its spot and ignores angles-only setstarts.
	RsPredictor q;
	RsDest c = ZoneStart();
	c.flags = RS_TELEPORT | RS_SS_GROUND | RS_SS_AIR_KEEP;
	q.SetState(0, c);
	q.SetStart(1, false, o, 0.0f, 0.0f);
	CHECK((q.Destination(2).flags & RS_TELEPORT) && !(q.Destination(2).flags & RS_ANGLES));

	// Not teleporting (a stop-timer warning, say) stays that way whatever the setstart.
	RsPredictor n;
	n.SetState(0, RsDest());
	n.SetStart(1, true, o, 0.0f, 0.0f);
	CHECK(!(n.Destination(2).flags & RS_TELEPORT));
}

static void TestDecideAndAck()
{
	RsPredictor p;
	p.SetState(0, ZoneStart());
	RsRecord* r = p.Decide(5, true);
	CHECK(r->apply && r->dest.origin[0] == 100.0f && r->oldLagged < 0.0f);

	// A state arriving later doesn't change a command already decided.
	RsDest moved = ZoneStart();
	moved.origin[0] = 300.0f;
	p.SetState(3, moved);
	CHECK(p.Decide(5, true)->dest.origin[0] == 100.0f);
	CHECK(p.Decide(6, true)->dest.origin[0] == 300.0f);
	// Without a server that takes the impulse nothing is applied.
	CHECK(!p.Decide(7, false)->apply);

	// The ack replaces the decision: replays do what the server did.
	RsDest server = ZoneStart();
	server.origin[0] = 400.0f;
	server.flags |= RS_ANGLES;
	server.yaw = 180.0f;
	p.Ack(5, true, server);
	CHECK(p.Find(5)->apply && p.Find(5)->dest.origin[0] == 400.0f && p.Find(5)->dest.yaw == 180.0f);
	p.Ack(6, false, RsDest());
	CHECK(!p.Find(6)->apply);
	// One we didn't predict but the server did.
	p.Ack(7, true, server);
	CHECK(p.Find(7)->apply);
	// Acks for commands we never ran are kept for their replays.
	p.Ack(8, true, server);
	CHECK(p.Find(8) && p.Find(8)->apply);
}

static void TestLogLimits()
{
	RsPredictor p;
	p.SetState(0, ZoneStart());
	for (int k = 1; k <= RsPredictor::kLog + 5; ++k) p.Decide(k, true);
	CHECK(p.logCount == RsPredictor::kLog && !p.Find(5) && p.Find(6));

	const float o[3] = { 0.0f, 0.0f, 0.0f };
	for (int k = 1; k <= RsPredictor::kSetStarts + 2; ++k) p.SetStart(k * 10, true, o, 0.0f, 0.0f);
	CHECK(p.ssCount == RsPredictor::kSetStarts && p.ss[0].key == 30);
	// Older than everything kept: dropped rather than pushing a newer one out.
	p.SetStart(5, true, o, 0.0f, 0.0f);
	CHECK(p.ss[0].key == 30);
	// Out of order stays sorted, the oldest making room.
	p.SetStart(35, true, o, 0.0f, 0.0f);
	bool sorted = true;
	for (int i = 1; i < p.ssCount; ++i) sorted = sorted && p.ss[i - 1].key < p.ss[i].key;
	CHECK(sorted && p.ss[0].key == 35);
}

int main()
{
	TestSetStartOverlay();
	TestSetStartOtherTrack();
	TestDecideAndAck();
	TestLogLimits();
	printf("restart: %d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
