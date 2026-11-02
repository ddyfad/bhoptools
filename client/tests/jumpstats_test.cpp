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

// jumpstats' defaults (PushDefault*), with `bools` in place of the default bools.
static void Settings(uint32_t bools)
{
	char hex[16];
	std::vector<std::string> a = { "bhoptools_jsset", "7", "1", "511", "0", "1", "2", "3", "4", "5" };
	const int settings[15] = { 0, 1, 2, 3, 4, (int)bools, 1, 0, 0x02593300, 0, 0x66, 0, 0, 6, 0 };
	for (int s : settings) {
		snprintf(hex, sizeof(hex), "%x", (unsigned)s);
		a.push_back(hex);
	}
	Command(a);
}

static int g_cmd;

static void Step(bool onGround, int buttons, float side, float yaw, float vy)
{
	js::Move m = {};
	m.cmd = ++g_cmd;
	m.buttons = buttons;
	m.side = side;
	m.angles[1] = yaw;
	m.velocity[1] = vy;
	m.onGround = onGround;
	m.maxSpeed = 250.0f;
	m.lagged = 1.0f;
	js::PreMove(m);
	if (onGround && (buttons & IN_JUMP)) js::OnJump();
	const float origin[3] = { 0, 0, 0 };
	js::PostMove(origin, m.velocity);
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

	// A new session draws nothing until settings come again.
	js::Reset(8);
	g_sent.clear();
	for (int i = 0; i < 10; ++i) Step(false, 0, 400, 90, 300);
	Step(true, IN_JUMP, 400, 90, 300);
	CHECK(g_sent.empty());

	printf("jumpstats: %d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
