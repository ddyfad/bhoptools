// The center key overlays of shavit-hud (FillCenterKeys) and bgs-jumpstats (ShowKeys_Send). The examples
// were formatted from the plugins' own FormatEx calls.
#include "hudkeys.h"

#include <cstdio>

static int g_fail = 0, g_pass = 0;

#define CHECK(cond) do { if (cond) ++g_pass; else { ++g_fail; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

using namespace hudkeys;

struct Example { int layout; int buttons; float yawDiff; const char* text; };

// Layouts: 0 shavit on Windows, 1 shavit on Linux, 2 jumpstats, 3 jumpstats simple.
static const Example kExamples[] = {
	{ 0, 0x0, 0.0f, "\xE3\x80\x80" "  \xEF\xBD\xB0" "\xE3\x80\x80" "\xE3\x80\x80" "\xEF\xBD\xB0" "\n        \xEF\xBD\xB0" "   \n  \xEF\xBD\xB0" "\xE3\x80\x80" " \xEF\xBD\xB0" " \xE3\x80\x80" "\xEF\xBD\xB0" "\n\xE3\x80\x80" "   \xE3\x80\x80" "\xE3\x80\x80" " " },
	{ 0, 0xa, 1.5f, "\xE3\x80\x80" "  \xEF\xBC\xAA" "\xE3\x80\x80" "\xE3\x80\x80" "\xEF\xBD\xB0" "\n  <   \xEF\xBC\xB7" "   \n  \xEF\xBD\xB0" "\xE3\x80\x80" " \xEF\xBD\xB0" " \xE3\x80\x80" "\xEF\xBD\xB0" "\n\xE3\x80\x80" "   \xE3\x80\x80" "\xE3\x80\x80" " " },
	{ 0, 0x204, -0.2f, "\xE3\x80\x80" "  \xEF\xBD\xB0" "\xE3\x80\x80" "\xE3\x80\x80" "\xEF\xBC\xA3" "\n        \xEF\xBD\xB0" "   >\n  \xEF\xBC\xA1" "\xE3\x80\x80" " \xEF\xBD\xB0" " \xE3\x80\x80" "\xEF\xBD\xB0" "\n\xE3\x80\x80" "   \xE3\x80\x80" "\xE3\x80\x80" " " },
	{ 0, 0x590, 0.0f, "\xE3\x80\x80" "  \xEF\xBD\xB0" "\xE3\x80\x80" "\xE3\x80\x80" "\xEF\xBD\xB0" "\n        \xEF\xBD\xB0" "   \n  \xEF\xBD\xB0" "\xE3\x80\x80" " \xEF\xBC\xB3" " \xE3\x80\x80" "\xEF\xBC\xA4" "\n\xE3\x80\x80" "  \xEF\xBC\xAC" "\xE3\x80\x80" "\xE3\x80\x80" "\xEF\xBC\xB2" "" },
	{ 0, 0x7fe, 3.0f, "\xE3\x80\x80" "  \xEF\xBC\xAA" "\xE3\x80\x80" "\xE3\x80\x80" "\xEF\xBC\xA3" "\n  <   \xEF\xBC\xB7" "   \n  \xEF\xBC\xA1" "\xE3\x80\x80" " \xEF\xBC\xB3" " \xE3\x80\x80" "\xEF\xBC\xA4" "\n\xE3\x80\x80" "  \xEF\xBC\xAC" "\xE3\x80\x80" "\xE3\x80\x80" "\xEF\xBC\xB2" "" },
	{ 1, 0x0, 0.0f, "\xE3\x80\x80" "_\xE3\x80\x80" "\xE3\x80\x80" "_\n      _   \n_\xE3\x80\x80" " _ \xE3\x80\x80" "_\n\xE3\x80\x80" " \xE3\x80\x80" "\xE3\x80\x80" " " },
	{ 1, 0xa, 1.5f, "\xE3\x80\x80" "J\xE3\x80\x80" "\xE3\x80\x80" "_\n<   W   \n_\xE3\x80\x80" " _ \xE3\x80\x80" "_\n\xE3\x80\x80" " \xE3\x80\x80" "\xE3\x80\x80" " " },
	{ 1, 0x204, -0.2f, "\xE3\x80\x80" "_\xE3\x80\x80" "\xE3\x80\x80" "C\n      _   >\nA\xE3\x80\x80" " _ \xE3\x80\x80" "_\n\xE3\x80\x80" " \xE3\x80\x80" "\xE3\x80\x80" " " },
	{ 1, 0x590, 0.0f, "\xE3\x80\x80" "_\xE3\x80\x80" "\xE3\x80\x80" "_\n      _   \n_\xE3\x80\x80" " S \xE3\x80\x80" "D\n\xE3\x80\x80" "L\xE3\x80\x80" "\xE3\x80\x80" "R" },
	{ 1, 0x7fe, 3.0f, "\xE3\x80\x80" "J\xE3\x80\x80" "\xE3\x80\x80" "C\n<   W   \nA\xE3\x80\x80" " S \xE3\x80\x80" "D\n\xE3\x80\x80" "L\xE3\x80\x80" "\xE3\x80\x80" "R" },
	{ 2, 0x0, 0.0f, "  \xEF\xBD\xB0" "   \xEF\xBD\xB0" "\n \xEF\xBD\xB0" "  \xEF\xBD\xB0" "  \xEF\xBD\xB0" "\n\xEF\xBD\xB0" "\xE3\x80\x80" " \xEF\xBD\xB0" " \xE3\x80\x80" "\xEF\xBD\xB0" "\n  \xE3\x80\x80" "\xE3\x80\x80" " " },
	{ 2, 0xa, 1.5f, "  \xEF\xBC\xAA" "   \xEF\xBD\xB0" "\n <  \xEF\xBC\xB7" "  \xEF\xBD\xB0" "\n\xEF\xBD\xB0" "\xE3\x80\x80" " \xEF\xBD\xB0" " \xE3\x80\x80" "\xEF\xBD\xB0" "\n  \xE3\x80\x80" "\xE3\x80\x80" " " },
	{ 2, 0x204, -0.2f, "  \xEF\xBD\xB0" "   \xEF\xBC\xA3" "\n \xEF\xBD\xB0" "  \xEF\xBD\xB0" "  >\n\xEF\xBC\xA1" "\xE3\x80\x80" " \xEF\xBD\xB0" " \xE3\x80\x80" "\xEF\xBD\xB0" "\n  \xE3\x80\x80" "\xE3\x80\x80" " " },
	{ 2, 0x590, 0.0f, "  \xEF\xBD\xB0" "   \xEF\xBD\xB0" "\n \xEF\xBD\xB0" "  \xEF\xBD\xB0" "  \xEF\xBD\xB0" "\n\xEF\xBD\xB0" "\xE3\x80\x80" " \xEF\xBC\xB3" " \xE3\x80\x80" "\xEF\xBC\xA4" "\n \xEF\xBC\xAC" "\xE3\x80\x80" "\xE3\x80\x80" "\xEF\xBC\xB2" "" },
	{ 2, 0x7fe, 3.0f, "  \xEF\xBC\xAA" "   \xEF\xBC\xA3" "\n <  \xEF\xBC\xB7" "  \xEF\xBD\xB0" "\n\xEF\xBC\xA1" "\xE3\x80\x80" " \xEF\xBC\xB3" " \xE3\x80\x80" "\xEF\xBC\xA4" "\n \xEF\xBC\xAC" "\xE3\x80\x80" "\xE3\x80\x80" "\xEF\xBC\xB2" "" },
	{ 3, 0x0, 0.0f, "\xE3\x80\x80" "   \xE3\x80\x80" "\xE3\x80\x80" " \n            \n     \xE3\x80\x80" "   \xE3\x80\x80" "  \n\xE3\x80\x80" "   \xE3\x80\x80" "\xE3\x80\x80" " " },
	{ 3, 0xa, 1.5f, "\xE3\x80\x80" "  \xEF\xBC\xAA" "\xE3\x80\x80" "\xE3\x80\x80" " \n      \xEF\xBC\xB7" "    \n  \xE2\x86\x90" " \xE3\x80\x80" "   \xE3\x80\x80" "  \n\xE3\x80\x80" "   \xE3\x80\x80" "\xE3\x80\x80" " " },
	{ 3, 0x204, -0.2f, "\xE3\x80\x80" "   \xE3\x80\x80" "\xE3\x80\x80" "\xEF\xBC\xA3" "\n            \n    \xEF\xBC\xA1" "\xE3\x80\x80" "   \xE3\x80\x80" " \xE2\x86\x92" "\n\xE3\x80\x80" "   \xE3\x80\x80" "\xE3\x80\x80" " " },
	{ 3, 0x590, 0.0f, "\xE3\x80\x80" "   \xE3\x80\x80" "\xE3\x80\x80" " \n            \n     \xE3\x80\x80" " \xEF\xBC\xB3" " \xE3\x80\x80" "\xEF\xBC\xA4" " \n\xE3\x80\x80" "  \xEF\xBC\xAC" "\xE3\x80\x80" "\xE3\x80\x80" "\xEF\xBC\xB2" "" },
	{ 3, 0x7fe, 3.0f, "\xE3\x80\x80" "  \xEF\xBC\xAA" "\xE3\x80\x80" "\xE3\x80\x80" "\xEF\xBC\xA3" "\n      \xEF\xBC\xB7" "    \n  \xE2\x86\x90" "\xEF\xBC\xA1" "\xE3\x80\x80" " \xEF\xBC\xB3" " \xE3\x80\x80" "\xEF\xBC\xA4" " \n\xE3\x80\x80" "  \xEF\xBC\xAC" "\xE3\x80\x80" "\xE3\x80\x80" "\xEF\xBC\xB2" "" },
};

