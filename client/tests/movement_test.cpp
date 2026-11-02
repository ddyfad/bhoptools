#include "server_movement.h"

#include <cstdio>

static int g_fail = 0, g_pass = 0;

#define CHECK(cond) do { if (cond) ++g_pass; else { ++g_fail; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static void TestMaxSpeedScan()
{
	// client.dll x64 CGameMovement::ProcessMovement: the touch list loop's store to +0x3c,
	// then mv->m_flMaxSpeed = player->GetPlayerMaxSpeed() through vtable slot 0x730 / 8.
	const uint8_t win[] = {
		0xC7, 0x40, 0x30, 0xF1, 0xD8, 0xFF, 0xFF, 0xC7, 0x40, 0x3C, 0xF1, 0xD8, 0xFF, 0xFF,
		0x48, 0x8D, 0x40, 0x04, 0x49, 0x83, 0xE9, 0x01, 0x0F, 0x85, 0x8D, 0xFD, 0xFF, 0xFF, 0x44, 0x89,
		0x89, 0xC4, 0x0C, 0x00, 0x00, 0x48, 0x89, 0x51, 0x08, 0x4C, 0x89, 0x41, 0x10, 0x48, 0x8B, 0xCA,
		0x48, 0x8B, 0x02, 0xFF, 0x90, 0x30, 0x07, 0x00, 0x00, 0x48, 0x8B, 0x43, 0x10, 0x48, 0x8D, 0x15,
		0x07, 0xB0, 0x31, 0x00, 0x48, 0x8B, 0xCB, 0xF3, 0x0F, 0x11, 0x40, 0x3C, 0x48, 0x8B, 0x43, 0x10,
	};
	CHECK(FindMaxSpeedVcall(win, sizeof(win)) == 230);

	// GCC shapes: mv kept in r13, and in r12 (which needs a SIB byte).
	const uint8_t r13[] = { 0x48, 0x8B, 0x03, 0x48, 0x89, 0xDF, 0xFF, 0x90, 0x38, 0x07, 0x00, 0x00,
		0xF3, 0x41, 0x0F, 0x11, 0x45, 0x3C, 0x90 };
	CHECK(FindMaxSpeedVcall(r13, sizeof(r13)) == 231);
	const uint8_t r12[] = { 0xFF, 0x90, 0x38, 0x07, 0x00, 0x00, 0x49, 0x8B, 0x04, 0x24,
		0xF3, 0x41, 0x0F, 0x11, 0x44, 0x24, 0x3C, 0x90 };
	CHECK(FindMaxSpeedVcall(r12, sizeof(r12)) == 231);

	// A store of another register, or to another field, isn't it.
	const uint8_t xmm1[] = { 0xFF, 0x90, 0x30, 0x07, 0x00, 0x00, 0xF3, 0x0F, 0x11, 0x48, 0x3C, 0x90 };
	CHECK(FindMaxSpeedVcall(xmm1, sizeof(xmm1)) == -1);
	const uint8_t field[] = { 0xFF, 0x90, 0x30, 0x07, 0x00, 0x00, 0xF3, 0x0F, 0x11, 0x40, 0x40, 0x90 };
	CHECK(FindMaxSpeedVcall(field, sizeof(field)) == -1);
	// Too far after the call.
	uint8_t far_[60] = { 0xFF, 0x90, 0x30, 0x07, 0x00, 0x00 };
	for (int i = 6; i < 50; ++i) far_[i] = 0x90;
	const uint8_t store[] = { 0xF3, 0x0F, 0x11, 0x40, 0x3C };
	memcpy(far_ + 50, store, sizeof(store));
	CHECK(FindMaxSpeedVcall(far_, sizeof(far_)) == -1);
	CHECK(FindMaxSpeedVcall(win, 70) == -1);
}

