// bgs-jumpstats' HUD worked out from our own commands: jhud, ssj and offsets for hops whose
// numbers can be worked out by hand from bhop-get-stats.sp.
#include "../shared/jumpstats.cpp"

#include <cstdio>
#include <vector>

using namespace js;

static int g_fail = 0, g_pass = 0;

#define CHECK(cond) do { if (cond) ++g_pass; else { ++g_fail; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

struct Sent { int id; std::string data; };
static std::vector<Sent> g_sent;

namespace speedo {
int UserMessageId(const char* name) { return !strcmp(name, "HudMsg") ? 21 : !strcmp(name, "TextMsg") ? 5 : -1; }
void Dispatch(int id, const void* data, int len) { g_sent.push_back({ id, std::string(static_cast<const char*>(data), len) }); }
}
float TeleportPredict_TickInterval() { return 0.01f; }
void CoreLog(const char*, ...) {}

static const int kSayText2 = 3;

static void Command(std::vector<std::string> args)
{
	CCommandX c = {};
	c.argc = (int)args.size();
	static std::vector<std::string> keep;
	keep = args;
	for (int i = 0; i < c.argc; ++i) c.argv[i] = keep[i].c_str();
	if (args[0] == "bhoptools_jsset") js::Cmd_Settings(c);
	else if (args[0] == "bhoptools_jscc") js::Cmd_Colors(c);
	else if (args[0] == "bhoptools_jstime") js::Cmd_Time(c);
}

// jumpstats' defaults (PushDefault*), with `bools` in place of the default bools. bools2 < 0 leaves
// setting 15 out, as jumpstats before the prestrafe trainer does; moveCvars adds sv_accelerate 5,
// sv_friction 4 and sv_stopspeed 75 after it, as jumpstats with the ground prestrafe trainer does.
static void Settings(uint32_t bools, int cvars = 511, int bools2 = -1, bool moveCvars = false)
{
	char hex[16];
	std::vector<std::string> a = { "bhoptools_jsset", "7", "1", std::to_string(cvars), "0", "1", "2", "3", "4", "5" };
	const int settings[15] = { 0, 1, 2, 3, 4, (int)bools, 1, 0, 0x02593300, 0, 0x66, 0, 0, 6, 0 };
	for (int s : settings) {
		snprintf(hex, sizeof(hex), "%x", (unsigned)s);
		a.push_back(hex);
	}
	if (bools2 >= 0) a.push_back(std::to_string(bools2));
	if (moveCvars) {
		for (float f : { 5.0f, 4.0f, 75.0f }) {
			uint32_t bits;
			memcpy(&bits, &f, 4);
			snprintf(hex, sizeof(hex), "%x", bits);
			a.push_back(hex);
		}
	}
	Command(a);
}

static int g_cmd;

static void Run(js::Move& m)
{
	m.cmd = ++g_cmd;
	m.maxSpeed = 250.0f;
	m.lagged = 1.0f;
	js::PreMove(m);
	if (m.onGround && (m.buttons & IN_JUMP)) js::OnJump();
	const float origin[3] = { 0, 0, 0 };
	js::PostMove(origin, m.velocity, 1.0f);
}

static void Step(bool onGround, int buttons, float side, float yaw, float vy, float vx = 0)
{
	js::Move m = {};
	m.buttons = buttons;
	m.side = side;
	m.angles[1] = yaw;
	m.velocity[0] = vx;
	m.velocity[1] = vy;
	m.onGround = onGround;
	Run(m);
}

