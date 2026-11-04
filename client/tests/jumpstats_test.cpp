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
	m.surfaceFriction = 1.0f;
	js::PreMove(m);
	if (m.onGround && (m.buttons & IN_JUMP)) js::OnJump();
	const float origin[3] = { 0, 0, 0 };
	js::PostMove(origin, m.velocity);
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
// Returns the speed after the last move and leaves what was drawn in g_sent, in g_expect what the last tick
// gained over the most it could, mirrored above 1 when the angle was too wide, in g_last the last move and in
// g_above how many ticks started at or above 93% of the best top speed.
static double g_expect;
static js::Move g_last;
static int g_above;
static double Prestrafe(double share, int ticks, double start = 250.0)
{
	const double pi = 3.14159265358979323846, W = 250.0, a = 5.0 * 0.01 * W;
	const double top = W * sqrt((5.0 / 4.0) * (2.0 - 5.0 * 0.01) / (2.0 - 4.0 * 0.01));
	double vx = start, vy = 0.0;
	g_above = 0;
	for (int i = 0; i < ticks; ++i) {
		js::Move m = {};
		m.forward = 400.0f;
		m.side = -400.0f;
		m.onGround = true;
		m.velocity[0] = (float)vx;
		m.velocity[1] = (float)vy;

		double speed = sqrt(vx * vx + vy * vy);
		g_above += speed >= 0.93 * top;
		const double control = speed < 75.0 ? 75.0 : speed;
		const double after = speed - control * 4.0 * 0.01;
		vx *= after / speed;
		vy *= after / speed;
		speed = after;
		const double best = speed > W - a ? acos((W - a) / speed) : 0.0;
		const double wish = atan2(vy, vx) + share * best;
		m.angles[1] = (float)((wish - pi / 4) * 180.0 / pi);
		Run(m);
		g_last = m;

		const double c = vx * cos(wish) + vy * sin(wish);
		const double add = c < W - a ? a : W - c;
		if (add > 0.0) {
			vx += add * cos(wish);
			vy += add * sin(wish);
		}
		const double most = speed > W - a ? sqrt(speed * speed + 2.0 * a * (W - a) + a * a) : speed + a;
		const double gain = (sqrt(vx * vx + vy * vy) - speed) / (most - speed);
		g_expect = c > W - a ? gain : 2.0 - gain;
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

// The trainer bar's marker in half slots from the left, 25 the centre.
static int Marker(const std::string& text)
{
	size_t at = text.find('\n', text.find('\n') + 1) + 2;
	for (int i = 0; at < text.size() && text[at] != '\n'; ++i, ++at) {
		if (text[at] != ' ') return 2 * i + (text.compare(at, 3, "\xE2\x96\x95") == 0);
	}
	return -1;
}

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
	// W * sqrt((A / F) * (2 - A dt) / (2 - F dt)). From 93% of it, and not below, the trainer shows the top speed
	// each tick is on pace for, white, drawn every command on the first group whose own HUD is off.
	Settings(defaults, 511 | JSCV_PRETRAINER, PRESTRAFETRAINER_ENABLED, true);
	g_sent.clear();
	const double maxPrestrafe = 250.0 * sqrt((5.0 / 4.0) * (2.0 - 5.0 * 0.01) / (2.0 - 4.0 * 0.01));
	double speed = Prestrafe(1.0, 390);
	CHECK(fabs(speed - maxPrestrafe) < 0.01);
	CHECK(g_above > 0 && g_above < 390 && (int)g_sent.size() == g_above);
	if (!g_sent.empty()) {
		const Sent& hud = g_sent.back();
		CHECK(hud.id == 21 && hud.data[0] == 0);
		CHECK(FloatAt(hud.data, 1) == -1.0f);
		CHECK((unsigned char)hud.data[9] == 255 && (unsigned char)hud.data[10] == 255 && (unsigned char)hud.data[11] == 255);
		CHECK(FloatAt(hud.data, 1 + 8 + 8 + 1 + 8) == 13 / 100.0f + 0.05f);
		CHECK(HudText(hud).compare(0, 4, Fmt("%i\n", RoundFloat((float)maxPrestrafe))) == 0);
		CHECK(abs(Marker(HudText(hud)) - 25) <= 1);
	}
	// Held at any angle, the speed settles at the pace the trainer showed for the gain share the ticks get there.
	// The marker is left of centre turning too slowly, right too fast; white only within 1.5 u/s of the best.
	const struct { double share; int side; } held[] = { { 0.999, -1 }, { 0.998, -1 }, { 0.995, -1 }, { 0.99, -1 }, { 0.98, -1 }, { 0.95, -1 }, { 0.8, -1 }, { 0.5, -1 }, { 1.02, 1 }, { 1.05, 1 }, { 1.1, 1 }, { 1.2, 1 }, { 1.5, 1 } };
	for (const auto& h : held) {
		g_sent.clear();
		speed = Prestrafe(h.share, 390, 275.0);
		float pace, top;
		const float value = js::PreValue(g_last.velocity, g_last.forward, g_last.side, g_last.angles[1], 250.0f, 5.0f, 4.0f, 75.0f, 0.01f, pace, top);
		CHECK(fabs(value - g_expect) < 0.0002);
		CHECK(fabs(pace - speed) < 0.02);
		CHECK(fabs(top - maxPrestrafe) < 0.01);
		CHECK((int)g_sent.size() == g_above);
		if (speed < 0.93 * maxPrestrafe) continue;
		const Sent& hud = g_sent.back();
		CHECK(HudText(hud).compare(0, 4, Fmt("%i\n", RoundFloat(pace))) == 0);
		CHECK((Marker(HudText(hud)) - 25) * h.side >= 1);
		const bool white = (unsigned char)hud.data[9] == 255 && (unsigned char)hud.data[10] == 255 && (unsigned char)hud.data[11] == 255;
		CHECK(white == (top - pace <= 1.5f));
	}

	// Ticks from WR prestrafes on the beta servers (maxspeed 260), whose next recorded speed the engine steps
	// reproduce: the velocity before the move, view yaw, move keys, and the value worked out in double.
	struct ReplayTick { float vx, vy, yaw, forward, side; double value; };
	const ReplayTick ticks[] = {
		{ 237.858582f, 15.1580811f, -2.73442388f, 400, -400, 1.208096 },   // W+D at 238: turning before it pays
		{ 229.969208f, 133.538818f, -2.73442388f, 400, -400, 0.807893 },   // 266: the old trainer read 83
		{ 263.720703f, 78.7109375f, -8.22267246f, 400, -400, 0.924778 },   // 275, a little too slow
		{ 80.6976318f, 267.880249f, 51.4542427f, 400, -400, 1.000669 },    // 280, all but perfect
		{ 254.281616f, 54.3251038f, 11.3472071f, 400, 0, 0.808014 },       // W alone at 260: the old trainer read 9
		{ 134.181213f, -253.471375f, -51.5783157f, 400, 400, 1.077510 },   // 287, the old trainer read 131
		{ -118.29071f, 16.65802f, 171.510223f, 400, 0, 1.000031 },         // W at 119: straight ahead is best
		{ -141.264725f, 247.630386f, 139.130096f, 400, 400, 1.000483 },    // 285
	};
	for (const ReplayTick& t : ticks) {
		const float v[3] = { t.vx, t.vy, 0.0f };
		float pace, top;
		CHECK(fabs(js::PreValue(v, t.forward, t.side, t.yaw, 260.0f, 5.0f, 4.0f, 75.0f, 0.01f, pace, top) - t.value) < 0.0002);
		CHECK(fabs(top - 289.95f) < 0.01f);
	}

	Settings(defaults | TRAINER_ENABLED, 511 | JSCV_PRETRAINER, PRESTRAFETRAINER_ENABLED, true);
	g_sent.clear();
	Prestrafe(1.0, 13, 275.0);
	CHECK(g_sent.size() == 13 && g_sent[0].data[0] == 2);
	// Pushing straight ahead below the speed where turning pays: under 93%, nothing.
	g_sent.clear();
	{
		js::Move m = {};
		m.forward = 400.0f;
		m.angles[1] = 90.0f;
		m.velocity[1] = 200.0f;
		m.onGround = true;
		Run(m);
	}
	CHECK(g_sent.empty());
	// Jumping, in the air, with stamina left, without move keys, off in Bools2 or from a plugin that doesn't
	// send the cvars: nothing.
	g_sent.clear();
	for (int i = 0; i < 13; ++i) Step(true, IN_JUMP, -400, 90.0f + i, 270);
	for (int i = 0; i < 13; ++i) Step(false, 0, -400, 90.0f + i, 270);
	for (int i = 0; i < 13; ++i) Step(true, 0, 0, 90.0f + i, 270);
	{
		js::Move m = {};
		m.side = -400.0f;
		m.angles[1] = 90.0f;
		m.velocity[1] = 270.0f;
		m.onGround = true;
		m.stamina = 500.0f;
		Run(m);
	}
	int drawn = 0;
	for (const Sent& s : g_sent) drawn += s.id == 21 && s.data[0] == 2;
	CHECK(drawn == 0);
	Settings(defaults, 511 | JSCV_PRETRAINER, 0, true);
	g_sent.clear();
	Prestrafe(1.0, 26, 275.0);
	Settings(defaults, 511 | JSCV_PRETRAINER, PRESTRAFETRAINER_ENABLED);
	Prestrafe(1.0, 26, 275.0);
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