static void TestHaze()
{
	// bhop_coolkist_s2, floor at -224: landed at -223.704834, the server jumped from floor + 1.5.
	CHECK(LandfixHazeZ(-223.704834f, true, -223.96875f) == -222.5f);
	// A trace end on a whole unit has no default height.
	CHECK(LandfixHazeZ(100.25f, true, 100.0f) == 101.5f);
	// Dead, not walking, deep water or a missed trace: GetGroundUnits is 0.
	CHECK(LandfixHazeZ(64.0f, false, 0.0f) == 65.5f);
	CHECK(LandfixHazeZ(-223.704834f, false, -223.96875f) == -222.204834f);
	CHECK(LandfixHazeZ(-222.5f, true, -223.96875f) == -222.5f);

	const std::vector<int> none, skip = { 57, 90 };
	CHECK(LandfixHazeWanted(true, -1, 0, none));        // landed on the world
	CHECK(LandfixHazeWanted(true, 0, 57, none));        // world to an entity
	CHECK(!LandfixHazeWanted(true, 0, 0, none));        // still on the same ground
	CHECK(!LandfixHazeWanted(true, 0, -1, none));       // took off
	CHECK(!LandfixHazeWanted(false, -1, 0, none));      // no history
	CHECK(!LandfixHazeWanted(true, -1, 57, skip));      // func_movelinear
	CHECK(LandfixHazeWanted(true, -1, 58, skip));
}

static void TestOlivia()
{
	float z = 0.0f;
	// Low: raised to 0.49 above the trace end.
	CHECK(LandfixOliviaZ(-223.704834f, -223.96875f, &z) && z == -223.96875f + 0.49f);
	CHECK(LandfixOliviaZ(10.0f, 10.0f, &z) && z == 10.0f + 0.49f);
	// Between 1.5 and 2: to 1.5.
	CHECK(LandfixOliviaZ(11.75f, 10.0f, &z) && z == 11.5f);
	// Otherwise untouched.
	CHECK(!LandfixOliviaZ(10.5f, 10.0f, &z));
	CHECK(!LandfixOliviaZ(11.0f, 10.0f, &z));
	CHECK(!LandfixOliviaZ(11.5f, 10.0f, &z));

	OliviaState s = { true, 0.0f, 0, true, 0 };
	CHECK(LandfixOliviaArmed(s));
	s.oldButtons = 1 << 1; CHECK(!LandfixOliviaArmed(s));  // jump held
	s.oldButtons = 1 << 2; CHECK(!LandfixOliviaArmed(s));  // duck held
	s.oldButtons = 1 << 0; CHECK(LandfixOliviaArmed(s));   // attack doesn't matter
	s.oldButtons = 0;
	s.waterLevel = 2; CHECK(!LandfixOliviaArmed(s));
	s.waterLevel = 1; CHECK(LandfixOliviaArmed(s));
	s.waterJumpTime = 0.5f; CHECK(!LandfixOliviaArmed(s));
	s.waterJumpTime = 0.0f;
	s.onGround = false; CHECK(!LandfixOliviaArmed(s));
	s.onGround = true;
	s.alive = false; CHECK(!LandfixOliviaArmed(s));
}

static void TestGroundLog()
{
	GroundLog log;
	int g = 0;
	CHECK(!log.Get(100, &g));
	log.Put(100, -1);
	log.Put(101, 0);
	CHECK(log.Get(100, &g) && g == -1);
	CHECK(log.Get(101, &g) && g == 0);
	// A newer command in the same slot replaces it.
	log.Put(100 + GroundLog::kSize, 7);
	CHECK(!log.Get(100, &g));
	CHECK(log.Get(100 + GroundLog::kSize, &g) && g == 7);
	// Tickbase keys can be anything.
	log.Put(-5, 3);
	CHECK(log.Get(-5, &g) && g == 3);
	log.Clear();
	CHECK(!log.Get(101, &g));
}

int main()
{
	TestMaxSpeedScan();
	TestHaze();
	TestOlivia();
	TestGroundLog();
	printf("movement: %d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
