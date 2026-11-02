// shavit-hud's center key overlay (FillCenterKeys) and bgs-jumpstats' center showkeys (ShowKeys_Send),
// as CS:S gets them, so the keys can be redrawn from the newest local command.
#pragma once

#include <cmath>
#include <cstring>
#include <string>

namespace hudkeys {

const int IN_JUMP = 1 << 1, IN_DUCK = 1 << 2, IN_FORWARD = 1 << 3, IN_BACK = 1 << 4,
	IN_LEFT = 1 << 7, IN_RIGHT = 1 << 8, IN_MOVELEFT = 1 << 9, IN_MOVERIGHT = 1 << 10;
// The mouse direction: the yaw difference from the previous command.
const int kTurnLeft = -1, kTurnRight = -2;

struct Slot { int key; const char* on; const char* off; };
struct Layout { const char* format; Slot slots[10]; };

#define HK_SP "\xE3\x80\x80"   // U+3000 ideographic space
#define HK_BAR "\xEF\xBD\xB0"  // U+FF70
#define HK_FW(c) "\xEF\xBC" c  // fullwidth U+FF21..
const Layout kLayouts[] = {
	// shavit-hud, Windows clients
	{ HK_SP "  %s" HK_SP HK_SP "%s\n  %s   %s   %s\n  %s" HK_SP " %s " HK_SP "%s\n" HK_SP "  %s" HK_SP HK_SP "%s", {
		{ IN_JUMP, HK_FW("\xAA"), HK_BAR }, { IN_DUCK, HK_FW("\xA3"), HK_BAR },
		{ kTurnLeft, "<", "  " }, { IN_FORWARD, HK_FW("\xB7"), " " HK_BAR }, { kTurnRight, ">", "" },
		{ IN_MOVELEFT, HK_FW("\xA1"), HK_BAR }, { IN_BACK, HK_FW("\xB3"), HK_BAR }, { IN_MOVERIGHT, HK_FW("\xA4"), HK_BAR },
		{ IN_LEFT, HK_FW("\xAC"), " " }, { IN_RIGHT, HK_FW("\xB2"), " " } } },
	// shavit-hud, clients without windows_speaker_config (Linux, whose HUD font lacks the fullwidth letters)
	{ HK_SP "%s" HK_SP HK_SP "%s\n%s   %s   %s\n%s" HK_SP " %s " HK_SP "%s\n" HK_SP "%s" HK_SP HK_SP "%s", {
		{ IN_JUMP, "J", "_" }, { IN_DUCK, "C", "_" },
		{ kTurnLeft, "<", "  " }, { IN_FORWARD, "W", " _" }, { kTurnRight, ">", "" },
		{ IN_MOVELEFT, "A", "_" }, { IN_BACK, "S", "_" }, { IN_MOVERIGHT, "D", "_" },
		{ IN_LEFT, "L", " " }, { IN_RIGHT, "R", " " } } },
	// bgs-jumpstats, unreliable showkeys
	{ "  %s   %s\n %s  %s  %s\n%s" HK_SP " %s " HK_SP "%s\n %s" HK_SP HK_SP "%s", {
		{ IN_JUMP, HK_FW("\xAA"), HK_BAR }, { IN_DUCK, HK_FW("\xA3"), HK_BAR },
		{ kTurnLeft, "<", HK_BAR }, { IN_FORWARD, HK_FW("\xB7"), HK_BAR }, { kTurnRight, ">", HK_BAR },
		{ IN_MOVELEFT, HK_FW("\xA1"), HK_BAR }, { IN_BACK, HK_FW("\xB3"), HK_BAR }, { IN_MOVERIGHT, HK_FW("\xA4"), HK_BAR },
		{ IN_LEFT, HK_FW("\xAC"), " " }, { IN_RIGHT, HK_FW("\xB2"), " " } } },
	// bgs-jumpstats, unreliable simple showkeys
	{ HK_SP "  %s" HK_SP HK_SP "%s\n      %s    \n  %s%s" HK_SP " %s " HK_SP "%s%s\n" HK_SP "  %s" HK_SP HK_SP "%s", {
		{ IN_JUMP, HK_FW("\xAA"), " " }, { IN_DUCK, HK_FW("\xA3"), " " }, { IN_FORWARD, HK_FW("\xB7"), "  " },
		{ kTurnLeft, "\xE2\x86\x90", "  " }, { IN_MOVELEFT, HK_FW("\xA1"), " " }, { IN_BACK, HK_FW("\xB3"), " " },
		{ IN_MOVERIGHT, HK_FW("\xA4"), " " }, { kTurnRight, "\xE2\x86\x92", " " },
		{ IN_LEFT, HK_FW("\xAC"), " " }, { IN_RIGHT, HK_FW("\xB2"), " " } } },
};
#undef HK_SP
#undef HK_BAR
#undef HK_FW
const int kLayoutCount = sizeof(kLayouts) / sizeof(kLayouts[0]);

// shavit's GetAngleDiff.
inline float AngleDiff(float current, float previous)
{
	const float diff = current - previous;
	return diff - 360.0f * floorf((diff + 180.0f) / 360.0f);
}

inline bool Pressed(int key, int buttons, float yawDiff)
{
	if (key == kTurnLeft) return yawDiff > 0.0f;
	if (key == kTurnRight) return yawDiff < 0.0f;
	return (buttons & key) != 0;
}

// Readings of `text` from this point of the format, stopping at two.
inline int Readings(const Layout& l, const char* f, int slot, const char* t)
{
	for (;; ++f, ++t) {
		if (f[0] == '%' && f[1] == 's') {
			const Slot& s = l.slots[slot];
			int n = 0;
			for (const char* alt : { s.on, s.off }) {
				const size_t len = strlen(alt);
				if (!strncmp(t, alt, len)) n += Readings(l, f + 2, slot + 1, t + len);
				if (n > 1) return n;
			}
			return n;
		}
		if (*f != *t) return 0;
		if (!*f) return 1;
	}
}

// The layout `text` was formatted with, or -1 unless exactly one reading of it fits.
inline int Parse(const char* text)
{
	int found = -1, n = 0;
	for (int i = 0; i < kLayoutCount && n < 2; ++i) {
		const int r = Readings(kLayouts[i], kLayouts[i].format, 0, text);
		if (r) found = i;
		n += r;
	}
	return n == 1 ? found : -1;
}

// TextMsg to HUD_PRINTCENTER as SourceMod's PrintCenterText and both plugins write it: the text, then
// four empty parameters.
inline bool ReadCenterText(const unsigned char* data, int bytes, std::string& text)
{
	if (bytes < 1 || data[0] != 4) return false;
	int end = 1;
	while (end < bytes && data[end]) ++end;
	if (end + 4 >= bytes) return false;
	for (int i = 1; i <= 4; ++i) if (data[end + i]) return false;
	text.assign(reinterpret_cast<const char*>(data) + 1, end - 1);
	return true;
}

inline std::string CenterTextMsg(const std::string& text)
{
	return std::string("\x04", 1) + text + std::string(5, '\0');
}

inline std::string Format(int layout, int buttons, float yawDiff)
{
	const Layout& l = kLayouts[layout];
	std::string out;
	int slot = 0;
	for (const char* f = l.format; *f; ++f) {
		if (f[0] == '%' && f[1] == 's') {
			const Slot& s = l.slots[slot++];
			out += Pressed(s.key, buttons, yawDiff) ? s.on : s.off;
			++f;
		} else {
			out += *f;
		}
	}
	return out;
}

}