// Prestrafing with W+A at 100 tick, sv_accelerate 5, sv_friction 4, sv_stopspeed 75 and maxspeed 250, worked
// out here in double as CGameMovement does it: Friction, then Accelerate towards 45 degrees left of the view.
// The view keeps the wish direction at `share` of the best angle off the velocity, acos((W - a) / speed).
// Returns the speed after the last move and leaves what was drawn in g_sent.
static double Prestrafe(double share, int ticks)
{
	const double pi = 3.14159265358979323846, W = 250.0, a = 5.0 * 0.01 * W;
	double vx = 250.0, vy = 0.0;
	for (int i = 0; i < ticks; ++i) {
		js::Move m = {};
		m.forward = 400.0f;
		m.side = -400.0f;
		m.onGround = true;
		m.velocity[0] = (float)vx;
		m.velocity[1] = (float)vy;

		double speed = sqrt(vx * vx + vy * vy);
		const double control = speed < 75.0 ? 75.0 : speed;
		const double after = speed - control * 4.0 * 0.01;
		vx *= after / speed;
		vy *= after / speed;
		speed = after;
		const double best = speed > W - a ? acos((W - a) / speed) : 0.0;
		const double wish = atan2(vy, vx) + share * best;
		m.angles[1] = (float)((wish - pi / 4) * 180.0 / pi);
		Run(m);

		const double c = vx * cos(wish) + vy * sin(wish);
		const double add = c < W - a ? a : W - c;
		if (add > 0.0) {
			vx += add * cos(wish);
			vy += add * sin(wish);
		}
	}
	return sqrt(vx * vx + vy * vy);
}

static float FloatAt(const std::string& s, size_t at)
{
	float f;
	memcpy(&f, s.data() + at, 4);
	return f;
}

// The text of a HudMsg, after channel, x, y, two colours, the effect and four floats.
static std::string HudText(const Sent& s) { return s.data.substr(1 + 8 + 8 + 1 + 16, s.data.size() - 34 - 1); }
static std::string ChatText(const Sent& s) { return s.data.substr(2, s.data.size() - 3); }

