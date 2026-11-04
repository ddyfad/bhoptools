// A port of bhop-get-stats 1.5 and the bgs-jumpstats 3.9 modules that draw for the player themselves,
// as deployed (bhop-get-stats 5467a29, jumpstats b2a90d6), plus the real gain, gain modes, compact trainers and
// ground prestrafe trainer of bhop-get-stats branch sj-beta when the plugin says so. Float math is kept in float, as
// SourcePawn does it, so the numbers come out the same.
#include "jumpstats.h"
#include "speedo.h"
#include "teleport_predict.h"
#include "engine_trace_layout.h"
#include "hudkeys.h"
#include "game_layout.h"
#include "settings.h"
#include "sigs.h"
#include "utils.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

namespace js {
namespace {

// bgs-jumpstats' settings indexes and bits (jumpstat/settings.sp)
enum { GainReallyBad, GainBad, GainMeh, GainGood, GainReallyGood, Bools, Usage, Positions_X, Positions_Y,
	Positions2_X, Positions2_Y, TrainerSpeed, JhudCutOff, JhudSpeedColorsJump, JhudHidden, Bools2, kSettings };
enum { HudJhud, HudTrainer, HudOffset, HudSpeedometer, HudFjt, HudShowKeys, HudPreStrafeTrainer, HudReversal };
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
// Bools2. MMOD_GAIN shows the mmod gain, BOTH_GAIN "old / mmod", neither the old gain. Bits 5-8 are the compact
// trainers' colour, bits 11-12 the reversal close-up's ticks either side (an index into kReversalTicks).
const int PRESTRAFETRAINER_ENABLED = 1 << 0, MMOD_GAIN = 1 << 1, BOTH_GAIN = 1 << 2, TRAINER_COMPACT = 1 << 3,
	TRAINER_PCT_BELOW = 1 << 4, TRAINER_COLOUR_SHIFT = 5, PRETRAINER_COMPACT = 1 << 9, REVERSAL_ENABLED = 1 << 10,
	REVERSAL_TICKS_SHIFT = 11;
const int TrainerColour_Rainbow = 9;

// bhoptools_jsset cvar bits, which js-enabled-* and js-allow-trainer-* are on. The last two aren't cvars:
// JSCV_REALGAIN is an older plugin's real gain, shown in place of the old one. JSCV_GAINMODES is bhop-get-stats
// with both gains and Bools2 picking what's shown, sent to clients that say they draw it.
const int JSCV_JHUD = 1 << 0, JSCV_TRAINER = 1 << 1, JSCV_TRAINER_MEDIUM = 1 << 2, JSCV_TRAINER_FAST = 1 << 3,
	JSCV_OFFSET = 1 << 4, JSCV_SPEEDOMETER = 1 << 5, JSCV_SSJ = 1 << 6, JSCV_SHOWKEYS = 1 << 7, JSCV_SHOWYAWS = 1 << 8,
	JSCV_PRETRAINER = 1 << 9, JSCV_REALGAIN = 1 << 10, JSCV_GAINMODES = 1 << 11, JSCV_REVERSAL = 1 << 12;

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
const float PRETRAINER_FROM = 0.93f;
const float PRETRAINER_WHITE = 1.5f;
const int IN_JUMP = 1 << 1, IN_DUCK = 1 << 2, IN_FORWARD = 1 << 3, IN_BACK = 1 << 4, IN_LEFT = 1 << 7, IN_RIGHT = 1 << 8,
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
// sv_accelerate, sv_friction and sv_stopspeed, for the prestrafe trainer
bool  g_haveMoveCvars;
float g_accelerate, g_friction, g_stopspeed;
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
bool Has2(int bit) { return (g_set[Bools2] & bit) != 0; }
// Which gain is shown, as MMOD_GAIN and BOTH_GAIN: an older plugin's real gain stands in for the old one.
int GainBits()
{
	if (Cvar(JSCV_GAINMODES)) return g_set[Bools2] & (MMOD_GAIN | BOTH_GAIN);
	return Cvar(JSCV_REALGAIN) ? MMOD_GAIN : 0;
}
const int* Color(int setting)
{
	const int c = setting >= 0 && setting < kSettings ? g_set[setting] : 0;
	return kColors[c >= 0 && c < 9 ? c : 0];
}

// bhop-get-stats
struct Bgs {
	bool  touchesWall, jumpedThisTick, overlap, noPress, sawTurn, sawPress;
	int   ticksOnGround, strafeTick, syncedTick, jump, strafeCount, turnTick, keyTick, turnDir, cmdNum, yawwingTick;
	float oldHeight, rawGain, mmodGain, idealGain, trajectory, traveled[2], lastYaw, avgDiffFromPerf, avgAbsoluteJss;
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
// The compact strafe trainer's smoothed jss and the compact prestrafe trainer's smoothed value and pace; jss and
// value are -1 until the first sample.
float g_compactJss = -1.0f, g_compactPreValue = -1.0f, g_compactPrePace;
// The prestrafe trainer's sums over the trainer speed.
float g_preValueSum, g_prePaceSum;
int   g_preSamples;
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

// colors.sp's GetGainColorIdx, for the mmod gain: the colour of the old gain it converts to (per tick
// mmod = 1 - (1 - old)^2). An older plugin's real gain had steps of its own.
int GainColorIdx(float gain)
{
	if (Cvar(JSCV_GAINMODES)) {
		if (gain > 100.0f) return GainReallyBad;
		return PercentageColorIdx(100.0f * (1.0f - sqrtf(1.0f - gain / 100.0f)), true);
	}
	if (gain >= 93.0f) return GainReallyGood;
	if (gain >= 88.0f) return GainGood;
	if (gain >= 85.0f) return GainMeh;
	if (gain >= 83.0f) return GainBad;
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

void Jhud(int jump, int speed, float gain, float mmod, float sync, float jss, float absJss)
{
	if (!Cvar(JSCV_JHUD) || !Has(JHUD_ENABLED)) return;
	if (jump > g_set[JhudCutOff] && g_set[JhudCutOff] != 0) return;

	const int bits = GainBits();
	// one colour per hud line, so "both" is coloured by the old gain
	int idx = jump <= 16 && jump <= g_set[JhudSpeedColorsJump] ? SpeedColorIdx(jump, speed) : (bits & MMOD_GAIN) ? GainColorIdx(mmod) : PercentageColorIdx(gain, true);
	const int hidden = g_set[JhudHidden];
	// jss only: colour by how far jss is from 100%
	if (jump > 1 && (hidden & JHUD_HIDE_SPEED) && (hidden & JHUD_HIDE_GAIN) && !Has(JHUD_SYNC) && (BoolSet() & (JHUD_JSS | JHUD_GMOD_JSS))) {
		const float off = Cvar(JSCV_REALGAIN | JSCV_GAINMODES) ? 100.0f * sqrtf(1.0f - absJss / 100.0f) : fabsf(jss * 100.0f - 100.0f);
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
		// with gain modes the mmod gain has three decimals; "both" only comes with them
		if (!(hidden & JHUD_HIDE_GAIN))
			stats = (bits & BOTH_GAIN) ? Fmt("%.2f / %.3f", gain, mmod) : !(bits & MMOD_GAIN) ? Fmt("%.2f", gain) : Cvar(JSCV_GAINMODES) ? Fmt("%.3f", mmod) : Fmt("%.2f", mmod);
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

void SsjWrite(int jump, int speed, int strafes, float maxHeight, float heightDelta, float gain, float mmod, float sync, float eff, float jss, float time)
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
		// each gain in its own colour; with gain modes the mmod gain has three decimals
		auto append = [&](float g, int idx, bool three) {
			const int c = g_set[idx];
			const char* hex = kColorsHex[c >= 0 && c < 9 ? c : 0];
			m += !decimals ? Fmt("%s%i%%", hex, RoundToFloor(g)) : three ? Fmt("%s%.3f%%", hex, g) : Fmt("%s%.2f%%", hex, g);
		};
		const int bits = GainBits();
		m += Fmt(" %s| G: ", t);
		if (!(bits & MMOD_GAIN)) append(gain, PercentageColorIdx(gain, true), false);
		if (bits & BOTH_GAIN) m += Fmt(" %s/ ", t);
		if (bits & (MMOD_GAIN | BOTH_GAIN)) append(mmod, GainColorIdx(mmod), Cvar(JSCV_GAINMODES));
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

void Ssj(int jump, int speed, int strafes, float maxHeight, float heightDelta, float gain, float mmod, float sync, float eff, float jss)
{
	if (!Cvar(JSCV_SSJ)) return;
	const float time = ShavitTime();
	if (Has(SSJ_ENABLED)) SsjWrite(jump, speed, strafes, maxHeight, heightDelta, gain, mmod, sync, eff, jss, time);
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

// h in 0-6, full saturation and value
void Hue(float h, int rgb[3])
{
	const float f = h - floorf(h);
	const int up = RoundFloat(255.0f * f), down = 255 - up;
	const int c[6][3] = { { 255, up, 0 }, { down, 255, 0 }, { 0, 255, up }, { 0, down, 255 }, { up, 0, 255 }, { 255, 0, down } };
	memcpy(rgb, c[(int)floorf(h) % 6], sizeof(c[0]));
}

void CompactColour(float value, int rgb[3])
{
	static const int fixed[8][3] = { { 255, 0, 0 }, { 255, 255, 0 }, { 0, 255, 0 }, { 0, 0, 255 }, { 255, 0, 255 }, { 128, 0, 255 }, { 255, 0, 128 }, { 15, 15, 15 } };
	const int mode = (g_set[Bools2] >> TRAINER_COLOUR_SHIFT) & 15;
	if (mode == TrainerColour_Rainbow) {
		// the plugin cycles on the game time; which colour it starts on doesn't matter
		const double t = MonotonicSeconds() / 6.0;
		Hue((float)(t - floor(t)) * 6.0f, rgb);
	} else if (mode > 0 && mode < TrainerColour_Rainbow) {
		memcpy(rgb, fixed[mode - 1], sizeof(fixed[0]));
	} else {
		// white at 100%, then cyan, green, orange, red the further off it is
		static const int stops[5][3] = { { 255, 255, 255 }, { 0, 255, 255 }, { 0, 255, 0 }, { 255, 165, 0 }, { 255, 0, 0 } };
		static const float at[5] = { 0.0f, 0.15f, 0.35f, 0.65f, 1.0f };
		const float t = fabsf(value - 100.0f) / (value < 100.0f ? 99.0f : 100.0f);
		int i = 0;
		while (i < 3 && t > at[i + 1]) ++i;
		const float f = (t - at[i]) / (at[i + 1] - at[i]);
		for (int c = 0; c < 3; ++c) rgb[c] = RoundFloat(stops[i][c] + (stops[i + 1][c] - stops[i][c]) * f);
	}
}

// What a per-frame HUD last showed, so an unchanged one isn't sent again every frame.
struct Shown {
	std::string text;
	int rgb[3];
	double at;
};
// An unchanged per-frame HUD is still sent this often, to stay up for its hold time.
const double kFrameRefresh = 0.1;

// trainer.sp's compact look, from 2x74/strafetrainer: a label over a one line gauge, 1-200%, 100% in the middle.
void DrawCompact(int hud, float x, float value, float hold, int group, const std::string& label, Shown* shown = nullptr)
{
	value = value < 1.0f ? 1.0f : value > 200.0f ? 200.0f : value;
	const int marker = RoundFloat((value - 1.0f) / 199.0f * 20.0f);
	std::string bar;
	for (int i = 0; i < 21; ++i) bar += i == marker ? 'o' : i == 10 ? '|' : '-';
	int rgb[3];
	CompactColour(value, rgb);
	const std::string text = Has2(TRAINER_PCT_BELOW) ? "[" + bar + "]\n" + label : label + "\n[" + bar + "]";
	if (shown) {
		const double now = MonotonicSeconds();
		if (text == shown->text && !memcmp(rgb, shown->rgb, sizeof(rgb)) && now - shown->at < kFrameRefresh) return;
		shown->text = text;
		memcpy(shown->rgb, rgb, sizeof(rgb));
		shown->at = now;
	}
	ShowHud(hud, x, rgb, hold, group, text);
}

// The compact trainers are drawn from values smoothed over time, as 2x74/strafetrainer does, where the plugin can
// only send a trainer window's average. This is how far `dt` seconds move them, by trainer speed.
float CompactStep(int which, float dt)
{
	static const float tau[3] = { 0.15f, 0.1f, 0.06f };
	return 1.0f - expf(-dt / tau[which]);
}
const float kCompactHold = 0.1f, kFrameHold = 0.2f;

// Per frame, each compact trainer keeps its samples from the last two ticks and is drawn in between them,
// smoothed on the frame time. Values are jss for the strafe trainer, value, pace and top for the prestrafe one.
struct FrameTrack {
	bool   live;
	double at;  // when the last sample came
	float  prev[3], last[3];
	float  shown[3];  // the first two smoothed, the third as it is
	int    which, group;
	Shown  drawn;
};
FrameTrack g_frameStrafe, g_framePre;
double g_lastFrame = -1.0;

// Frames come from the IN_SetSampleTime hook; without them it stays per tick.
bool PerFrame()
{
	return settings::Get().jsframe && g_lastFrame >= 0.0 && MonotonicSeconds() - g_lastFrame < 0.5;
}

void FrameSample(FrameTrack& t, const float v[3], int which, int group)
{
	if (!t.live) {
		memcpy(t.last, v, sizeof(t.last));
		memcpy(t.shown, v, sizeof(t.shown));
		t.drawn.at = -1.0;
		t.live = true;
	}
	memcpy(t.prev, t.last, sizeof(t.prev));
	memcpy(t.last, v, sizeof(t.last));
	t.at = MonotonicSeconds();
	t.which = which;
	t.group = group;
}

int TrainerSpeedIdx()
{
	return g_set[TrainerSpeed] >= 0 && g_set[TrainerSpeed] < 3 ? g_set[TrainerSpeed] : Trainer_Slow;
}

void TrainerSpeeds(int speeds[3])
{
	const int rate = TickRate();
	speeds[0] = RoundToFloor(rate * (TRAINER_FULLUPDATE_TICK_INTERVAL / 100.0f));
	speeds[1] = RoundToFloor(rate * 0.09f);
	speeds[2] = RoundToFloor(rate * 0.05f);
}

void Trainer(bool inbhop, float jss)
{
	if (!Cvar(JSCV_TRAINER)) return;
	++g_trainerCmd;
	if (!inbhop) {
		g_trainerNumber = 0.0f;
		g_trainerBar[0] = g_trainerBar[1] = g_trainerBar[2] = 0.0f;
		g_compactJss = -1.0f;
		g_frameStrafe.live = false;
		return;
	}
	g_trainerNumber += jss;
	for (float& b : g_trainerBar) b += jss;

	const int which = TrainerSpeedIdx();
	const bool compact = Has(TRAINER_ENABLED) && Has2(TRAINER_COMPACT);
	if (compact && PerFrame()) {
		const float v[3] = { jss * 100.0f, 0.0f, 0.0f };
		FrameSample(g_frameStrafe, v, which, 0);
	} else if (compact) {
		const float sample = jss * 100.0f;
		g_compactJss = g_compactJss < 0.0f ? sample : g_compactJss + (sample - g_compactJss) * CompactStep(which, TickInterval());
		DrawCompact(HudTrainer, -1.0f, g_compactJss, kCompactHold, 0, Fmt("%.1f%%", g_compactJss));
		g_frameStrafe.live = false;
	} else {
		g_frameStrafe.live = false;
	}

	const int rate = TickRate();
	int speeds[3];
	TrainerSpeeds(speeds);
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

	if (compact || !Has(TRAINER_ENABLED) || speeds[which] <= 0 || cmd % speeds[which] != 0 || avg[which] == -1.0f) return;
	const int idx = PercentageColorIdx(avg[which] * 100, Has(TRAINER_STRICT));
	const float hold = speeds[which] / (rate * 1.0f) + 0.05f;
	ShowHud(HudTrainer, -1.0f, Color(idx), hold, 0, TrainerString(g_lastAverage, avg[which]));
}

// pretrainer.sp: the speed v that a tick getting `share` of its most leaves at v: v = after + share * (most - after),
// with after = v * (1 - loss). Under sv_stopspeed is left out, far below where the trainer shows.
float PrePace(float share, float push, float best, float loss)
{
	if (share <= 0.0f) return 0.0f;
	const float keep = 1.0f - loss;
	float pace = share * push / loss;
	if (keep * pace > best) pace = share * sqrtf((2.0f * push * best + push * push) / (loss * (loss + 2.0f * share * keep)));
	return pace;
}

// pretrainer.sp: this ground tick's speed gain over the most it could gain, as Friction then Accelerate give it.
// 1 is the most, under is the wish direction too close to the velocity, over is the same loss with it too far
// off; -1 without move keys. pace is the top speed for this tick's share every tick, top the same at the most.
float PreValue(const float velocity[3], float forward, float side, float yaw, float maxspeed, float accelerate, float friction, float stopspeed, float frametime, float& pace, float& top)
{
	float wishspeed = sqrtf(forward * forward + side * side);
	if (maxspeed != 0.0f && wishspeed > maxspeed) wishspeed = maxspeed;
	const float push = accelerate * frametime * wishspeed;
	if (wishspeed < 0.01f || push <= 0.0f) return -1.0f;

	const float speed = sqrtf(velocity[0] * velocity[0] + velocity[1] * velocity[1]);
	float after = speed;
	if (speed >= 0.1f) {
		const float control = speed < stopspeed ? stopspeed : speed;
		after = speed - control * friction * frametime;
		if (after < 0.0f) after = 0.0f;
	}

	const float rad = (yaw * 3.14159265358979323846f) / 180.0f;
	const float wishx = cosf(rad) * forward + sinf(rad) * side;
	const float wishy = sinf(rad) * forward - cosf(rad) * side;
	float along = 0.0f;
	if (speed > 0.0f) along = (velocity[0] * wishx + velocity[1] * wishy) / sqrtf(wishx * wishx + wishy * wishy) * (after / speed);

	float gained = after;
	if (along < wishspeed) {
		float add = wishspeed - along;
		if (add > push) add = push;
		gained = sqrtf(after * after + 2.0f * add * along + add * add);
	}
	float best = wishspeed - push;
	if (best < 0.0f) best = 0.0f;
	const float most = after <= best ? after + push : sqrtf(after * after + 2.0f * push * best + push * push);

	const float share = (gained - after) / (most - after);
	pace = PrePace(share, push, best, friction * frametime);
	top = PrePace(1.0f, push, best, friction * frametime);
	return along > best ? share : 2.0f - share;
}

void PreTrainerReset()
{
	g_preValueSum = g_prePaceSum = 0.0f;
	g_preSamples = 0;
	g_compactPreValue = -1.0f;
	g_framePre.live = false;
}

// the compact gauge's percent: each colour step is PRETRAINER_WHITE off the top, left turning too slowly
float PrePercent(float value, float pace, float top)
{
	const float off = (top - pace) / PRETRAINER_WHITE;
	return value > 1.0f ? 100.0f + off * 5.0f : 100.0f - off * 10.0f;
}

// From PRETRAINER_FROM of the best top speed: the top speed each tick is on pace for, averaged over the trainer
// speed and drawn once a window, so the side only changes when the average does. The bar is left of centre turning
// too slowly, right too fast; each colour step is PRETRAINER_WHITE on both sides.
void PreTrainer(bool inbhop)
{
	if (!Cvar(JSCV_PRETRAINER) || !Has2(PRESTRAFETRAINER_ENABLED) || !g_haveMoveCvars) return;
	const Move& m = g_move;
	if (inbhop || (m.buttons & (IN_JUMP | IN_DUCK)) || m.noStats || m.stamina > 0.0f) {
		PreTrainerReset();
		return;
	}
	float pace, top;
	const float value = PreValue(m.velocity, m.forward, m.side, m.angles[1], m.maxSpeed, g_accelerate * m.surfaceFriction, g_friction * m.surfaceFriction, g_stopspeed, TickInterval() * m.lagged, pace, top);
	if (value < 0.0f || sqrtf(m.velocity[0] * m.velocity[0] + m.velocity[1] * m.velocity[1]) < top * PRETRAINER_FROM) {
		PreTrainerReset();
		return;
	}

	const int which = TrainerSpeedIdx();
	// the first group whose own HUD is off, else 5
	const int group = !Has(TRAINER_ENABLED) ? 0 : !Has(JHUD_ENABLED) ? 1 : !Has(OFFSETS_ENABLED) ? 2 : !Has(SHOWKEYS_ENABLED) ? 3 : !Has(SPEEDOMETER_ENABLED) ? 4 : 5;
	if (Has2(PRETRAINER_COMPACT) && PerFrame()) {
		const float v[3] = { value, pace, top };
		FrameSample(g_framePre, v, which, group);
		return;
	}
	g_framePre.live = false;
	if (Has2(PRETRAINER_COMPACT)) {
		if (g_compactPreValue < 0.0f) {
			g_compactPreValue = value;
			g_compactPrePace = pace;
		} else {
			const float k = CompactStep(which, TickInterval());
			g_compactPreValue += (value - g_compactPreValue) * k;
			g_compactPrePace += (pace - g_compactPrePace) * k;
		}
		DrawCompact(HudPreStrafeTrainer, HudCoord(HudPreStrafeTrainer, 0), PrePercent(g_compactPreValue, g_compactPrePace, top), kCompactHold, group, Fmt("%i", RoundFloat(g_compactPrePace)));
		return;
	}

	int speeds[3];
	TrainerSpeeds(speeds);
	const int window = speeds[which] > 0 ? speeds[which] : 1;
	g_preValueSum += value;
	g_prePaceSum += pace;
	if (++g_preSamples % window != 0) return;
	const float avgValue = g_preValueSum / window, avgPace = g_prePaceSum / window;
	g_preValueSum = g_prePaceSum = 0.0f;

	const float off = (top - avgPace) / PRETRAINER_WHITE;
	const bool fast = avgValue > 1.0f;
	const float percent = fast ? 100.0f + off * 5.0f : 100.0f - off * 10.0f;
	const int idx = PercentageColorIdx(percent, Has(TRAINER_STRICT));
	const float hold = window / (TickRate() * 1.0f) + 0.05f;
	ShowHud(HudPreStrafeTrainer, Color(idx), hold, group, TrainerString(avgPace / 100.0f, fast ? 1.0f + off * 0.1f : 1.0f - off * 0.1f));
}

// reversal.sp, the reversal close-up after the ok.tools loss trainer: each air tick's turn as a share of the best
// (bhop-get-stats' jss), around the last time the turn changed direction. A tick turning under kReversalSlow deg/s
// has no direction, so a reversal can cross a few of them, and those are kept in front of the flip. A capture is
// shown once its ticks after the flip are in (or the next flip or the ground cuts it short) and stays up until the
// next one. Drawn as bars when the engine's top panel can be painted on, else as the plugin's text.
const int kReversalTicks[4] = { 10, 15, 20, 6 };
const float kReversalSlow = 5.0f;
const size_t kReversalKeep = 256;
// the plugin's text: a HudMsg holds about 220 bytes and a column is three 3-byte glyphs
const int kReversalTextSide = 11, kReversalTextTicks = 22;
const double kReversalResend = 1.0;

struct RevTick { float share; int dir; bool slow; };
struct Reversal {
	int dir, slow, gap, following;
	bool recording, drawn;
	std::deque<RevTick> active, before, after;
	std::vector<RevTick> shownBefore, shownAfter;
	std::string text;
	double sent;
} g_rev;
double g_revPainted = -1.0;  // the last paint of the engine's top panel, which draws the bars

int ReversalTicks() { return kReversalTicks[(g_set[Bools2] >> REVERSAL_TICKS_SHIFT) & 3]; }
bool ReversalOn() { return g_on && Cvar(JSCV_REVERSAL) && Has2(REVERSAL_ENABLED); }
bool ReversalBars() { return g_revPainted >= 0.0 && MonotonicSeconds() - g_revPainted < 0.5; }

// The first group whose own HUD is off and the prestrafe trainer didn't take, else 5.
int ReversalGroup()
{
	const uint32_t huds[5] = { TRAINER_ENABLED, JHUD_ENABLED, OFFSETS_ENABLED, SHOWKEYS_ENABLED, SPEEDOMETER_ENABLED };
	bool pretrainer = Cvar(JSCV_PRETRAINER) && Has2(PRESTRAFETRAINER_ENABLED);
	for (int i = 0; i < 5; ++i) {
		if (Has(huds[i])) continue;
		if (pretrainer) {
			pretrainer = false;
			continue;
		}
		return i;
	}
	return 5;
}

void ReversalBreak()
{
	g_rev.dir = g_rev.slow = 0;
	g_rev.active.clear();
	g_rev.recording = false;
}

// Bars two rows tall under a line at the best turn, the flip a column between; overturns stop at full.
void ReversalText()
{
	const int after = (std::min)((int)g_rev.shownAfter.size(), kReversalTextSide);
	const int before = (std::min)((int)g_rev.shownBefore.size(), kReversalTextTicks - after);
	// U+2581..U+2588, an eighth to a full block; U+2591 light shade for an empty cell, so every cell is a block wide
	static const char* const blocks[9] = { "\xE2\x96\x91", "\xE2\x96\x81", "\xE2\x96\x82", "\xE2\x96\x83", "\xE2\x96\x84",
		"\xE2\x96\x85", "\xE2\x96\x86", "\xE2\x96\x87", "\xE2\x96\x88" };
	std::string rows[3];
	auto column = [&](float share) {
		const int level = RoundFloat((share > 1.0f ? 1.0f : share) * 16.0f);
		rows[0] += "\xE2\x94\x80";
		rows[1] += blocks[level > 8 ? level - 8 : 0];
		rows[2] += blocks[level > 8 ? 8 : level];
	};
	for (size_t i = g_rev.shownBefore.size() - before; i < g_rev.shownBefore.size(); ++i) column(g_rev.shownBefore[i].share);
	for (std::string& r : rows) r += "\xE2\x94\x82";
	for (int i = 0; i < after; ++i) column(g_rev.shownAfter[i].share);
	g_rev.text = rows[0] + "\n" + rows[1] + "\n" + rows[2];
}

void ReversalShow()
{
	g_rev.shownBefore.assign(g_rev.before.begin(), g_rev.before.end());
	g_rev.shownAfter.assign(g_rev.after.begin(), g_rev.after.end());
	g_rev.drawn = true;
	ReversalText();
	g_rev.sent = -1.0;
}

// A capture the next flip or the ground cut short is still shown, with the ticks after it that came.
void ReversalCutShort()
{
	if (g_rev.recording && !g_rev.drawn && !g_rev.after.empty()) ReversalShow();
}

// Without the bars, the text is sent again each kReversalResend to stay up.
void ReversalKeepUp()
{
	if (g_rev.text.empty() || !ReversalOn() || ReversalBars()) return;
	const double now = MonotonicSeconds();
	if (g_rev.sent >= 0.0 && now - g_rev.sent < kReversalResend) return;
	g_rev.sent = now;
	static const int white[3] = { 255, 255, 255 };
	ShowHud(HudReversal, white, (float)kReversalResend + 0.25f, ReversalGroup(), g_rev.text);
}

void ReversalTick(bool inbhop, float yawDiff, float speed)
{
	if (!Cvar(JSCV_REVERSAL)) return;
	// a turn this big in one tick is a teleport setting the angles
	if (!inbhop || fabsf(yawDiff) > 90.0f) {
		ReversalCutShort();
		ReversalBreak();
		ReversalKeepUp();
		return;
	}
	const bool slow = fabsf(yawDiff) <= kReversalSlow * TickInterval();
	const int next = slow ? 0 : yawDiff > 0.0f ? 1 : -1;
	bool reversed = false;
	if (!next) {
		if (g_rev.dir) ++g_rev.slow;
	} else {
		reversed = g_rev.dir && next != g_rev.dir;
		if (reversed) g_rev.gap = g_rev.slow;
		g_rev.dir = next;
		g_rev.slow = 0;
	}
	if (!g_rev.dir) {
		ReversalKeepUp();
		return;
	}

	if (reversed) {
		ReversalCutShort();
		const size_t keep = (std::min)((size_t)(ReversalTicks() + g_rev.gap), g_rev.active.size());
		g_rev.before.assign(g_rev.active.end() - keep, g_rev.active.end());
		g_rev.active.clear();
		g_rev.after.clear();
		g_rev.following = ReversalTicks();
		g_rev.recording = true;
		g_rev.drawn = false;
	}

	const RevTick tick = { yawDiff != 0.0f ? fabsf(yawDiff / RadToDeg(atanf(30 / speed))) : 0.0f, next, slow };
	g_rev.active.push_back(tick);
	if (g_rev.active.size() > kReversalKeep) g_rev.active.pop_front();
	if (g_rev.recording && (int)g_rev.after.size() < g_rev.following) {
		g_rev.after.push_back(tick);
		if ((int)g_rev.after.size() == g_rev.following) ReversalShow();
	}
	ReversalKeepUp();
}

// ok.tools' close-up: before the flip on the left half, after on the right, left turns green, right blue, slow
// ticks amber over a faint amber band, the best turn a red line with room above it for overturns.
typedef void (*Int4Fn)(void*, int, int, int, int);
typedef void (*ScreenSizeFn)(void*, int&, int&);
const int kVT_IVEngineClient_GetScreenSize = 5;
void* g_revSurface;
void* g_revEngine;

void RevRect(int x0, int y0, int x1, int y1, int r, int g, int b, int a)
{
	VFunc<Int4Fn>(g_revSurface, VT_Surface_DrawSetColor)(g_revSurface, r, g, b, a);
	VFunc<Int4Fn>(g_revSurface, VT_Surface_DrawFilledRect)(g_revSurface, x0, y0, x1, y1);
}

void ReversalPaint()
{
	g_revPainted = MonotonicSeconds();
	if (!ReversalOn() || (g_rev.shownBefore.empty() && g_rev.shownAfter.empty())) return;
	int sw = 0, sh = 0;
	VFunc<ScreenSizeFn>(g_revEngine, kVT_IVEngineClient_GetScreenSize)(g_revEngine, sw, sh);
	const int w = sw * 2 / 5, h = sh / 10;
	if (w < 20 || h < 8) return;
	const float hx = HudCoord(HudReversal, 0), hy = HudCoord(HudReversal, 1);
	const int x = hx < 0.0f ? (sw - w) / 2 : (std::min)((int)(hx * sw), sw - w);
	const int y = hy < 0.0f ? (sh - h) / 2 : (std::min)((int)(hy * sh), sh - h);
	const int amplitude = h * 3 / 4, baseline = y + h;

	RevRect(x, y, x + w, y + h, 15, 18, 24, 175);
	auto bars = [&](const std::vector<RevTick>& ticks, int left, int width) {
		const int n = (std::max)(1, (int)ticks.size());
		for (int i = 0; i < (int)ticks.size(); ++i) {
			const RevTick& t = ticks[i];
			const int x0 = left + i * width / n, x1 = left + (i + 1) * width / n - (width / n >= 4 ? 1 : 0);
			if (x1 <= x0) continue;
			if (t.slow) RevRect(x0, y, x1, baseline, 255, 180, 65, 65);
			const int top = baseline - (int)((std::min)(t.share, 4.0f / 3.0f) * amplitude);
			if (t.slow) RevRect(x0, top, x1, baseline, 255, 180, 65, 255);
			else if (t.dir > 0) RevRect(x0, top, x1, baseline, 0, 210, 135, 255);
			else RevRect(x0, top, x1, baseline, 0, 155, 255, 255);
		}
	};
	bars(g_rev.shownBefore, x, w / 2);
	bars(g_rev.shownAfter, x + w / 2, w - w / 2);
	const int line = (std::max)(1, h / 40);
	RevRect(x, baseline - amplitude - line / 2, x + w, baseline - amplitude - line / 2 + line, 235, 90, 90, 200);
	RevRect(x + w / 2 - line / 2, y, x + w / 2 - line / 2 + line, y + h, 225, 225, 225, 255);
}

typedef void (*PanelFn)(void*);
PanelFn oPostChildPaint;
void** g_revSlot;

void Hooked_PostChildPaint(void* self)
{
	oPostChildPaint(self);
	ReversalPaint();
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

void JumpForward(int jump, int speed, int strafes, float maxHeight, float heightDelta, float gain, float mmod, float sync, float eff, float jss, float absJss)
{
	Jhud(jump, speed, gain, mmod, sync, jss, absJss);
	Ssj(jump, speed, strafes, maxHeight, heightDelta, gain, mmod, sync, eff, jss);
	if (Cvar(JSCV_OFFSET)) g_currentFrame = 0;
}

void ResetJumpState()
{
	Bgs& b = g_bgs;
	b.jump = b.strafeTick = b.syncedTick = 0;
	b.rawGain = b.mmodGain = b.idealGain = b.trajectory = 0.0f;
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
	// An older plugin's real gain also counted its sync. With either kind of plugin jss is rated by quality.
	const bool legacyRealGain = Cvar(JSCV_REALGAIN) && !Cvar(JSCV_GAINMODES);
	const bool jssQuality = Cvar(JSCV_REALGAIN | JSCV_GAINMODES);
	float jssThisTick = 0.0f, gaincoeff = 0.0f, mmodCoeff = 0.0f;

	if (b.ticksOnGround == 0) {
		if (origin[2] > b.jumpPeak) b.jumpPeak = origin[2];
		if (yawDiff != 0.0f) {
			const float perfJss = RadToDeg(atanf(30 / Speed2D(velocity)));
			const float finalJss = fabsf(yawDiff / perfJss);
			b.avgDiffFromPerf += finalJss;
			if (jssQuality) {
				const float quality = 1.0f - (1.0f - finalJss) * (1.0f - finalJss);
				b.avgAbsoluteJss += quality > 0.0f ? 100.0f * quality : 0.0f;
			} else {
				b.avgAbsoluteJss += 100 - fabsf((finalJss * 100.0f) - 100);
			}
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
		const float speed = Speed2D(velocity);
		const float idealGain = sqrtf(speed * speed + 900.0f) - speed;
		b.idealGain += idealGain;
		if (m.maxSpeed != 0.0f && wishspeed > m.maxSpeed) wishspeed = m.maxSpeed;
		if (wishspeed > 0.01f) {
			const float wishspd = wishspeed > 30.0f ? 30.0f : wishspeed;
			const float currentgain = velocity[0] * wishdir[0] + velocity[1] * wishdir[1] + velocity[2] * wishdir[2];
			if (currentgain < 30.0f) {
				gaincoeff = (wishspd - fabsf(currentgain)) / wishspd;
				if (!legacyRealGain) b.syncedTick++;
			}
			// mmod gain: the engine tops the wishdir component up to wishspd and leaves the rest alone
			if (currentgain < wishspd) {
				mmodCoeff = (sqrtf(speed * speed - currentgain * currentgain + wishspd * wishspd) - speed) / idealGain;
				if (legacyRealGain) b.syncedTick++;
			}
			if (touchesWall && gaincoeff > 0.5f) gaincoeff = fabsf(gaincoeff - 1.0f);
			if (touchesWall && mmodCoeff > 0.5f) mmodCoeff = fabsf(mmodCoeff - 1.0f);
			b.rawGain += gaincoeff;
			b.mmodGain += mmodCoeff * idealGain;
		}
		b.cmdNum++;
	}

	Trainer(b.ticksOnGround == 0, jssThisTick);
	PreTrainer(b.ticksOnGround == 0);
	ReversalTick(b.ticksOnGround == 0, yawDiff, Speed2D(velocity));
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
			float mmod = 100.0f * b.mmodGain / b.idealGain;
			// the older plugin's real gain was its only gain, efficiency included
			if (legacyRealGain) coeffsum = mmod;
			float distance = sqrtf(b.traveled[0] * b.traveled[0] + b.traveled[1] * b.traveled[1]);
			if (distance > b.trajectory) distance = b.trajectory;
			float efficiency = 0.0f;
			if (distance > 0.0f) efficiency = coeffsum * distance / b.trajectory;
			coeffsum = RoundToFloor(coeffsum * 100.0f + 0.5f) / 100.0f;
			mmod = Cvar(JSCV_GAINMODES) ? RoundToFloor(mmod * 1000.0f + 0.5f) / 1000.0f : RoundToFloor(mmod * 100.0f + 0.5f) / 100.0f;
			efficiency = RoundToFloor(efficiency * 100.0f + 0.5f) / 100.0f;
			JumpForward(b.jump, RoundToFloor(Speed2D(velocity)), b.strafeCount, b.jumpPeak - b.oldHeight, origin[2] - b.oldHeight,
				coeffsum, mmod, 100.0f * b.syncedTick / strafeTick, efficiency, b.avgDiffFromPerf / strafeTick, b.avgAbsoluteJss / strafeTick);
		}
		b.rawGain = b.mmodGain = b.idealGain = 0.0f;
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
	g_frameStrafe.live = g_framePre.live = false;
	g_rev = Reversal();
	g_hudMsg = g_textMsg = -1;
}

void SetChat(int client, int sayText2)
{
	g_client = client;
	g_sayText2 = sayText2;
}

// bhoptools_jsset <session> <on> <cvars> <channel groups 0-5> <settings 0-14 as %x> [setting 15, Bools2]
// [sv_accelerate, sv_friction, sv_stopspeed bits]
void Cmd_Settings(const CCommandX& c)
{
	if (!OurSession(c, 3)) return;
	g_on = atoi(c.argv[2]) != 0;
	if (!g_on || c.argc < 4 + 6 + Bools2) return;
	g_cvars = atoi(c.argv[3]);
	for (int i = 0; i < 6; ++i) g_channels[i] = atoi(c.argv[4 + i]);
	for (int i = 0; i < kSettings; ++i) g_set[i] = 10 + i < c.argc ? (int)(uint32_t)strtoul(c.argv[10 + i], nullptr, 16) : 0;
	g_haveMoveCvars = c.argc >= 10 + kSettings + 3;
	if (!g_haveMoveCvars) return;
	float* cvars[3] = { &g_accelerate, &g_friction, &g_stopspeed };
	for (int i = 0; i < 3; ++i) {
		const uint32_t bits = (uint32_t)strtoul(c.argv[10 + kSettings + i], nullptr, 16);
		memcpy(cvars[i], &bits, 4);
	}
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
	ReversalBreak();
}

bool InstallPaint()
{
	if (g_revSlot) return true;
	g_revSurface = CreateInterfaceIn(SURFACE_MODULE, "VGUI_Surface030");
	g_revEngine = CreateInterfaceIn(ENGINE_MODULE, "VEngineClient014");
	if (!g_revSurface || !g_revEngine) return false;
	void** focus = FindVTable(ENGINE_MODULE, RTTI_FocusOverlayPanel);
	void** panel = focus ? FindVTable(ENGINE_MODULE, RTTI_VguiPanel) : nullptr;
	// It overrides PostChildPaint and leaves Paint, PaintBackground and PaintBorder to vgui::Panel. Anything else
	// means the slots aren't where they should be, so the close-up stays text.
	bool ok = panel != nullptr;
	for (int i = VT_Panel_Paint; ok && i < VT_Panel_PostChildPaint; ++i) ok = focus[i] == panel[i];
	ok = ok && focus[VT_Panel_PostChildPaint] != panel[VT_Panel_PostChildPaint] && InModuleCode(ENGINE_MODULE, focus[VT_Panel_PostChildPaint]);
	if (!ok) {
		CoreLog("jumpstats: the engine's top panel isn't as expected, the reversal close-up is drawn as text");
		return true;
	}
	oPostChildPaint = reinterpret_cast<PanelFn>(focus[VT_Panel_PostChildPaint]);
	if (!PatchPointer(&focus[VT_Panel_PostChildPaint], reinterpret_cast<void*>(&Hooked_PostChildPaint))) {
		CoreLog("jumpstats: couldn't hook the engine's top panel, the reversal close-up is drawn as text");
		return true;
	}
	g_revSlot = &focus[VT_Panel_PostChildPaint];
	return true;
}

#ifdef _WIN32
void UninstallPaint()
{
	if (g_revSlot) PatchPointer(g_revSlot, reinterpret_cast<void*>(oPostChildPaint));
	g_revSlot = nullptr;
}
#endif

void OnFrame()
{
	const double now = MonotonicSeconds();
	const float dt = g_lastFrame < 0.0 ? 0.0f : (float)(now - g_lastFrame);
	g_lastFrame = now;
	if (!g_on || !settings::Get().jsframe) return;

	FrameTrack* tracks[2] = { &g_frameStrafe, &g_framePre };
	for (FrameTrack* t : tracks) {
		// no sample for a while: the trainer was switched off or the game stopped running commands
		if (t->live && now - t->at > 0.1) t->live = false;
		if (!t->live) continue;
		float a = (float)((now - t->at) / TickInterval());
		a = a < 0.0f ? 0.0f : a > 1.0f ? 1.0f : a;
		const float k = CompactStep(t->which, dt);
		for (int i = 0; i < 3; ++i) {
			const float x = t->prev[i] + (t->last[i] - t->prev[i]) * a;
			t->shown[i] = i < 2 ? t->shown[i] + (x - t->shown[i]) * k : x;
		}
	}

	if (g_frameStrafe.live) {
		const float jss = g_frameStrafe.shown[0];
		DrawCompact(HudTrainer, -1.0f, jss, kFrameHold, 0, Fmt("%.1f%%", jss), &g_frameStrafe.drawn);
	}
	if (g_framePre.live) {
		const FrameTrack& t = g_framePre;
		DrawCompact(HudPreStrafeTrainer, HudCoord(HudPreStrafeTrainer, 0), PrePercent(t.shown[0], t.shown[1], t.shown[2]), kFrameHold, t.group,
			Fmt("%i", RoundFloat(t.shown[1])), &g_framePre.drawn);
	}
}

}
