#include "options_tab.h"
#include "settings.h"
#include "sigs.h"
#include "utils.h"
#include "game_layout.h"

#include <cstdint>
#include <cstring>
#include <cwchar>
#include <vector>

namespace options {
namespace {

typedef uintptr_t VPANEL;
typedef uint32_t  HFont;

typedef void   (*PanelFn)(void*);
typedef void   (*MousePressedFn)(void*, int);
typedef VPANEL (*GetVPanelFn)(void*);
typedef void   (*SetTextFn)(void*, const char*);
typedef void   (*GetTextFn)(void*, char*, int);
typedef void   (*GetAbsPosFn)(void*, VPANEL, int&, int&);
typedef void   (*SetVisibleFn)(void*, VPANEL, bool);
typedef bool   (*IsVisibleFn)(void*, VPANEL);
typedef int    (*GetChildCountFn)(void*, VPANEL);
typedef VPANEL (*GetChildFn)(void*, VPANEL, int);
typedef void   (*GetCursorPosFn)(void*, int&, int&);
typedef void   (*Int4Fn)(void*, int, int, int, int);
typedef void   (*Int2Fn)(void*, int, int);
typedef void   (*SetTextFontFn)(void*, HFont);
typedef void   (*PrintTextFn)(void*, const wchar_t*, int, int);
typedef HFont  (*CreateFontFn)(void*);
typedef bool   (*SetFontGlyphSetFn)(void*, HFont, const char*, int, int, int, int, int, int, int);

const int FONTFLAG_ANTIALIAS = 0x010;
#ifdef _WIN32
const char kFace[] = "Tahoma";
#else
// Steam's runtime ships it; Tahoma would go through fontconfig to whatever it picks.
const char kFace[] = "DejaVu Sans";
#endif
const char kTitle[] = "bhoptools";

struct Row {
	bool Settings::* value;
	bool Settings::* also;
	const wchar_t* name;
};
const Row kRows[] = {
	{ &Settings::rawinput,  nullptr,            L"rawinput2" },
	{ &Settings::predict,   nullptr,            L"prediction" },
	{ &Settings::speedo,    &Settings::hudkeys, L"clientside hud fixes" },
	{ &Settings::jsframe,   nullptr,            L"jumpstats trainers every frame (off: every tick)" },
};
const int kRowCount = sizeof(kRows) / sizeof(kRows[0]);
const int kLeft = 20, kTop = 20, kRowTall = 28, kBox = 14, kWidth = 560;

void* g_panel;    // IPanel
void* g_input;    // vgui IInput
void* g_surface;
void** g_hostVt;  // COptionsSubMultiplayer
void** g_sheetVt;
PanelFn        oPaint;
PanelFn        oSheetLayout;
MousePressedFn oMousePressed;
HFont  g_font;
double g_lastPaint = -1.0;
VPANEL g_page;
std::vector<VPANEL> g_hidden;

VPANEL VPanelOf(void* panel) { return VFunc<GetVPanelFn>(panel, VT_Panel_GetVPanel)(panel); }

void HideChildren(VPANEL page)
{
	if (page != g_page) {
		g_page = page;
		g_hidden.clear();
	}
	const int n = VFunc<GetChildCountFn>(g_panel, VT_IPanel_GetChildCount)(g_panel, page);
	for (int i = 0; i < n; ++i) {
		const VPANEL child = VFunc<GetChildFn>(g_panel, VT_IPanel_GetChild)(g_panel, page, i);
		if (!VFunc<IsVisibleFn>(g_panel, VT_IPanel_IsVisible)(g_panel, child)) continue;
		VFunc<SetVisibleFn>(g_panel, VT_IPanel_SetVisible)(g_panel, child, false);
		g_hidden.push_back(child);
	}
}

int RowAt(VPANEL page)
{
	int cx = 0, cy = 0, px = 0, py = 0;
	VFunc<GetCursorPosFn>(g_input, VT_IInput_GetCursorPos)(g_input, cx, cy);
	VFunc<GetAbsPosFn>(g_panel, VT_IPanel_GetAbsPos)(g_panel, page, px, py);
	const int x = cx - px;
	const int y = cy - py - kTop;
	if (x < kLeft || x > kLeft + kWidth || y < 0) return -1;
	const int row = y / kRowTall;
	return row < kRowCount && y % kRowTall < kRowTall - 8 ? row : -1;
}

void Text(int x, int y, const wchar_t* s, int r, int g, int b)
{
	VFunc<Int4Fn>(g_surface, VT_Surface_DrawSetTextColor)(g_surface, r, g, b, 255);
	VFunc<Int2Fn>(g_surface, VT_Surface_DrawSetTextPos)(g_surface, x, y);
	VFunc<PrintTextFn>(g_surface, VT_Surface_DrawPrintText)(g_surface, s, (int)wcslen(s), 0);
}

void Rect(int op, int x0, int y0, int x1, int y1, int r, int g, int b, int a)
{
	VFunc<Int4Fn>(g_surface, VT_Surface_DrawSetColor)(g_surface, r, g, b, a);
	VFunc<Int4Fn>(g_surface, op)(g_surface, x0, y0, x1, y1);
}

void Draw(VPANEL page)
{
	// A video mode change drops every font, so load it again each time the page opens.
	const double now = MonotonicSeconds();
	if (now - g_lastPaint > 1.0) {
		if (!g_font) g_font = VFunc<CreateFontFn>(g_surface, VT_Surface_CreateFont)(g_surface);
		if (g_font) VFunc<SetFontGlyphSetFn>(g_surface, VT_Surface_SetFontGlyphSet)(
			g_surface, g_font, kFace, 15, 500, 0, 0, FONTFLAG_ANTIALIAS, 0, 0);
	}
	g_lastPaint = now;
	if (!g_font) return;

	VFunc<SetTextFontFn>(g_surface, VT_Surface_DrawSetTextFont)(g_surface, g_font);
	const int hover = RowAt(page);
	const Settings& s = settings::Get();
	for (int i = 0; i < kRowCount; ++i) {
		const int y = kTop + i * kRowTall;
		if (i == hover) Rect(VT_Surface_DrawFilledRect, kLeft - 6, y - 4, kLeft + kWidth, y + kRowTall - 8, 255, 255, 255, 18);
		Rect(VT_Surface_DrawOutlinedRect, kLeft, y + 2, kLeft + kBox, y + 2 + kBox, 200, 200, 200, 255);
		if (s.*kRows[i].value) Rect(VT_Surface_DrawFilledRect, kLeft + 3, y + 5, kLeft + kBox - 3, y + kBox - 1, 255, 176, 0, 255);
		Text(kLeft + kBox + 10, y, kRows[i].name, 235, 235, 235);
	}
}

void Hooked_Paint(void* self)
{
	oPaint(self);
	const VPANEL page = VPanelOf(self);
	HideChildren(page);
	Draw(page);
}

void Hooked_MousePressed(void* self, int code)
{
	const int row = RowAt(VPanelOf(self));
	Settings& s = settings::Get();
	if (row >= 0) {
		bool& v = s.*kRows[row].value;
		v = !v;
		if (kRows[row].also) s.*kRows[row].also = v;
		settings::Changed();
	}
	oMousePressed(self, code);
}

// Every property sheet runs this; only the Options dialog's holds our page. The sheet sizes each tab
// to its text as it lays them out, so a tab just renamed has it lay them out again.
void Hooked_SheetLayout(void* self)
{
	oSheetLayout(self);
	char* sheet = static_cast<char*>(self);
	void** pages = *reinterpret_cast<void***>(sheet + OFF_Sheet_Pages);
	void** tabs = *reinterpret_cast<void***>(sheet + OFF_Sheet_Tabs);
	const int n = *reinterpret_cast<int*>(sheet + OFF_Sheet_PageCount);
	if (!pages || !tabs || n <= 0 || n > 32 || *reinterpret_cast<int*>(sheet + OFF_Sheet_TabCount) != n) return;
	for (int i = 0; i < n; ++i) {
		void* page = pages[i * 2];
		if (!page || *reinterpret_cast<void***>(page) != g_hostVt) continue;
		char text[64] = {};
		VFunc<GetTextFn>(tabs[i], VT_Label_GetText)(tabs[i], text, sizeof(text));
		if (strcmp(text, kTitle) == 0) return;
		VFunc<SetTextFn>(tabs[i], VT_Label_SetText)(tabs[i], kTitle);
		oSheetLayout(self);
		return;
	}
}

}

bool Install()
{
	if (g_hostVt) return true;
	g_panel = CreateInterfaceIn(VGUI_MODULE, "VGUI_Panel009");
	g_input = CreateInterfaceIn(VGUI_MODULE, "VGUI_Input005");
	g_surface = CreateInterfaceIn(SURFACE_MODULE, "VGUI_Surface030");
	void** host = g_panel && g_input && g_surface ? FindVTable(GAMEUI_MODULE, RTTI_OptionsSubMultiplayer) : nullptr;
	void** sheet = host ? FindVTable(GAMEUI_MODULE, RTTI_PropertySheet) : nullptr;
	if (!sheet) return false;
	// zastools takes the same page over; leave it to it.
	if (!InModuleCode(GAMEUI_MODULE, host[VT_Panel_Paint]) || !InModuleCode(GAMEUI_MODULE, sheet[VT_Panel_PerformLayout])) {
		CoreLog("options: the Multiplayer page is already taken over, not adding ours");
		g_hostVt = host;
		return true;
	}
	oPaint = reinterpret_cast<PanelFn>(host[VT_Panel_Paint]);
	oMousePressed = reinterpret_cast<MousePressedFn>(host[VT_Panel_OnMousePressed]);
	oSheetLayout = reinterpret_cast<PanelFn>(sheet[VT_Panel_PerformLayout]);
	if (!PatchPointer(&host[VT_Panel_Paint], reinterpret_cast<void*>(&Hooked_Paint))
			|| !PatchPointer(&host[VT_Panel_OnMousePressed], reinterpret_cast<void*>(&Hooked_MousePressed))
			|| !PatchPointer(&sheet[VT_Panel_PerformLayout], reinterpret_cast<void*>(&Hooked_SheetLayout))) {
		CoreLog("options: couldn't patch GameUI's vtables");
		// Put back whichever went in, since Uninstall skips a half-done install.
		PatchPointer(&host[VT_Panel_Paint], reinterpret_cast<void*>(oPaint));
		PatchPointer(&host[VT_Panel_OnMousePressed], reinterpret_cast<void*>(oMousePressed));
		g_hostVt = host;
		return true;
	}
	g_hostVt = host;
	g_sheetVt = sheet;
	return true;
}

#ifdef _WIN32
void Uninstall()
{
	if (!g_sheetVt) return;
	PatchPointer(&g_hostVt[VT_Panel_Paint], reinterpret_cast<void*>(oPaint));
	PatchPointer(&g_hostVt[VT_Panel_OnMousePressed], reinterpret_cast<void*>(oMousePressed));
	PatchPointer(&g_sheetVt[VT_Panel_PerformLayout], reinterpret_cast<void*>(oSheetLayout));
	for (VPANEL child : g_hidden) VFunc<SetVisibleFn>(g_panel, VT_IPanel_SetVisible)(g_panel, child, true);
	g_hidden.clear();
	g_sheetVt = nullptr;
}
#endif

}