int main()
{
	js::Reset(7);
	js::SetChat(1, kSayText2);
	Command({ "bhoptools_jscc", "7", "10", "0", "%07aaPRE\n%01\n%03" });

	// Nothing until the plugin says so.
	Step(true, IN_JUMP, 0, 90, 300);
	CHECK(g_sent.empty());

	const uint32_t defaults = JHUD_ENABLED | JHUD_SYNC | SSJ_ENABLED | SSJ_REPEAT | SSJ_GAIN | SSJ_STRAFES | SSJ_SYNC | SHOWKEYS_UNRELIABLE;
	Settings(defaults);
	for (int i = 0; i < 12; ++i) Step(true, 0, 0, 90, 0);  // past BHOP_FRAMES: the jump state resets
	g_sent.clear();

	// Strafing right with the velocity at right angles to the view: every air tick gains fully.
	Step(true, IN_JUMP, 400, 90, 300);
	CHECK(g_sent.size() == 2);
	if (g_sent.size() == 2) {
		const Sent& hud = g_sent[0];
		CHECK(hud.id == 21 && hud.data[0] == 1);
		CHECK(FloatAt(hud.data, 1) == -1.0f && FloatAt(hud.data, 5) == -1.0f);
		CHECK((unsigned char)hud.data[9] == 255 && (unsigned char)hud.data[10] == 255 && (unsigned char)hud.data[11] == 255);
		CHECK(FloatAt(hud.data, 1 + 8 + 8 + 1 + 8) == 1.0f);
		CHECK(HudText(hud) == "1: 300");
		CHECK(g_sent[1].id == kSayText2 && g_sent[1].data[0] == 1 && g_sent[1].data[1] == 0);
		CHECK(ChatText(g_sent[1]) == "\x07" "aaPRE\x01J: \x03" "1 \x01| S: \x03" "300");
	}
	g_sent.clear();

	for (int i = 0; i < 10; ++i) Step(false, 0, 400, 90, 300);
	CHECK(g_sent.empty());
	Step(true, IN_JUMP, 400, 90, 300);
	CHECK(g_sent.size() == 2);
	if (g_sent.size() == 2) {
		CHECK(HudText(g_sent[0]) == "2: 300\n100.00 100.00%");
		// 300 is under jump 2's slowest speed colour
		CHECK((unsigned char)g_sent[0].data[9] == 255 && g_sent[0].data[10] == 0 && g_sent[0].data[11] == 0);
		CHECK(ChatText(g_sent[1]) == "\x07" "aaPRE\x01J: \x03" "2 \x01| S: \x03" "300 \x01| G: \x07" "ffffff100% \x01| S: \x03" "100% \x01| Stf: \x03" "0");
	}
	g_sent.clear();

	// shavit's time: 1.5 after this command, 0.01 more each command after.
	Settings(defaults | SSJ_SHAVIT_TIME | SSJ_SHAVIT_TIME_DELTA);
	Command({ "bhoptools_jstime", "7", std::to_string(g_cmd), "3fc00000", "3c23d70a" });
	for (int i = 0; i < 10; ++i) Step(false, 0, 400, 90, 300);
	Step(true, IN_JUMP, 400, 90, 300);
	CHECK(g_sent.size() == 2);
	if (g_sent.size() == 2)
		CHECK(ChatText(g_sent[1]) == "\x07" "aaPRE\x01J: \x03" "3 \x01| S: \x03" "300 \x01| G: \x07" "ffffff100% \x01| S: \x03" "100% \x01| Stf: \x03" "0 \x01| T: \x03" "1.61 \x01| T\xCE\x94: \x03" "1.61");
	g_sent.clear();
	for (int i = 0; i < 10; ++i) Step(false, 0, 400, 90, 300);
	Step(true, IN_JUMP, 400, 90, 300);
	if (g_sent.size() == 2)
		CHECK(ChatText(g_sent[1]).find("T: \x03" "1.72 \x01| T\xCE\x94: \x03" "0.11") != std::string::npos);
	g_sent.clear();
	Settings(defaults);

	// Offsets: the key goes from right to left a tick before the view turns from left to right.
	Settings(defaults | OFFSETS_ENABLED);
	for (int i = 0; i < 12; ++i) Step(true, 0, 0, 90, 0);
	Step(true, IN_JUMP, 400, 90, 300);
	g_sent.clear();
	float yaw = 90;
	for (int i = 0; i < 4; ++i) Step(false, 0, 400, yaw += 1, 300);
	Step(false, 0, -400, yaw += 1, 300);
	Step(false, 0, -400, yaw -= 1, 300);
	CHECK(g_sent.size() == 1);
	if (g_sent.size() == 1) {
		CHECK(g_sent[0].data[0] == 2);
		CHECK(HudText(g_sent[0]) == "-1 (0)");
		CHECK((unsigned char)g_sent[0].data[9] == 255 && (unsigned char)g_sent[0].data[10] == 255 && (unsigned char)g_sent[0].data[11] == 255);
		CHECK(FloatAt(g_sent[0].data, 5) == 89 / 255.0f);
	}

	// Real gain: at 40 u/s with 14 of it along wishdir, the engine takes the speed to
	// sqrt(40² - 14² + 30²) = 48 of the best sqrt(40² + 30²) = 50, so 8 of 10. The old gain is (30 - 14) / 30.
	const float vy = sqrtf(40 * 40 - 14 * 14);
	Settings(defaults, 511 | JSCV_REALGAIN);
	for (int i = 0; i < 12; ++i) Step(true, 0, 0, 90, 0);
	Step(true, IN_JUMP, 400, 90, vy, 14);
	for (int i = 0; i < 10; ++i) Step(false, 0, 400, 90, vy, 14);
	g_sent.clear();
	Step(true, IN_JUMP, 400, 90, vy, 14);
	CHECK(g_sent.size() == 2);
	if (g_sent.size() == 2) {
		CHECK(HudText(g_sent[0]).find("\n80.00 100.00%") != std::string::npos);
		CHECK(ChatText(g_sent[1]).find("| G: \x07" "ff000080%") != std::string::npos);
	}
	Settings(defaults);
	for (int i = 0; i < 10; ++i) Step(false, 0, 400, 90, vy, 14);
	g_sent.clear();
	Step(true, IN_JUMP, 400, 90, vy, 14);
	CHECK(g_sent.size() == 2);
	if (g_sent.size() == 2) CHECK(HudText(g_sent[0]).find("\n53.33 100.00%") != std::string::npos);

	// Prestrafe trainer. Holding the best angle every tick, the speed settles at shavit's MaxPrestrafe,
	// W * sqrt((A / F) * (2 - A dt) / (2 - F dt)), and the trainer reads 100, drawn every 13
	// commands like the slow strafe trainer, on the first group whose own HUD is off.
	Settings(defaults, 511 | JSCV_PRETRAINER, PRESTRAFETRAINER_ENABLED, true);
	g_sent.clear();
	const double maxPrestrafe = 250.0 * sqrt((5.0 / 4.0) * (2.0 - 5.0 * 0.01) / (2.0 - 4.0 * 0.01));
	double speed = Prestrafe(1.0, 390);
	CHECK(fabs(speed - maxPrestrafe) < 0.01);
	CHECK(g_sent.size() == 30);
	if (!g_sent.empty()) {
		const Sent& hud = g_sent.back();
		CHECK(hud.id == 21 && hud.data[0] == 0);
		CHECK(FloatAt(hud.data, 1) == -1.0f);
		CHECK((unsigned char)hud.data[9] == 255 && (unsigned char)hud.data[10] == 255 && (unsigned char)hud.data[11] == 255);
		CHECK(FloatAt(hud.data, 1 + 8 + 8 + 1 + 8) == 13 / 100.0f + 0.05f);
		CHECK(HudText(hud).compare(0, 4, "100\n") == 0);
	}
	// Any other angle ends slower and reads as its share: 80 is turning too slowly, 120 too fast.
	g_sent.clear();
	CHECK(Prestrafe(0.8, 390) < maxPrestrafe - 0.5);
	CHECK(!g_sent.empty() && HudText(g_sent.back()).compare(0, 3, "80\n") == 0);
	g_sent.clear();
	CHECK(Prestrafe(1.2, 390) < maxPrestrafe - 0.5);
	CHECK(!g_sent.empty() && HudText(g_sent.back()).compare(0, 4, "120\n") == 0);

	Settings(defaults | TRAINER_ENABLED, 511 | JSCV_PRETRAINER, PRESTRAFETRAINER_ENABLED, true);
	g_sent.clear();
	Prestrafe(1.0, 13);
	CHECK(g_sent.size() == 1 && g_sent[0].data[0] == 2);
	// Jumping, in the air, under the speed where pushing straight is best, without move keys, off in
	// Bools2 or from a plugin that doesn't send the cvars: nothing.
	g_sent.clear();
	for (int i = 0; i < 13; ++i) Step(true, IN_JUMP, -400, 90.0f + i, 270);
	for (int i = 0; i < 13; ++i) Step(false, 0, -400, 90.0f + i, 270);
	for (int i = 0; i < 13; ++i) Step(true, 0, -400, 90.0f + i, 200);
	for (int i = 0; i < 13; ++i) Step(true, 0, 0, 90.0f + i, 270);
	int drawn = 0;
	for (const Sent& s : g_sent) drawn += s.id == 21 && s.data[0] == 2;
	CHECK(drawn == 0);
	Settings(defaults, 511 | JSCV_PRETRAINER, 0, true);
	g_sent.clear();
	Prestrafe(1.0, 26);
	Settings(defaults, 511 | JSCV_PRETRAINER, PRESTRAFETRAINER_ENABLED);
	Prestrafe(1.0, 26);
	CHECK(g_sent.empty());

	// A new session draws nothing until settings come again.
	js::Reset(8);
	g_sent.clear();
	for (int i = 0; i < 10; ++i) Step(false, 0, 400, 90, 300);
	Step(true, IN_JUMP, 400, 90, 300);
	CHECK(g_sent.empty());

	printf("jumpstats: %d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
