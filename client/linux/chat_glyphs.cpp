// The Linux surface has no font fallback of its own, so characters the chat font lacks come out
// as its empty box. Ported from zastools' chatbackdrop.

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>
#include <dlfcn.h>

#include "utils.h"
#include "sigs.h"
#include "game_layout.h"
#include "chat_glyphs.h"
#include "settings.h"

namespace glyphs {
namespace {

typedef unsigned int HFont;
typedef void  (*PaintFn)(void*);
typedef void  (*SetTextFontFn)(void*, HFont);
typedef void  (*PrintTextFn)(void*, const wchar_t*, int, int);
typedef HFont (*CreateFontFn)(void*);
typedef bool  (*SetFontGlyphSetFn)(void*, HFont, const char*, int, int, int, int, int, int, int);
typedef int   (*GetFontTallFn)(void*, HFont);
typedef char* (*FontManagerFn)();
typedef unsigned (*CharIndexFn)(void*, unsigned long);
typedef void  (*GetTextPosFn)(void*, int*, int*);
typedef void  (*SetTextPosFn)(void*, int, int);
typedef void  (*GetCharABCFn)(void*, HFont, int, int*, int*, int*);

struct FcFontSet { int nfont, sfont; void** fonts; };
typedef void*      (*FcCreateFn)();
typedef int        (*FcCharSetAddCharFn)(void*, unsigned);
typedef int        (*FcPatternAddCharSetFn)(void*, const char*, const void*);
typedef int        (*FcConfigSubstituteFn)(void*, void*, int);
typedef void       (*FcDestroyFn)(void*);
typedef FcFontSet* (*FcFontSortFn)(void*, void*, int, void**, int*);
typedef int        (*FcPatternGetCharSetFn)(void*, const char*, int, void**);
typedef int        (*FcCharSetHasCharFn)(const void*, unsigned);
typedef int        (*FcPatternGetStringFn)(void*, const char*, int, const char**);
typedef void       (*FcFontSetDestroyFn)(FcFontSet*);

const int FONTFLAG_ANTIALIAS = 0x010;

// Tried in order, then whatever fontconfig finds. Steam's runtime only sees ~/.local/share/fonts
// and its own DejaVu. Noto Serif Tibetan and Trikz Chat have the brackets of ༼ ºل͟º ༽.
const char* const kFallbackFaces[] = { "DejaVu Sans", "DejaVu Sans Mono", "Symbola", "Noto Sans CJK JP",
	"Noto Sans Symbols2", "Noto Serif Tibetan", "JetBrainsMono Nerd Font", "CaskaydiaCove Nerd Font", "mononoki Nerd Font",
	"Trikz Chat", "Trikz Chat Deja", "Trikz Chat Publicsans" };

struct Fallback {
	std::string face;
	HFont handle;
	int   tall;
	bool  loaded;
};

void**        g_surfaceVt = nullptr;
void*         g_surface = nullptr;
char*         g_fontManager = nullptr;
CharIndexFn   g_charIndex = nullptr;
PaintFn       g_originalPaint = nullptr;
SetTextFontFn g_originalSetTextFont = nullptr;
PrintTextFn   g_originalPrintText = nullptr;

bool  g_inPaint = false;
bool  g_stoodDown = false;
HFont g_font = 0;
wchar_t g_lastChar = 0;
HFont   g_lastFont = 0;
std::vector<Fallback> g_fallbacks;
// -1 is the chat font itself, else the fallback's index. Keyed by font << 32 | character.
std::unordered_map<uint64_t, int>* g_drawnWith = nullptr;

ConCommandX g_cmd;

struct {
	FcCreateFn            PatternCreate, CharSetCreate;
	FcCharSetAddCharFn    CharSetAddChar;
	FcPatternAddCharSetFn PatternAddCharSet;
	FcConfigSubstituteFn  ConfigSubstitute;
	FcDestroyFn           DefaultSubstitute, PatternDestroy, CharSetDestroy;
	FcFontSortFn          FontSort;
	FcPatternGetCharSetFn PatternGetCharSet;
	FcCharSetHasCharFn    CharSetHasChar;
	FcPatternGetStringFn  PatternGetString;
	FcFontSetDestroyFn    FontSetDestroy;
} g_fc;
bool g_haveFc = false;

char* Amalgam(HFont font)
{
	if (font == 0 || font >= *(unsigned*)(g_fontManager + OFF_FontManager_Count)) return nullptr;
	return *(char**)g_fontManager + (size_t)font * FontAmalgam_SIZE;
}

int FaceCount(HFont font)
{
	char* a = Amalgam(font);
	return a ? *(int*)(a + OFF_FontAmalgam_Count) : 0;
}

// FreeType's glyph index for ch in the face `font` draws it with: 0 missing, -1 can't tell.
long GlyphIndex(HFont font, unsigned ch)
{
	const int n = FaceCount(font);
	if (n <= 0) return -1;
	char* faces = *(char**)Amalgam(font);
	// Same pick as CFontAmalgam::GetFontForChar: the first range that takes ch in, else the first face.
	void* face = *(void**)(faces + 8);
	for (int i = 0; i < n; ++i) {
		char* e = faces + i * FontRange_SIZE;
		if ((int)ch >= *(int*)e && (int)ch <= *(int*)(e + 4)) {
			face = *(void**)(e + 8);
			break;
		}
	}
	void* ft = face ? *(void**)((char*)face + OFF_LinuxFont_FTFace) : nullptr;
	return ft ? (long)g_charIndex(ft, ch) : -1;
}

// A face that isn't installed gets fontconfig's closest match, so this rarely fails outright.
bool FallbackReady(int i, int tall)
{
	Fallback& f = g_fallbacks[i];
	// Loaded but empty: the surface cleared every font (a video mode change), so load it again.
	if (f.tall == tall && (!f.loaded || FaceCount(f.handle) > 0)) return f.loaded;
	f.tall = tall;
	if (!f.handle) f.handle = VFunc<CreateFontFn>(g_surface, VT_Surface_CreateFont)(g_surface);
	f.loaded = f.handle && VFunc<SetFontGlyphSetFn>(g_surface, VT_Surface_SetFontGlyphSet)(
		g_surface, f.handle, f.face.c_str(), tall, 400, 0, 0, FONTFLAG_ANTIALIAS, 0, 0);
	return f.loaded;
}

bool IsCombining(unsigned ch)
{
	return (ch >= 0x0300 && ch <= 0x036F) || (ch >= 0x1AB0 && ch <= 0x1AFF) || (ch >= 0x1DC0 && ch <= 0x1DFF)
		|| (ch >= 0x20D0 && ch <= 0x20FF) || (ch >= 0xFE20 && ch <= 0xFE2F);
}

std::string FontconfigFace(unsigned ch)
{
	std::string face;
	void* pattern = g_fc.PatternCreate();
	void* chars = g_fc.CharSetCreate();
	g_fc.CharSetAddChar(chars, ch);
	g_fc.PatternAddCharSet(pattern, "charset", chars);
	g_fc.ConfigSubstitute(nullptr, pattern, 0);
	g_fc.DefaultSubstitute(pattern);
	int result;
	if (FcFontSet* set = g_fc.FontSort(nullptr, pattern, 0, nullptr, &result)) {
		for (int i = 0; i < set->nfont && face.empty(); ++i) {
			void* has;
			const char* family;
			if (g_fc.PatternGetCharSet(set->fonts[i], "charset", 0, &has) == 0 && g_fc.CharSetHasChar(has, ch)
					&& g_fc.PatternGetString(set->fonts[i], "family", 0, &family) == 0)
				face = family;
		}
		g_fc.FontSetDestroy(set);
	}
	g_fc.PatternDestroy(pattern);
	g_fc.CharSetDestroy(chars);
	return face;
}

int DrawnWith(wchar_t ch)
{
	if ((unsigned)ch < 0x80) return -1;
	const uint64_t key = (uint64_t)g_font << 32 | (uint32_t)ch;
	auto found = g_drawnWith->find(key);
	if (found != g_drawnWith->end()) return found->second;
	int use = -1;
	if (GlyphIndex(g_font, ch) == 0) {
		const int tall = VFunc<GetFontTallFn>(g_surface, VT_Surface_GetFontTall)(g_surface, g_font);
		for (int i = 0; i < (int)g_fallbacks.size() && use < 0; ++i)
			if (FallbackReady(i, tall) && GlyphIndex(g_fallbacks[i].handle, ch) > 0) use = i;
		const std::string face = use < 0 && g_haveFc ? FontconfigFace(ch) : std::string();
		bool known = face.empty();
		for (const Fallback& f : g_fallbacks) known = known || f.face == face;
		if (!known) {
			g_fallbacks.push_back({ face, 0, 0, false });
			const int i = (int)g_fallbacks.size() - 1;
			if (FallbackReady(i, tall) && GlyphIndex(g_fallbacks[i].handle, ch) > 0) use = i;
		}
	}
	(*g_drawnWith)[key] = use;
	return use;
}

void Hooked_SetTextFont(void* self, HFont font)
{
	if (g_inPaint) g_font = font;
	g_originalSetTextFont(self, font);
}

// A mark from another font can't be drawn in the same run as the character it goes on, and on its
// own the surface would move the pen on by its width, leaving it between two characters. So it's
// centred over the character before it and the pen put back.
void DrawMark(void* self, wchar_t ch, HFont font, int type)
{
	int x, y, a, b, c, baseA = 0, baseB = 0, baseC = 0;
	VFunc<GetTextPosFn>(g_surface, VT_Surface_DrawGetTextPos)(g_surface, &x, &y);
	VFunc<GetCharABCFn>(g_surface, VT_Surface_GetCharABCwide)(g_surface, font, ch, &a, &b, &c);
	if (g_lastChar) VFunc<GetCharABCFn>(g_surface, VT_Surface_GetCharABCwide)(g_surface, g_lastFont, g_lastChar, &baseA, &baseB, &baseC);
	// The surface draws a glyph `a` past the pen.
	const int baseWidth = baseA + baseB + baseC;
	VFunc<SetTextPosFn>(g_surface, VT_Surface_DrawSetTextPos)(g_surface, x - baseWidth + (baseWidth - b) / 2 - a, y);
	g_originalSetTextFont(self, font);
	g_originalPrintText(self, &ch, 1, type);
	g_originalSetTextFont(self, g_font);
	VFunc<SetTextPosFn>(g_surface, VT_Surface_DrawSetTextPos)(g_surface, x, y);
}

void Hooked_PrintText(void* self, const wchar_t* text, int length, int type)
{
	if (!g_inPaint || !text || length <= 0) return g_originalPrintText(self, text, length, type);
	for (int at = 0; at < length;) {
		int use = DrawnWith(text[at]);
		if (use >= 0 && FaceCount(g_fallbacks[use].handle) <= 0) {
			g_drawnWith->clear();
			use = -1;
		}
		if (use >= 0 && IsCombining((unsigned)text[at])) {
			DrawMark(self, text[at], g_fallbacks[use].handle, type);
			++at;
			continue;
		}
		int end = at + 1;
		while (end < length && DrawnWith(text[end]) == use && !(use >= 0 && IsCombining((unsigned)text[end]))) ++end;
		const HFont font = use >= 0 ? g_fallbacks[use].handle : g_font;
		if (use >= 0) g_originalSetTextFont(self, font);
		g_originalPrintText(self, text + at, end - at, type);
		if (use >= 0) g_originalSetTextFont(self, g_font);
		g_lastChar = text[end - 1];
		g_lastFont = font;
		at = end;
	}
}

void Hooked_Paint(void* self)
{
	// Anything hooked on the surface after us (zastools injected later) calls ours as its original.
	if (g_surfaceVt[VT_Surface_DrawSetTextFont] != (void*)&Hooked_SetTextFont
			|| g_surfaceVt[VT_Surface_DrawPrintText] != (void*)&Hooked_PrintText) {
		if (!g_stoodDown) CoreLog("chat glyphs: something else hooked the surface's text calls, standing down");
		g_stoodDown = true;
		return g_originalPaint(self);
	}
	g_inPaint = settings::Get().chatglyphs;
	g_lastChar = 0;
	g_originalPaint(self);
	g_inPaint = false;
}

void Cmd_ChatGlyphs(const CCommandX& c)
{
	if (c.argc > 1) {
		settings::Get().chatglyphs = atoi(c.argv[1]) != 0;
		settings::Changed();
	}
	CoreLog("chat_glyphs %d", settings::Get().chatglyphs ? 1 : 0);
}

void RegisterCommand()
{
	void* cvar = CreateInterfaceIn(VSTDLIB_MODULE, "VEngineCvar004");
	void* echo = cvar ? VFunc<void* (*)(void*, const char*)>(cvar, VT_Cvar_FindBase)(cvar, "echo") : nullptr;
	const char* echoName = echo ? static_cast<ConCommandX*>(echo)->name : nullptr;
	if (!echo || !VFunc<bool (*)(void*)>(echo, VT_CCB_IsCommand)(echo) || !echoName || strcmp(echoName, "echo")) {
		CoreLog("chat glyphs: couldn't register chat_glyphs, staying on");
		return;
	}
	g_cmd.vtable = *reinterpret_cast<void**>(echo);
	g_cmd.name = "chat_glyphs";
	g_cmd.help = "Draw chat characters the chat font lacks from a fallback font (0/1).";
	g_cmd.callback = reinterpret_cast<void*>(&Cmd_ChatGlyphs);
	g_cmd.bits = 0x02;  // new-style callback taking const CCommand&
	VFunc<void (*)(void*, void*)>(cvar, VT_Cvar_Register)(cvar, &g_cmd);
}

bool InSegments(const std::vector<ModuleSegment>& segs, const void* p, size_t len)
{
	for (const auto& s : segs)
		if ((uintptr_t)p >= s.start && (uintptr_t)p + len <= s.end) return true;
	return false;
}

bool CodeMatches(const std::vector<ModuleSegment>& segs, const uint8_t* p, const char* sig)
{
	const ParsedSig s = ParseSig(sig);
	if (!p || !InSegments(segs, p, s.bytes.size())) return false;
	for (size_t i = 0; i < s.bytes.size(); ++i)
		if (s.mask[i] && p[i] != s.bytes[i]) return false;
	return true;
}

const uint8_t* Rel32(const uint8_t* p, size_t at)
{
	int32_t rel;
	memcpy(&rel, p + at, sizeof rel);
	return p + at + 4 + rel;
}

// Follows GetFontName and GetFontFamilyName down to the layouts GlyphIndex reads, so a game
// update that moves any of them turns the feature off instead of reading the wrong memory.
char* FindFontManager(const std::vector<ModuleSegment>& text)
{
	const uint8_t* name   = (const uint8_t*)g_surfaceVt[VT_Surface_GetFontName];
	const uint8_t* family = (const uint8_t*)g_surfaceVt[VT_Surface_GetFontFamilyName];
	if (!CodeMatches(text, name, SIG_Surface_FontManagerCall) || !CodeMatches(text, family, SIG_Surface_FontManagerCall))
		return nullptr;
	const uint8_t* getter = Rel32(name, 14);
	if (!CodeMatches(text, getter, SIG_FontManager_Getter)) return nullptr;

	const uint8_t* nameBody   = Rel32(name, 30);
	const uint8_t* familyBody = Rel32(family, 30);
	if (!CodeMatches(text, nameBody, SIG_FontManager_Amalgam) || !CodeMatches(text, familyBody, SIG_FontManager_Amalgam))
		return nullptr;
	const uint8_t* firstFace  = Rel32(nameBody, 16);
	const uint8_t* familyFace = Rel32(familyBody, 16);
	if (!CodeMatches(text, firstFace, SIG_FontAmalgam_FirstFace) || !CodeMatches(text, familyFace, SIG_FontAmalgam_FirstFace)
			|| !CodeMatches(text, familyFace + OFF_FontAmalgam_FamilyLea, SIG_FontAmalgam_FamilyLea))
		return nullptr;
	if (!CodeMatches(text, Rel32(familyFace, OFF_FontAmalgam_FamilyLea + 3), SIG_LinuxFont_GetFamilyName))
		return nullptr;
	return ((FontManagerFn)getter)();
}

// Itanium RTTI: typeinfo name string, the typeinfo whose second word points at it, and the
// primary vtable whose slot -1 is that typeinfo with a zero offset-to-top before it.
void** FindVTable(const char* module, const char* mangled)
{
	const std::vector<MemRange> data = ModuleDataRanges(module);
	const size_t len = strlen(mangled) + 1;
	uintptr_t name = 0;
	for (const auto& r : data) {
		for (const char* p = (const char*)r.start + 1; !name && p + len <= (const char*)r.end; ++p) {
			p = (const char*)memmem(p, (const char*)r.end - p, mangled, len);
			if (!p) break;
			if (p[-1] == 0) name = (uintptr_t)p;
		}
	}
	if (!name) return nullptr;

	auto findPointer = [&](uintptr_t value, bool zeroBefore) -> uintptr_t {
		for (const auto& r : data)
			for (uintptr_t a = (r.start + 15) & ~(uintptr_t)7; a + 8 <= r.end; a += 8)
				if (*(uintptr_t*)a == value && (!zeroBefore || *(uintptr_t*)(a - 8) == 0)) return a;
		return 0;
	};
	const uintptr_t typeinfoName = findPointer(name, false);
	const uintptr_t slot = typeinfoName ? findPointer(typeinfoName - 8, true) : 0;
	return slot ? (void**)(slot + 8) : nullptr;
}

}

void Install()
{
	if (!FindExecSegments("libzastools.so").empty()) {
		CoreLog("chat glyphs: zastools is loaded and does this itself, not installing");
		return;
	}
	g_surface = CreateInterfaceIn(SURFACE_MODULE, "VGUI_Surface030");
	const std::vector<ModuleSegment> text = FindExecSegments(SURFACE_MODULE);
	if (!g_surface || text.empty()) {
		CoreLog("chat glyphs: surface not found, not installing");
		return;
	}
	g_surfaceVt = *(void***)g_surface;
	if (!InSegments(text, g_surfaceVt[VT_Surface_DrawSetTextFont], 1) || !InSegments(text, g_surfaceVt[VT_Surface_DrawPrintText], 1)) {
		CoreLog("chat glyphs: the surface's text calls are already hooked by something else, not installing");
		return;
	}
	g_fontManager = FindFontManager(text);
	if (!g_fontManager) {
		CoreLog("chat glyphs: the surface's font manager isn't the build this was written for, not installing");
		return;
	}
	// The surface's own FreeType, looked up from the surface so it's the copy that made its faces.
	if (void* h = dlopen(text[0].path.c_str(), RTLD_LAZY | RTLD_NOLOAD)) {
		g_charIndex = (CharIndexFn)dlsym(h, "FT_Get_Char_Index");
		// And the fontconfig it finds fonts with, so this sees the same ones.
		void** fc = (void**)&g_fc;
		const char* const names[] = { "FcPatternCreate", "FcCharSetCreate", "FcCharSetAddChar", "FcPatternAddCharSet",
			"FcConfigSubstitute", "FcDefaultSubstitute", "FcPatternDestroy", "FcCharSetDestroy", "FcFontSort",
			"FcPatternGetCharSet", "FcCharSetHasChar", "FcPatternGetString", "FcFontSetDestroy" };
		g_haveFc = true;
		for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
			g_haveFc = (fc[i] = dlsym(h, names[i])) && g_haveFc;
		dlclose(h);
	}
	if (!g_charIndex) {
		CoreLog("chat glyphs: FreeType's FT_Get_Char_Index not found, not installing");
		return;
	}
	void** history = FindVTable(CLIENT_MODULE, "15CHudChatHistory");
	if (!history || !InSegments(FindExecSegments(CLIENT_MODULE), history[VT_CHudChatHistory_Paint], 1)) {
		CoreLog("chat glyphs: CHudChatHistory's Paint not found, not installing");
		return;
	}

	if (!g_haveFc) CoreLog("chat glyphs: fontconfig not found, only the built-in fallback fonts will be tried");
	for (const char* face : kFallbackFaces) g_fallbacks.push_back({ face, 0, 0, false });
	g_drawnWith = new std::unordered_map<uint64_t, int>();
	// Surface first: its detours pass straight through until Paint is hooked.
	g_originalSetTextFont = (SetTextFontFn)g_surfaceVt[VT_Surface_DrawSetTextFont];
	g_originalPrintText   = (PrintTextFn)g_surfaceVt[VT_Surface_DrawPrintText];
	g_originalPaint       = (PaintFn)history[VT_CHudChatHistory_Paint];
	if (!PatchPointer(&g_surfaceVt[VT_Surface_DrawSetTextFont], (void*)&Hooked_SetTextFont)) {
		CoreLog("chat glyphs: couldn't hook the surface, not installing");
		return;
	}
	if (!PatchPointer(&g_surfaceVt[VT_Surface_DrawPrintText], (void*)&Hooked_PrintText)
			|| !PatchPointer(&history[VT_CHudChatHistory_Paint], (void*)&Hooked_Paint)) {
		PatchPointer(&g_surfaceVt[VT_Surface_DrawPrintText], (void*)g_originalPrintText);
		PatchPointer(&g_surfaceVt[VT_Surface_DrawSetTextFont], (void*)g_originalSetTextFont);
		CoreLog("chat glyphs: couldn't hook the surface, not installing");
		return;
	}
	RegisterCommand();
	CoreLog("chat glyphs: drawing characters the chat font lacks from a fallback font");
}

}