int main()
{
	for (const Example& e : kExamples) {
		CHECK(Parse(e.text) == e.layout);
		CHECK(Format(e.layout, e.buttons, e.yawDiff) == e.text);
	}

	for (const Example& from : kExamples)
		for (const Example& to : kExamples)
			if (from.layout == to.layout) CHECK(Format(Parse(from.text), to.buttons, to.yawDiff) == to.text);

	const int keys[] = { IN_JUMP, IN_DUCK, IN_FORWARD, IN_BACK, IN_LEFT, IN_RIGHT, IN_MOVELEFT, IN_MOVERIGHT };
	int unique = 0, total = 0;
	for (int l = 0; l < kLayoutCount; ++l)
		for (int b = 0; b < 256; ++b)
			for (float turn : { 0.0f, 1.0f, -1.0f }) {
				int buttons = 0;
				for (int k = 0; k < 8; ++k) if (b & 1 << k) buttons |= keys[k];
				++total;
				if (Parse(Format(l, buttons, turn).c_str()) == l) ++unique;
			}
	CHECK(total == 4 * 256 * 3 && unique == total);

	CHECK(Parse("") == -1);
	CHECK(Parse("\n \n \n+12 280") == -1);  // jhud
	CHECK(Parse("You are now in the start zone.") == -1);
	std::string edited = kExamples[1].text;
	edited += " ";
	CHECK(Parse(edited.c_str()) == -1);
	edited = kExamples[1].text;
	edited.erase(edited.size() - 1);
	CHECK(Parse(edited.c_str()) == -1);
	edited = kExamples[6].text;
	edited[3] = 'X';  // the J slot on Linux
	CHECK(Parse(edited.c_str()) == -1);
	// shavit's CS:GO layout.
	CHECK(Parse("\xEF\xBD\xB0" "   " "\xEF\xBD\xB0" "\n" "\xEF\xBD\xB0" "  " "\xEF\xBD\xB0" "  " "\xEF\xBD\xB0"
		"\n" "\xEF\xBD\xB0" "\xE3\x80\x80" " " "\xEF\xBD\xB0" " " "\xE3\x80\x80" "\xEF\xBD\xB0" "\n " "\xE3\x80\x80" "\xE3\x80\x80" " ") == -1);

	// Mouse direction: the yaw difference, wrapped like shavit's GetAngleDiff.
	CHECK(AngleDiff(20.0f, 10.0f) == 10.0f);
	CHECK(AngleDiff(10.0f, 20.0f) == -10.0f);
	CHECK(AngleDiff(-179.0f, 179.0f) == 2.0f);
	CHECK(AngleDiff(179.0f, -179.0f) == -2.0f);
	CHECK(AngleDiff(90.0f, 90.0f) == 0.0f);

	// TextMsg payloads: HUD_PRINTCENTER, the text, four empty parameters.
	std::string text;
	const std::string center = CenterTextMsg(kExamples[0].text);
	const unsigned char* data = reinterpret_cast<const unsigned char*>(center.data());
	CHECK(ReadCenterText(data, (int)center.size(), text) && text == kExamples[0].text);
	CHECK(center.size() == strlen(kExamples[0].text) + 6);
	CHECK(!ReadCenterText(data, (int)center.size() - 1, text));
	const std::string chat = std::string("\x03hi\0\0\0\0\0", 8);
	CHECK(!ReadCenterText(reinterpret_cast<const unsigned char*>(chat.data()), (int)chat.size(), text));
	const std::string param = std::string("\x04#Hint\0x\0\0\0\0", 11);
	CHECK(!ReadCenterText(reinterpret_cast<const unsigned char*>(param.data()), (int)param.size(), text));
	CHECK(!ReadCenterText(data, 0, text));

	printf("hud keys: %d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
