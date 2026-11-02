// A port of bhop-get-stats 1.5 and the bgs-jumpstats 3.9 modules that draw for the player themselves,
// as deployed (bhop-get-stats 5467a29, jumpstats b2a90d6). Float math is kept in float, as SourcePawn
// does it, so the numbers come out the same.
#include "jumpstats.h"
#include "speedo.h"
#include "teleport_predict.h"
#include "engine_trace_layout.h"
#include "hudkeys.h"
#include "game_layout.h"
#include "utils.h"

#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace js {
namespace {

// bgs-jumpstats' settings indexes and bits (jumpstat/settings.sp)
enum { GainReallyBad, GainBad, GainMeh, GainGood, GainReallyGood, Bools, Usage, Positions_X, Positions_Y,
	Positions2_X, Positions2_Y, TrainerSpeed, JhudCutOff, JhudSpeedColorsJump, JhudHidden, kSettings };
enum { HudJhud, HudTrainer, HudOffset, HudSpeedometer, HudFjt, HudShowKeys };
enum { Trainer_Slow, Trainer_Medium, Trainer_Fast };

const uint32_t JHUD_ENABLED = 1u << 0, JHUD_JSS = 1u << 1, JHUD_SYNC = 1u << 2, JHUD_GMOD_JSS = 1u << 3,
	TRAINER_ENABLED = 1u << 4, OFFSETS_ENABLED = 1u << 5, SPEEDOMETER_ENABLED = 1u << 6,
	SPEEDOMETER_VELOCITY_DIFF = 1u << 7, SSJ_ENABLED = 1u << 8, SSJ_REPEAT = 1u << 9, SSJ_HEIGHTDIFF = 1u << 10,
	SSJ_GAIN = 1u << 11, SSJ_JUMP_HEIGHT = 1u << 12, SSJ_EFFICIENCY = 1u << 13, SSJ_SHAVIT_TIME = 1u << 14,
	SSJ_SHAVIT_TIME_DELTA = 1u << 15, SSJ_STRAFES = 1u << 16, SSJ_SYNC = 1u << 17, OFFSETS_ADVANCED = 1u << 21,
	SSJ_JSS = 1u << 22, SSJ_DECIMALS = 1u << 23, SSJ_OFFSETS = 1u << 24, TRAINER_STRICT = 1u << 25,
	SHOWKEYS_ENABLED = 1u << 26, SHOWKEYS_SIMPLE = 1u << 27, SHOWKEYS_UNRELIABLE = 1u << 28,
	SHOWYAWS_ENABLED = 1u << 29, SPEEDOMETER_SMALL_VELOCITY = 1u << 30, SPEEDOMETER_COMPARISON = 1u << 31;
const int JHUD_HIDE_JUMP = 1 << 0, JHUD_HIDE_SPEED = 1 << 1, JHUD_HIDE_GAIN = 1 << 2;

// bhoptools_jsset cvar bits, which js-enabled-* and js-allow-trainer-* are on
const int JSCV_JHUD = 1 << 0, JSCV_TRAINER = 1 << 1, JSCV_TRAINER_MEDIUM = 1 << 2, JSCV_TRAINER_FAST = 1 << 3,
	JSCV_OFFSET = 1 << 4, JSCV_SPEEDOMETER = 1 << 5, JSCV_SSJ = 1 << 6, JSCV_SHOWKEYS = 1 << 7, JSCV_SHOWYAWS = 1 << 8;

const int kColors[9][3] = {
	{ 255, 0, 0 }, { 255, 165, 0 }, { 0, 255, 0 }, { 0, 255, 255 }, { 255, 255, 255 },
	{ 255, 255, 0 }, { 0, 0, 255 }, { 128, 0, 128 }, { 238, 0, 255 },
};
const char* const kColorsHex[9] = {
	"\x07" "ff0000", "\x07" "ffa500", "\x07" "00ff00", "\x07" "00ffff", "\x07" "ffffff",
	"\x07" "ffff00", "\x07" "0000ff", "\x07" "800080", "\x07" "ee00ff",
};
const int kJhudSpeeds[17][4] = {
	{ 0, 0, 0, 0 },
	{ 280, 282, 287, 289 }, { 366, 370, 375, 380 }, { 438, 442, 450, 455 }, { 500, 505, 515, 525 },
	{ 555, 560, 570, 580 }, { 605, 610, 620, 633 }, { 655, 665, 675, 680 }, { 700, 710, 725, 730 },
	{ 740, 750, 765, 770 }, { 780, 790, 805, 810 }, { 810, 820, 840, 850 }, { 850, 860, 875, 885 },
	{ 880, 900, 900, 920 }, { 910, 920, 935, 955 }, { 945, 955, 965, 990 }, { 970, 980, 1000, 1020 },
};

const int BHOP_FRAMES = 10;
const int OFFSETS_MAX_FRAME = 15;
const int SPEED_UPDATE_INTERVAL = 5;
const int YAWS_UPDATE_INTERVAL = 10;
const int MIN_UPDATE_RATE = 50;
const int TRAINER_FULLUPDATE_TICK_INTERVAL = 13;
const int TRAINER_SIZE = 13;
const float TRAINER_MIN_GAIN = 0.3f;
const int IN_JUMP = 1 << 1, IN_FORWARD = 1 << 3, IN_BACK = 1 << 4, IN_LEFT = 1 << 7, IN_RIGHT = 1 << 8,
	IN_MOVELEFT = 1 << 9, IN_MOVERIGHT = 1 << 10;

// What the plugin sent.
int  g_session;
bool g_on;
int  g_cvars;
int  g_channels[6];
int  g_set[kSettings];
std::string g_prefix, g_text, g_variable;
std::string g_colorsIn;
int  g_colorsLen = -1;
bool g_hasComparison;
float g_comparison;
// shavit's time after command g_timeCmd and what it adds each command after.
bool   g_haveTime;
int    g_timeCmd;
double g_time, g_timeRate;
int  g_client;
int  g_sayText2 = -1;
int  g_hudMsg = -1, g_textMsg = -1;

uint32_t BoolSet() { return (uint32_t)g_set[Bools]; }
bool Has(uint32_t bit) { return (BoolSet() & bit) != 0; }
bool Cvar(int bit) { return (g_cvars & bit) != 0; }
const int* Color(int setting)
{
	const int c = setting >= 0 && setting < kSettings ? g_set[setting] : 0;
	return kColors[c >= 0 && c < 9 ? c : 0];
}

// bhop-get-stats
struct Bgs {
	bool  touchesWall, jumpedThisTick, overlap, noPress, sawTurn, sawPress;
	int   ticksOnGround, strafeTick, syncedTick, jump, strafeCount, turnTick, keyTick, turnDir, cmdNum, yawwingTick;
	float oldHeight, rawGain, trajectory, traveled[2], lastYaw, avgDiffFromPerf, avgAbsoluteJss;
	float lastNonZeroMove[2], lastJumpPosition[3], lastVeer, jumpPeak;
} g_bgs;

// The command between PreMove and PostMove.
Move g_move;
bool g_inMove;
bool g_touched;

// jumpstats' module state
int   g_lastOffset, g_repeatedOffsets;
int   g_offsetHistory[OFFSETS_MAX_FRAME], g_currentFrame;
float g_trainerNumber, g_lastAverage, g_trainerBar[3];
int   g_trainerCmd;
float g_currentSpeed;
int   g_speedDirection, g_speedTick, g_yawTick;
int   g_keysCmd, g_keysLastTurn, g_keysLastButtons;
float g_keysLastYaw;
bool  g_keysDelayed;

typedef bool (*AddToTouchedFn)(void* self, const void* trace, const float* velocity);
AddToTouchedFn oAddToTouched;
void** g_touchSlot;

float NormalizeAngle(float ang)
{
	while (ang > 180.0f) ang -= 360.0f;
	while (ang < -180.0f) ang += 360.0f;
	return ang;
}

float Speed2D(const float v[3]) { return sqrtf(v[0] * v[0] + v[1] * v[1]); }
float RadToDeg(float r) { return r * (180.0f / 3.14159265358979323846f); }
int RoundToFloor(float f) { return (int)floorf(f); }
int RoundFloat(float f) { return (int)lroundf(f); }

float TickInterval()
{
	const float t = TeleportPredict_TickInterval();
	return t > 0.0f ? t : 0.01f;
}
int TickRate() { return RoundToFloor(1.0f / TickInterval()); }

float HudCoord(int hud, int dim)
{
	const int index = hud / 4 == 0 ? (dim ? Positions_Y : Positions_X) : (dim ? Positions2_Y : Positions2_X);
	const int v = (int)(((uint32_t)g_set[index] >> ((hud % 4) * 8)) & 255);
	return v <= 0 ? -1.0f : (float)v / 255.0f;
}

void Send(int id, const std::string& payload)
{
	speedo::Dispatch(id, payload.data(), (int)payload.size());
}

void PutFloat(std::string& s, float f)
{
	char b[4];
	memcpy(b, &f, 4);
	s.append(b, 4);
}

// SetHudTextParams with alpha 255 and no effect, then ShowHudText.
void ShowHud(int hud, float x, const int rgb[3], float hold, int group, const std::string& text)
{
	if (g_hudMsg < 0) g_hudMsg = speedo::UserMessageId("HudMsg");
	if (g_hudMsg < 0) return;
	std::string m;
	m += (char)(g_channels[group] & 0xFF);
	PutFloat(m, x);
	PutFloat(m, HudCoord(hud, 1));
	m += (char)rgb[0]; m += (char)rgb[1]; m += (char)rgb[2]; m += (char)255;
	m += (char)255; m += (char)255; m += (char)250; m += (char)0;
	m += (char)0;
	PutFloat(m, 0.0f);
	PutFloat(m, 0.0f);
	PutFloat(m, hold);
	PutFloat(m, 0.0f);
	m += text;
	m += '\0';
	Send(g_hudMsg, m);
}

void ShowHud(int hud, const int rgb[3], float hold, int group, const std::string& text)
{
	ShowHud(hud, HudCoord(hud, 0), rgb, hold, group, text);
}

void CenterText(const std::string& text)
{
	if (g_textMsg < 0) g_textMsg = speedo::UserMessageId("TextMsg");
	if (g_textMsg >= 0) Send(g_textMsg, hudkeys::CenterTextMsg(text));
}

// chatcolors' SayText2: the prefix, the text colour, the message, cut at 252 bytes.
void Chat(const std::string& message)
{
	if (g_sayText2 < 0) return;
	std::string line = g_prefix + g_text + message;
	if (line.size() > 252) line.resize(252);
	std::string m;
	m += (char)g_client;
	m += (char)0;
	m += line;
	m += '\0';
	Send(g_sayText2, m);
}

std::string Fmt(const char* fmt, ...)
#ifndef _WIN32
	__attribute__((format(printf, 1, 2)))
#endif
	;
std::string Fmt(const char* fmt, ...)
{
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	return buf;
}

void Trim(std::string& s)
{
	const size_t a = s.find_first_not_of(" \t\r\n");
	if (a == std::string::npos) { s.clear(); return; }
	s = s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}

int PercentageColorIdx(float gain, bool strict)
{
	if (gain > 100) {
		if (strict) return GainReallyBad;
		if (gain <= 105.0f) return GainReallyGood;
		if (gain <= 110.0f) return GainGood;
		if (gain <= 115.0f) return GainBad;
		return GainReallyBad;
	}
	if (gain >= 90) return GainReallyGood;
	if (gain >= 80) return GainGood;
	if (gain >= 70) return GainMeh;
	if (gain >= 60) return GainBad;
	return GainReallyBad;
}

int SpeedColorIdx(int jump, int speed)
{
	if (jump < 0 || jump > 16) return -1;
	if (speed >= kJhudSpeeds[jump][3]) return GainReallyGood;
	if (speed >= kJhudSpeeds[jump][2]) return GainGood;
	if (speed >= kJhudSpeeds[jump][1]) return GainMeh;
	if (speed >= kJhudSpeeds[jump][0]) return GainBad;
	return GainReallyBad;
}

// jhud.sp
void JhudFirst(int speed)
{
	if (!Cvar(JSCV_JHUD) || !Has(JHUD_ENABLED)) return;
	const int hidden = g_set[JhudHidden];
	std::string message;
	if (!(hidden & JHUD_HIDE_JUMP)) message = "1:";
	if (!(hidden & JHUD_HIDE_SPEED)) message += Fmt(" %i", speed);
	Trim(message);
	if (message.empty()) return;
	ShowHud(HudJhud, Color(SpeedColorIdx(1, speed)), 1.0f, 1, message);
}

void Jhud(int jump, int speed, float gain, float sync, float jss, float absJss)
{
	if (!Cvar(JSCV_JHUD) || !Has(JHUD_ENABLED)) return;
	if (jump > g_set[JhudCutOff] && g_set[JhudCutOff] != 0) return;

	int idx = jump <= 16 && jump <= g_set[JhudSpeedColorsJump] ? SpeedColorIdx(jump, speed) : PercentageColorIdx(gain, true);
	const int hidden = g_set[JhudHidden];
	// jss only: colour by how far jss is from 100%
	if (jump > 1 && (hidden & JHUD_HIDE_SPEED) && (hidden & JHUD_HIDE_GAIN) && !Has(JHUD_SYNC) && (BoolSet() & (JHUD_JSS | JHUD_GMOD_JSS))) {
		const float off = fabsf(jss * 100.0f - 100.0f);
		idx = off <= 3.0f ? GainReallyGood : off <= 10.0f ? GainGood : off <= 25.0f ? GainMeh : GainReallyBad;
	}

	std::string message, stats;
	if (!(hidden & JHUD_HIDE_JUMP)) message = Fmt("%i:", jump);
	if (!(hidden & JHUD_HIDE_SPEED)) message += Fmt(" %i", speed);
	Trim(message);

	if (jump > 1) {
		const bool bracket = !message.empty();
		if (Has(JHUD_JSS)) {
			message += Fmt(bracket ? " (%i%%)" : "%i%%", RoundToFloor(jss * 100.0f));
		} else if (Has(JHUD_GMOD_JSS)) {
			const char* arrow = absJss == 100 ? "\xE2\x9C\x93" : RoundToFloor(jss * 100.0f) > 100 ? "\xE2\x86\x93" : "\xE2\x86\x91";
			message += Fmt(bracket ? " (%i%% %s)" : "%i%% %s", RoundToFloor(absJss), arrow);
		}
		if (!(hidden & JHUD_HIDE_GAIN)) stats = Fmt("%.2f", gain);
		if (Has(JHUD_SYNC)) stats += Fmt(" %.2f%%", sync);
		Trim(stats);
		if (!stats.empty()) message += (message.empty() ? "" : "\n") + stats;
	}
	if (message.empty()) return;
	ShowHud(HudJhud, Color(idx), 1.0f, 1, message);
}

// offsets.sp
int OffsetColorIdx(int offset)
{
	if (offset > 0) return GainReallyBad;
	if (offset == 0) return GainMeh;
	if (offset == -1) return GainReallyGood;
	if (offset == -2) return GainGood;
	if (offset == -3) return GainBad;
	return GainReallyBad;
}

void Offset(int offset, bool overlap, bool nopress)
{
	if (!Cvar(JSCV_OFFSET)) return;
	g_repeatedOffsets = g_lastOffset == offset ? g_repeatedOffsets + 1 : 0;
	g_offsetHistory[g_currentFrame] = offset;
	if (++g_currentFrame >= OFFSETS_MAX_FRAME) g_currentFrame = 0;

	if (Has(OFFSETS_ENABLED)) {
		std::string message = Fmt("%d (%i)", offset, g_repeatedOffsets);
		int idx = OffsetColorIdx(offset);
		if (Has(OFFSETS_ADVANCED)) {
			if (overlap) { message += " Overlap"; idx = GainReallyBad; }
			if (nopress) { message += " No Press"; idx = GainReallyBad; }
		}
		ShowHud(HudOffset, Color(idx), 0.5f, 2, message);
	}
	g_lastOffset = offset;
}

// ssj.sp
float g_lastJumpTime;

// Shavit_GetClientTime after the command in g_move
float ShavitTime()
{
	return g_haveTime ? (float)(g_time + (g_move.cmd - g_timeCmd) * g_timeRate) : 0.0f;
}

void SsjFirst(int speed)
{
	if (!Cvar(JSCV_SSJ)) return;
	if (Has(SSJ_ENABLED)) Chat(Fmt("J: %s1 %s| S: %s%i", g_variable.c_str(), g_text.c_str(), g_variable.c_str(), speed));
	g_lastJumpTime = ShavitTime();
}

void SsjWrite(int jump, int speed, int strafes, float maxHeight, float heightDelta, float gain, float sync, float eff, float jss, float time)
{
	if (Has(SSJ_REPEAT)) {
		if (g_set[Usage] > 0 && jump % g_set[Usage] != 0) return;
	} else if (jump != g_set[Usage]) {
		return;
	}
	const char* t = g_text.c_str();
	const char* v = g_variable.c_str();
	const bool decimals = Has(SSJ_DECIMALS);
	std::string m = Fmt("J: %s%i %s| S: %s%i", v, jump, t, v, speed);
	if (Has(SSJ_GAIN)) {
		const int c = g_set[PercentageColorIdx(gain, true)];
		const char* hex = kColorsHex[c >= 0 && c < 9 ? c : 0];
		m += decimals ? Fmt(" %s| G: %s%.2f%%", t, hex, gain) : Fmt(" %s| G: %s%i%%", t, hex, RoundToFloor(gain));
	}
	if (Has(SSJ_SYNC)) m += decimals ? Fmt(" %s| S: %s%.2f%%", t, v, sync) : Fmt(" %s| S: %s%i%%", t, v, RoundToFloor(sync));
	if (Has(SSJ_STRAFES)) m += Fmt(" %s| Stf: %s%i", t, v, strafes);
	if (Has(SSJ_JSS)) m += Fmt(" %s| Jss: %s%i", t, v, RoundToFloor(jss * 100));
	if (Has(SSJ_EFFICIENCY)) m += decimals ? Fmt(" %s| Ef: %s%.2f%%", t, v, eff) : Fmt(" %s| Ef: %s%i%%", t, v, RoundToFloor(eff));
	if (Has(SSJ_JUMP_HEIGHT)) m += Fmt(" %s| H: %s%.1f", t, v, maxHeight);
	if (Has(SSJ_HEIGHTDIFF)) m += Fmt(" %s| H\xCE\x94: %s%.1f", t, v, heightDelta);
	// message[230], with 30 kept for the prefix
	if (g_haveTime && Has(SSJ_SHAVIT_TIME)) m += Fmt(" %s| T: %s%.2f", t, v, time);
	if (m.size() > 229) m.resize(229);
	if (g_haveTime && (int)m.size() < 200 && Has(SSJ_SHAVIT_TIME_DELTA)) m += Fmt(" %s| T\xCE\x94: %s%.2f", t, v, time - g_lastJumpTime);
	if (m.size() > 229) m.resize(229);
	if ((int)m.size() < 200 && g_currentFrame >= 1 && Has(SSJ_OFFSETS)) {
		m += Fmt(" %s| Of:%s", t, v);
		for (int i = 0; i < g_currentFrame && m.size() < 229; ++i)
			m += Fmt(i == 0 ? " %i" : ", %i", g_offsetHistory[i]);
		if (m.size() > 229) m.resize(229);
	}
	Chat(m);
}

void Ssj(int jump, int speed, int strafes, float maxHeight, float heightDelta, float gain, float sync, float eff, float jss)
{
	if (!Cvar(JSCV_SSJ)) return;
	const float time = ShavitTime();
	if (Has(SSJ_ENABLED)) SsjWrite(jump, speed, strafes, maxHeight, heightDelta, gain, sync, eff, jss, time);
	g_lastJumpTime = time;
}

// trainer.sp
float TrainerPos(float percentage, int size)
{
	const int maxIndex = size - 1;
	const float minPercent = TRAINER_MIN_GAIN;
	const float maxPercent = 1.0f + (1.0f - TRAINER_MIN_GAIN);
	percentage = (percentage - minPercent) / (maxPercent - minPercent);
	float pos = percentage * maxIndex;
	if (pos < 0.0f) pos = 0.0f;
	if (pos >= (float)maxIndex) pos = (float)maxIndex;
	return pos;
}

std::string TrainerString(float number, float average)
{
	std::string line;
	const int center = RoundFloat(TrainerPos(1.0f, TRAINER_SIZE));
	for (int i = 0; i < TRAINER_SIZE; ++i) line += i != center ? "\xE2\x94\x80" : "|";

	const int size = TRAINER_SIZE * 2;
	const float pos = TrainerPos(average, size);
	const float remainder = fmodf(pos, 1.0f);
	const int index = RoundToFloor(pos);
	std::string vis;
	for (int i = 0; i < size; ++i)
		vis += i != index ? " " : remainder >= 0.5f ? "\xE2\x96\x95" : "\xE2\x96\x8F";

	return Fmt("%i\n", RoundFloat(number * 100)) + line + "\n " + vis + " \n" + line;
}

void Trainer(bool inbhop, float jss)
{
	if (!Cvar(JSCV_TRAINER)) return;
	++g_trainerCmd;
	if (!inbhop) {
		g_trainerNumber = 0.0f;
		g_trainerBar[0] = g_trainerBar[1] = g_trainerBar[2] = 0.0f;
		return;
	}
	g_trainerNumber += jss;
	for (float& b : g_trainerBar) b += jss;

	const int rate = TickRate();
	const int speeds[3] = { RoundToFloor(rate * (TRAINER_FULLUPDATE_TICK_INTERVAL / 100.0f)), RoundToFloor(rate * 0.09f), RoundToFloor(rate * 0.05f) };
	const bool allowed[3] = { true, Cvar(JSCV_TRAINER_MEDIUM), Cvar(JSCV_TRAINER_FAST) };
	const int cmd = g_trainerCmd;
	bool due = cmd % TRAINER_FULLUPDATE_TICK_INTERVAL == 0;
	for (int i = 0; i < 3; ++i) due = due || (speeds[i] > 0 && cmd % speeds[i] == 0 && allowed[i]);
	if (!due) return;

	if (cmd % TRAINER_FULLUPDATE_TICK_INTERVAL == 0) {
		g_lastAverage = g_trainerNumber / TRAINER_FULLUPDATE_TICK_INTERVAL;
		g_trainerNumber = 0.0f;
	}
	float avg[3] = { -1.0f, -1.0f, -1.0f };
	for (int i = 0; i < 3; ++i) {
		if (speeds[i] > 0 && cmd % speeds[i] == 0 && allowed[i]) {
			avg[i] = g_trainerBar[i] / speeds[i];
			g_trainerBar[i] = 0.0f;
		}
	}

	const int which = g_set[TrainerSpeed] >= 0 && g_set[TrainerSpeed] < 3 ? g_set[TrainerSpeed] : Trainer_Slow;
	if (!Has(TRAINER_ENABLED) || speeds[which] <= 0 || cmd % speeds[which] != 0 || avg[which] == -1.0f) return;
	const int idx = PercentageColorIdx(avg[which] * 100, Has(TRAINER_STRICT));
	const float hold = speeds[which] / (rate * 1.0f) + 0.05f;
	ShowHud(HudTrainer, -1.0f, Color(idx), hold, 0, TrainerString(g_lastAverage, avg[which]));
}

// speedometer.sp and yawdiff.sp, on every 5th and 10th command where the plugins count game frames
void Speedometer(const float velocity[3])
{
	if (!Cvar(JSCV_SPEEDOMETER) || ++g_speedTick % SPEED_UPDATE_INTERVAL != 0) return;
	g_speedTick = 0;
	const float before = g_currentSpeed;
	g_currentSpeed = Speed2D(velocity);
	const int delta = RoundToFloor(g_currentSpeed - before);
	int idx;
	if (delta > 0) { idx = GainGood; g_speedDirection = 1; }
	else if (delta == 0) idx = GainReallyGood;
	else { idx = GainReallyBad; g_speedDirection = -1; }
	const int speed = RoundToFloor(g_currentSpeed);

	const bool enabled = Has(SPEEDOMETER_ENABLED), smallVelocity = Has(SPEEDOMETER_SMALL_VELOCITY);
	if (!enabled && !smallVelocity) return;
	const bool comparison = g_hasComparison && Has(SPEEDOMETER_COMPARISON);
	const std::string cmp = comparison ? Fmt(" (%s%.0f)", g_comparison >= 0.0f ? "+" : "", g_comparison) : "";
	if (smallVelocity) CenterText(Fmt("%s%i", g_speedDirection < 0 ? "-" : "+", speed) + cmp);
	if (!enabled) return;

	std::string m = speed < 10 ? Fmt("   %i", speed) : speed < 1000 ? Fmt(" %i", speed) : Fmt("%i", speed);
	if (Has(SPEEDOMETER_VELOCITY_DIFF)) m += delta > 0 ? Fmt(" (+%i)", delta) : Fmt(" (%i)", delta);
	ShowHud(HudSpeedometer, Color(idx), 0.15f, 4, m + cmp);
}

void ShowYaws(const float velocity[3], float viewYaw)
{
	if (!Cvar(JSCV_SHOWYAWS) || ++g_yawTick % YAWS_UPDATE_INTERVAL != 0) return;
	g_yawTick = 0;
	if (Speed2D(velocity) < 10.0f || !Has(SHOWYAWS_ENABLED)) return;
	const float deltaYaw = -NormalizeAngle(RadToDeg(atan2f(velocity[1], velocity[0])) - viewYaw);
	int bar = RoundFloat((deltaYaw / 180.0f) * 10);
	bar = bar < -10 ? -10 : bar > 10 ? 10 : bar;
	char visual[22];
	for (int j = 0; j < 21; ++j) visual[j] = j == 10 + bar ? '|' : '-';
	visual[21] = '\0';
	// yawdiff.sp's first two checks always pass
	const int idx = deltaYaw > 90 || deltaYaw < -90 ? GainReallyBad : GainMeh;
	ShowHud(HudSpeedometer, Color(idx), 0.2f, 4, Fmt("Yaw \xCE\x94: %.1f\xC2\xB0\n[%s]", deltaYaw, visual));
}

// showkeys.sp
void ShowKeys(int buttons, float yaw)
{
	if (!Cvar(JSCV_SHOWKEYS)) return;
	++g_keysCmd;
	const float yawDiff = NormalizeAngle(yaw - g_keysLastYaw);
	const int turn = yawDiff > 0.0f ? 1 : yawDiff < 0.0f ? -1 : 0;
	bool update = g_keysDelayed || turn != g_keysLastTurn || buttons != g_keysLastButtons || g_keysCmd % MIN_UPDATE_RATE == 0;
	if (update && g_keysCmd < RoundToFloor(TickRate() * 0.03f)) {
		g_keysDelayed = true;
		update = false;
	}
	g_keysLastYaw = yaw;
	g_keysLastButtons = buttons;
	g_keysLastTurn = turn;
	if (!update) return;

	if (Has(SHOWKEYS_ENABLED)) {
		// hudkeys' layouts 2 and 3 are these two
		const std::string text = hudkeys::Format(Has(SHOWKEYS_SIMPLE) ? 3 : 2, buttons, yawDiff);
		static const int white[3] = { 255, 255, 255 };
		if (Has(SHOWKEYS_UNRELIABLE)) CenterText(text);
		else ShowHud(HudShowKeys, white, 0.65f, 3, text);
	}
	g_keysDelayed = false;
	g_keysCmd = 0;
}

// bhop-get-stats' forwards
void FirstJumpForward(int speed)
{
	JhudFirst(speed);
	SsjFirst(speed);
	g_currentFrame = 0;
}

void JumpForward(int jump, int speed, int strafes, float maxHeight, float heightDelta, float gain, float sync, float eff, float jss, float absJss)
{
	Jhud(jump, speed, gain, sync, jss, absJss);
	Ssj(jump, speed, strafes, maxHeight, heightDelta, gain, sync, eff, jss);
	if (Cvar(JSCV_OFFSET)) g_currentFrame = 0;
}

void ResetJumpState()
{
	Bgs& b = g_bgs;
	b.jump = b.strafeTick = b.syncedTick = 0;
	b.rawGain = b.trajectory = 0.0f;
	b.strafeCount = 0;
	b.traveled[0] = b.traveled[1] = 0.0f;
	b.cmdNum = b.yawwingTick = 0;
	b.noPress = b.overlap = b.sawPress = b.sawTurn = false;
}

void ProcessRunCmd(const Move& m)
{
	Bgs& b = g_bgs;
	if (m.onGround) {
		b.ticksOnGround++;
		if (b.ticksOnGround >= BHOP_FRAMES) {
			if (b.ticksOnGround == BHOP_FRAMES) ResetJumpState();
			return;
		}
		if ((m.buttons & IN_JUMP) && b.ticksOnGround == 1) {
			b.ticksOnGround = 0;
			const float x = fabsf(m.origin[0] - b.lastJumpPosition[0]);
			const float y = fabsf(m.origin[1] - b.lastJumpPosition[1]);
			b.lastVeer = x >= y ? y : x;
		}
	} else {
		b.ticksOnGround = 0;
	}
	if (m.noStats) b.ticksOnGround = BHOP_FRAMES + 1;
}

void AngleVectors(const float a[3], float fore[3], float side[3])
{
	const float d = 3.14159265358979323846f / 180.0f;
	const float sp = sinf(a[0] * d), cp = cosf(a[0] * d);
	const float sy = sinf(a[1] * d), cy = cosf(a[1] * d);
	const float sr = sinf(a[2] * d), cr = cosf(a[2] * d);
	fore[0] = cp * cy; fore[1] = cp * sy; fore[2] = -sp;
	side[0] = -1 * sr * sp * cy + -1 * cr * -sy;
	side[1] = -1 * sr * sp * sy + -1 * cr * cy;
	side[2] = -1 * sr * cp;
}

float Normalize(float v[3])
{
	const float len = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
	if (len > 0.0f) { v[0] /= len; v[1] /= len; v[2] /= len; }
	return len;
}

void ProcessPostRunCmd(const float origin[3])
{
	Bgs& b = g_bgs;
	const Move& m = g_move;
	const float* velocity = m.velocity;
	const float vel[2] = { m.forward, m.side };
	const float yawDiff = NormalizeAngle(m.angles[1] - b.lastYaw);
	const float tick = TickInterval();
	const bool touchesWall = b.touchesWall;
	float jssThisTick = 0.0f, gaincoeff = 0.0f;

	if (b.ticksOnGround == 0) {
		if (origin[2] > b.jumpPeak) b.jumpPeak = origin[2];
		if (yawDiff != 0.0f) {
			const float perfJss = RadToDeg(atanf(30 / Speed2D(velocity)));
			const float finalJss = fabsf(yawDiff / perfJss);
			b.avgDiffFromPerf += finalJss;
			b.avgAbsoluteJss += 100 - fabsf((finalJss * 100.0f) - 100);
			jssThisTick = finalJss;
		}
		if (m.buttons & (IN_LEFT | IN_RIGHT)) b.yawwingTick++;

		if (b.cmdNum >= 1 && (vel[1] * b.lastNonZeroMove[1] < 0 || vel[0] * b.lastNonZeroMove[0] < 0)) {
			b.keyTick = b.cmdNum;
			b.strafeCount++;
			b.sawPress = true;
		}
		if (yawDiff > 0) {
			if (b.turnDir == 1 && b.cmdNum > 1) { b.turnTick = b.cmdNum; b.sawTurn = true; }
			b.turnDir = 0;
		} else if (yawDiff < 0) {
			if (b.turnDir == 0 && b.cmdNum > 1) { b.turnTick = b.cmdNum; b.sawTurn = true; }
			b.turnDir = 1;
		}

		bool overlapThisTick = false;
		if (((m.buttons & IN_MOVELEFT) && (m.buttons & IN_MOVERIGHT)) || ((m.buttons & IN_FORWARD) && (m.buttons & IN_BACK))) {
			b.overlap = true;
			overlapThisTick = true;
		}
		if (vel[1] == 0.0f && vel[0] == 0.0f && !overlapThisTick) b.noPress = true;
		if (b.cmdNum - b.turnTick >= 20) b.sawTurn = false;
		if (b.cmdNum - b.keyTick >= 20) b.sawPress = false;
		if (b.sawPress && b.sawTurn) {
			Offset(b.keyTick - b.turnTick, b.overlap, b.noPress);
			b.overlap = b.noPress = b.sawPress = b.sawTurn = false;
		}

		b.strafeTick++;
		b.traveled[0] += velocity[0] * tick * m.lagged;
		b.traveled[1] += velocity[1] * tick * m.lagged;
		b.trajectory += Speed2D(velocity) * tick * m.lagged;

		float fore[3], side[3];
		AngleVectors(m.angles, fore, side);
		fore[2] = 0.0f;
		Normalize(fore);
		side[2] = 0.0f;
		Normalize(side);
		float wishdir[3] = { fore[0] * vel[0] + side[0] * vel[1], fore[1] * vel[0] + side[1] * vel[1], 0.0f };
		float wishspeed = Normalize(wishdir);
		if (m.maxSpeed != 0.0f && wishspeed > m.maxSpeed) wishspeed = m.maxSpeed;
		if (wishspeed > 0.01f) {
			const float wishspd = wishspeed > 30.0f ? 30.0f : wishspeed;
			const float currentgain = velocity[0] * wishdir[0] + velocity[1] * wishdir[1] + velocity[2] * wishdir[2];
			if (currentgain < 30.0f) {
				gaincoeff = (wishspd - fabsf(currentgain)) / wishspd;
				b.syncedTick++;
			}
			if (touchesWall && gaincoeff > 0.5f) gaincoeff = fabsf(gaincoeff - 1.0f);
			b.rawGain += gaincoeff;
		}
		b.cmdNum++;
	}

	Trainer(b.ticksOnGround == 0, jssThisTick);
	b.touchesWall = false;

	if (b.jumpedThisTick) {
		b.lastJumpPosition[0] = origin[0];
		b.lastJumpPosition[1] = origin[1];
		b.lastJumpPosition[2] = origin[2];
		b.jumpedThisTick = false;
		if (b.jump == 1) {
			FirstJumpForward(RoundToFloor(Speed2D(velocity)));
		} else {
			const int strafeTick = b.strafeTick;
			float coeffsum = b.rawGain;
			coeffsum /= strafeTick;
			coeffsum *= 100.0f;
			float distance = sqrtf(b.traveled[0] * b.traveled[0] + b.traveled[1] * b.traveled[1]);
			if (distance > b.trajectory) distance = b.trajectory;
			float efficiency = 0.0f;
			if (distance > 0.0f) efficiency = coeffsum * distance / b.trajectory;
			coeffsum = RoundToFloor(coeffsum * 100.0f + 0.5f) / 100.0f;
			efficiency = RoundToFloor(efficiency * 100.0f + 0.5f) / 100.0f;
			JumpForward(b.jump, RoundToFloor(Speed2D(velocity)), b.strafeCount, b.jumpPeak - b.oldHeight, origin[2] - b.oldHeight,
				coeffsum, 100.0f * b.syncedTick / strafeTick, efficiency, b.avgDiffFromPerf / strafeTick, b.avgAbsoluteJss / strafeTick);
		}
		b.rawGain = 0.0f;
		b.strafeTick = b.syncedTick = b.strafeCount = 0;
		b.oldHeight = origin[2];
		b.trajectory = 0.0f;
		b.traveled[0] = b.traveled[1] = 0.0f;
		b.avgDiffFromPerf = b.avgAbsoluteJss = 0.0f;
	}

	b.lastYaw = m.angles[1];
	if (vel[0] != 0.0f || vel[1] != 0.0f) {
		b.lastNonZeroMove[0] = vel[0];
		b.lastNonZeroMove[1] = vel[1];
	}
}

bool Hooked_AddToTouched(void* self, const void* trace, const float* velocity)
{
	if (g_inMove && trace && static_cast<const etrace::GameTraceX*>(trace)->ent) g_touched = true;
	return oAddToTouched(self, trace, velocity);
}

bool OurSession(const CCommandX& c, int minArgs)
{
	return c.argc >= minArgs && atoi(c.argv[1]) == g_session && g_session != 0;
}

int HexDigit(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

}  // namespace

void Reset(int session)
{
	g_session = session;
	g_on = false;
	g_colorsLen = -1;
	g_prefix.clear();
	g_text.clear();
	g_variable.clear();
	g_hasComparison = false;
	g_haveTime = false;
	g_hudMsg = g_textMsg = -1;
}

void SetChat(int client, int sayText2)
{
	g_client = client;
	g_sayText2 = sayText2;
}

// bhoptools_jsset <session> <on> <cvars> <channel groups 0-5> <settings 0-14 as %x>
void Cmd_Settings(const CCommandX& c)
{
	if (!OurSession(c, 3)) return;
	g_on = atoi(c.argv[2]) != 0;
	if (!g_on || c.argc < 4 + 6 + kSettings) return;
	g_cvars = atoi(c.argv[3]);
	for (int i = 0; i < 6; ++i) g_channels[i] = atoi(c.argv[4 + i]);
	for (int i = 0; i < kSettings; ++i) g_set[i] = (int)(uint32_t)strtoul(c.argv[10 + i], nullptr, 16);
}

// bhoptools_jscc <session> <length> <start> "<chunk>": ssj's prefix, text and variable colours joined
// by newlines, percent-encoded like bhoptools_chat.
void Cmd_Colors(const CCommandX& c)
{
	if (!OurSession(c, 5)) return;
	const int total = atoi(c.argv[2]), start = atoi(c.argv[3]);
	if (total < 0 || total > 1024) return;
	if (start == 0) { g_colorsIn.clear(); g_colorsLen = total; }
	if (g_colorsLen != total || (int)g_colorsIn.size() != start) return;
	for (const char* p = c.argv[4]; *p && (int)g_colorsIn.size() < total; ++p) {
		int b = (unsigned char)*p;
		if (b == '%' && HexDigit(p[1]) >= 0 && HexDigit(p[2]) >= 0) {
			b = HexDigit(p[1]) * 16 + HexDigit(p[2]);
			p += 2;
		}
		g_colorsIn += (char)b;
	}
	if ((int)g_colorsIn.size() != total) return;
	const size_t a = g_colorsIn.find('\n');
	const size_t b = a == std::string::npos ? a : g_colorsIn.find('\n', a + 1);
	if (b == std::string::npos) return;
	g_prefix = g_colorsIn.substr(0, a);
	g_text = g_colorsIn.substr(a + 1, b - a - 1);
	g_variable = g_colorsIn.substr(b + 1);
}

// bhoptools_jscmp <session> <has> <difference bits>: shavit's replay speed comparison.
void Cmd_Comparison(const CCommandX& c)
{
	if (!OurSession(c, 4)) return;
	g_hasComparison = atoi(c.argv[2]) != 0;
	const uint32_t bits = (uint32_t)strtoul(c.argv[3], nullptr, 16);
	memcpy(&g_comparison, &bits, 4);
}

// bhoptools_jsjump <session> <jump>: the jump count of the checkpoint shavit teleported us to.
void Cmd_Jump(const CCommandX& c)
{
	if (!OurSession(c, 3)) return;
	const int jump = atoi(c.argv[2]);
	if (jump >= 0) g_bgs.jump = jump;
}

// bhoptools_jstime <session> <cmd> <time bits> <per command bits>: shavit's time after <cmd>, sent when it
// stops adding the same amount each command.
void Cmd_Time(const CCommandX& c)
{
	if (!OurSession(c, 5)) return;
	uint32_t bits[2] = { (uint32_t)strtoul(c.argv[3], nullptr, 16), (uint32_t)strtoul(c.argv[4], nullptr, 16) };
	float f[2];
	memcpy(f, bits, sizeof(f));
	g_timeCmd = atoi(c.argv[2]);
	g_time = f[0];
	g_timeRate = f[1];
	g_haveTime = true;
}

void Attach(void* moveHelper)
{
	if (!moveHelper || g_touchSlot) return;
	void** slot = &(*reinterpret_cast<void***>(moveHelper))[2];
	oAddToTouched = reinterpret_cast<AddToTouchedFn>(*slot);
	if (PatchPointer(slot, reinterpret_cast<void*>(&Hooked_AddToTouched))) g_touchSlot = slot;
	else CoreLog("jumpstats: couldn't hook AddToTouched, wall touches don't count for gain");
}

#ifdef _WIN32
void Detach()
{
	if (g_touchSlot) PatchPointer(g_touchSlot, reinterpret_cast<void*>(oAddToTouched));
	g_touchSlot = nullptr;
}
#endif

void PreMove(const Move& m)
{
	g_move = m;
	g_inMove = true;
	g_touched = false;
	if (!g_on) return;
	ShowKeys(m.buttons, m.angles[1]);
	ProcessRunCmd(m);
}

void OnJump()
{
	if (!g_inMove || !g_on) return;
	Bgs& b = g_bgs;
	if (b.jump > 0 && b.strafeTick == 0) return;
	b.jump++;
	b.jumpedThisTick = true;
}

void PostMove(const float origin[3], const float velocity[3])
{
	g_inMove = false;
	if (!g_on) {
		g_bgs.jumpedThisTick = false;
		return;
	}
	if (g_touched) g_bgs.touchesWall = true;
	ProcessPostRunCmd(origin);
	Speedometer(velocity);
	ShowYaws(velocity, g_move.angles[1]);
}

void OnTeleport()
{
	g_bgs.cmdNum = 0;
}

}
