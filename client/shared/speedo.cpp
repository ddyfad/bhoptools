#include "speedo.h"
#include "settings.h"
#include "hudkeys.h"
#include "game_layout.h"
#include "sigs.h"
#include "utils.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace speedo {
namespace {

const int kHistory = 64;
const double kHudTimeout = 0.3;
// bgs-jumpstats sends its keys only on a change or every 50 commands.
const double kKeysTimeout = 1.0;
// Without prediction for a while the player may be dead or spectating, and the keys somebody else's.
const double kAliveTimeout = 0.25;

DispatchUserMessageFn g_orig;
void*  g_client;
#ifdef _WIN32
// CUserMessages*, filled in by client.dll's CRT init, so read on use.
void* const* g_userMessages;
void* UserMessages() { return g_userMessages ? *g_userMessages : nullptr; }
#else
void*  g_userMessages;
void* UserMessages() { return g_userMessages; }
#endif
int    g_hintMsg = -1;
int    g_textMsg = -1;

const int kHudExtraSlots = 10;
const int kHudMessageBytes = 0x58;
const int kHudNameOffset = 0x30;
const int kHudTextOffset = 0x38;
const int kHudTextBytes = 512;
const int kVT_IVEngineClient_TextMessageGet = 10;
static_assert(sizeof(void*) == 8, "client_textmessage_t layout is 64-bit");

typedef void* (*TextMessageGetFn)(void*, const char*);
TextMessageGetFn g_origTextMessageGet;
void*  g_engineIface;
bool   g_hudSlots;
int    g_hudMsg = -1;
int    g_hudVirtual = -1;
alignas(16) unsigned char g_hudMessages[kHudExtraSlots][kHudMessageBytes];
char   g_hudText[kHudExtraSlots][kHudTextBytes];

void* Hooked_TextMessageGet(void* self, const char* name)
{
	if (g_hudVirtual >= 0 && name && !strncmp(name, "__NETMESSAGE__", 14)) return g_hudMessages[g_hudVirtual];
	return g_origTextMessageGet(self, name);
}

bool InstallHudSlots()
{
	g_engineIface = CreateInterfaceIn(ENGINE_MODULE, "VEngineClient014");
	if (!g_engineIface) return false;
	void** vt = *reinterpret_cast<void***>(g_engineIface);
	g_origTextMessageGet = reinterpret_cast<TextMessageGetFn>(vt[kVT_IVEngineClient_TextMessageGet]);
	const unsigned char* m1 = static_cast<const unsigned char*>(g_origTextMessageGet(g_engineIface, "__NETMESSAGE__1"));
	const unsigned char* m2 = static_cast<const unsigned char*>(g_origTextMessageGet(g_engineIface, "__NETMESSAGE__2"));
	if (!m1 || !m2 || m2 - m1 != kHudMessageBytes || g_origTextMessageGet(g_engineIface, "__bhoptools_none__")) {
		CoreLog("hud slots: IVEngineClient::TextMessageGet isn't where expected, HUD stays on six channels");
		return false;
	}
	const char* name = *reinterpret_cast<const char* const*>(m1 + kHudNameOffset);
	if (!name || strcmp(name, "__NETMESSAGE__1") || !*reinterpret_cast<void* const*>(m1 + kHudTextOffset)) {
		CoreLog("hud slots: client_textmessage_t doesn't look as expected, HUD stays on six channels");
		return false;
	}
	for (int i = 0; i < kHudExtraSlots; ++i) {
		memcpy(g_hudMessages[i], m1, kHudMessageBytes);
		*reinterpret_cast<char**>(g_hudMessages[i] + kHudTextOffset) = g_hudText[i];
	}
	if (!PatchPointer(&vt[kVT_IVEngineClient_TextMessageGet], reinterpret_cast<void*>(&Hooked_TextMessageGet))) {
		CoreLog("hud slots: couldn't hook TextMessageGet, HUD stays on six channels");
		return false;
	}
	return true;
}

const char* g_watchName;
int    g_watchMsg = -1;
bool (*g_watchFn)(const BfRead&);

int    g_speed2d[kHistory], g_speed3d[kHistory];
int    g_count, g_next;

char   g_before[256], g_after[256];
bool   g_twoD;
bool   g_haveHud;
bool   g_held;
bool   g_enabled = true;
double g_hudTime;
double g_aliveTime = -1.0;

int    g_cmdButtons;
float  g_cmdYaw, g_cmdYawDiff;
bool   g_haveCmd;
int    g_keysLayout = -1;
double g_keysTime;
std::string g_keysShown;

// Registered after we attach, so looked up on first use.
int MessageId(const char* want)
{
	void* um = UserMessages();
	if (!um) return -1;
	const char* nodes = *reinterpret_cast<char* const*>(static_cast<char*>(um) + 0x8);
	const int count = *reinterpret_cast<const int*>(static_cast<char*>(um) + 0x1c);
	for (int i = 0; nodes && i < count; ++i) {
		const char* name = *reinterpret_cast<const char* const*>(nodes + i * 32 + 0x10);
		if (name && !strcmp(name, want)) return i;
	}
	return -1;
}

void Show()
{
	const int last = (g_next + kHistory - 1) % kHistory;
	const int speed = g_twoD ? g_speed2d[last] : g_speed3d[last];
	unsigned char buf[264] = {};  // zero padded: bf_read reads whole dwords
	int len = snprintf(reinterpret_cast<char*>(buf), 255, "%s%d%s", g_before, speed, g_after);
	if (len > 254) len = 254;
	len += 1;
	BfRead msg = { buf, len, len * 8, 0, false, false, "speedo" };
	g_orig(g_client, g_hintMsg, &msg);
}

bool IsDigit(char c) { return c >= '0' && c <= '9'; }

bool Near(int v, const int* speeds, int count)
{
	for (int i = 0; i < count; ++i) if (abs(speeds[i] - v) <= 1) return true;
	return false;
}

int FindSpeed(const char* text, const int* speed2d, const int* speed3d, int count, int* len, bool* twoD)
{
	int found = -1;
	for (int i = 0; text[i]; ) {
		if (!IsDigit(text[i])) { ++i; continue; }
		int j = i;
		while (IsDigit(text[j])) ++j;
		const char before = i ? text[i - 1] : ' ';
		const char after = text[j];
		const bool plain = before != '.' && before != ':' && before != '+' && before != '-'
			&& after != '.' && after != ':' && after != '%' && (unsigned char)after != 0xEF && j - i <= 5;
		if (plain) {
			const int v = atoi(text + i);
			const bool m2 = Near(v, speed2d, count), m3 = Near(v, speed3d, count);
			if (m2 || m3) {
				if (found >= 0) return -1;
				found = i;
				*len = j - i;
				*twoD = m2;
			}
		}
		i = j;
	}
	return found;
}

bool OnHint(const BfRead& msg)
{
	if (!g_enabled || !settings::Get().speedo) return false;
	if (g_held) {
		g_haveHud = false;
		return false;
	}
	if (msg.curBit & 7) return false;
	const int start = msg.curBit / 8;
	char text[256];
	int n = 0;
	for (int i = start; i < msg.bytes && n < (int)sizeof(text) - 1 && msg.data[i]; ++i) text[n++] = (char)msg.data[i];
	text[n] = '\0';

	int len;
	bool twoD;
	const int at = g_count ? FindSpeed(text, g_speed2d, g_speed3d, g_count, &len, &twoD) : -1;
	g_haveHud = at >= 0;
	if (!g_haveHud) return false;

	memcpy(g_before, text, at);
	g_before[at] = '\0';
	strcpy(g_after, text + at + len);
	g_twoD = twoD;
	g_hudTime = MonotonicSeconds();
	Show();
	return true;
}

// Not tied to prediction: the keys only need our own commands.
bool OwnHud()
{
	return g_haveCmd && g_aliveTime >= 0.0 && MonotonicSeconds() - g_aliveTime < kAliveTimeout;
}

void ShowKeys(const std::string& text)
{
	const std::string payload = hudkeys::CenterTextMsg(text);
	unsigned char buf[264] = {};  // zero padded: bf_read reads whole dwords
	if (payload.size() > 256) return;
	memcpy(buf, payload.data(), payload.size());
	const int len = (int)payload.size();
	BfRead msg = { buf, len, len * 8, 0, false, false, "hudkeys" };
	g_keysShown = text;
	g_orig(g_client, g_textMsg, &msg);
}

// The key overlay goes out every tick with the keys of a command a ping old; it's drawn from the newest one.
bool OnTextMsg(const BfRead& msg)
{
	const int start = msg.curBit / 8;
	if ((msg.curBit & 7) || start >= msg.bytes || msg.data[start] != 4) return false;
	// Any other center text ends the redraws until the overlay comes back.
	std::string text;
	g_keysLayout = OwnHud() && settings::Get().hudkeys && hudkeys::ReadCenterText(msg.data + start, msg.bytes - start, text)
		? hudkeys::Parse(text.c_str()) : -1;
	if (g_keysLayout < 0) return false;
	g_keysTime = MonotonicSeconds();
	ShowKeys(hudkeys::Format(g_keysLayout, g_cmdButtons, g_cmdYawDiff));
	return true;
}

struct HudElement { bool used; int channel; float x, y; double expires, lastUse; };
HudElement g_hudElements[kHudExtraSlots];

struct HudFields { int channel, effect; float x, y, fadeIn, fadeOut, hold, fxTime; bool empty; };

bool ParseHudMsg(const unsigned char* d, int bytes, HudFields& f)
{
	const int kHeader = 1 + 4 + 4 + 4 + 4 + 1 + 16;
	if (bytes < kHeader + 1) return false;
	f.channel = d[0];
	memcpy(&f.x, d + 1, 4);
	memcpy(&f.y, d + 5, 4);
	f.effect = d[17];
	memcpy(&f.fadeIn, d + 18, 4);
	memcpy(&f.fadeOut, d + 22, 4);
	memcpy(&f.hold, d + 26, 4);
	memcpy(&f.fxTime, d + 30, 4);
	f.empty = d[kHeader] == 0;
	return true;
}

bool RunHudHandler(void* self, int type, const BfRead* msg, int at, int slot)
{
	unsigned char buf[608] = {};
	memcpy(buf, msg->data, msg->bytes);
	buf[at] = 0;
	BfRead copy = *msg;
	copy.data = buf;
	g_hudVirtual = slot;
	const bool handled = g_orig(self, type, &copy);
	g_hudVirtual = -1;
	g_hudText[slot][kHudTextBytes - 1] = '\0';
	return handled;
}

bool CallOriginal(void* self, int type, BfRead* msg)
{
	if (g_hudSlots && g_hudMsg >= 0 && type == g_hudMsg && msg && !(msg->curBit & 7) && msg->bytes > 0 && msg->bytes <= 600) {
		const int at = msg->curBit / 8;
		HudFields f;
		if (at < msg->bytes && ParseHudMsg(msg->data + at, msg->bytes - at, f)) {
			const double now = MonotonicSeconds();
			if (f.empty) {
				for (int i = 0; i < kHudExtraSlots; ++i) {
					if (g_hudElements[i].used && g_hudElements[i].channel == f.channel) {
						g_hudElements[i].used = false;
						RunHudHandler(self, type, msg, at, i);
					}
				}
				return true;
			}
			int slot = -1, freeSlot = -1, oldest = 0;
			for (int i = 0; i < kHudExtraSlots && slot < 0; ++i) {
				const HudElement& e = g_hudElements[i];
				if (e.used && e.channel == f.channel && fabsf(e.x - f.x) < 0.0005f && fabsf(e.y - f.y) < 0.0005f) slot = i;
				else if (freeSlot < 0 && (!e.used || e.expires < now)) freeSlot = i;
				if (e.lastUse < g_hudElements[oldest].lastUse) oldest = i;
			}
			if (slot < 0) slot = freeSlot >= 0 ? freeSlot : oldest;
			HudElement& e = g_hudElements[slot];
			e.used = true;
			e.channel = f.channel;
			e.x = f.x;
			e.y = f.y;
			e.lastUse = now;
			e.expires = now + f.fadeIn + f.hold + f.fadeOut + 0.25;
			return RunHudHandler(self, type, msg, at, slot);
		}
	}
	return g_orig(self, type, msg);
}

bool Hooked_Dispatch(void* self, int type, BfRead* msg)
{
	if (g_hintMsg < 0) g_hintMsg = MessageId("HintText");
	if (g_textMsg < 0) g_textMsg = MessageId("TextMsg");
	if (g_hudSlots && g_hudMsg < 0) g_hudMsg = MessageId("HudMsg");
	if (g_watchFn && g_watchMsg < 0) g_watchMsg = MessageId(g_watchName);
	if (msg && g_watchFn && type == g_watchMsg && g_watchFn(*msg)) return true;
	if (msg && g_hintMsg >= 0 && type == g_hintMsg && OnHint(*msg)) return true;
	if (msg && g_textMsg >= 0 && type == g_textMsg && OnTextMsg(*msg)) return true;
	return CallOriginal(self, type, msg);
}

void SettingCmd(const CCommandX& c, bool Settings::* value, const char* name)
{
	if (c.argc > 1) {
		settings::Get().*value = atoi(c.argv[1]) != 0;
		settings::Changed();
	}
	CoreLog("%s %d", name, settings::Get().*value ? 1 : 0);
}

void Cmd_HudSpeed(const CCommandX& c) { SettingCmd(c, &Settings::speedo, "hud_speed"); }
void Cmd_HudKeys(const CCommandX& c) { SettingCmd(c, &Settings::hudkeys, "hud_keys"); }
void Cmd_Viewpunch(const CCommandX& c) { SettingCmd(c, &Settings::viewpunch, "bhoptools_viewpunch"); }

ConCommandX g_cmds[3];

}  // namespace

void RegisterCommands()
{
	static bool done;
	if (done) return;
	done = true;
	void* cvar = CreateInterfaceIn(VSTDLIB_MODULE, "VEngineCvar004");
	void* echo = cvar ? VFunc<void* (*)(void*, const char*)>(cvar, VT_Cvar_FindBase)(cvar, "echo") : nullptr;
	const char* echoName = echo ? static_cast<ConCommandX*>(echo)->name : nullptr;
	if (!echo || !VFunc<bool (*)(void*)>(echo, VT_CCB_IsCommand)(echo) || !echoName || strcmp(echoName, "echo")) {
		CoreLog("speedo: couldn't register hud_speed, hud_keys and bhoptools_viewpunch");
		return;
	}
	const struct { const char* name; const char* help; void (*fn)(const CCommandX&); } defs[3] = {
		{ "hud_speed", "Speed in shavit's HUD from local prediction (0/1).", Cmd_HudSpeed },
		{ "hud_keys", "Keys in shavit's key overlay from the newest local command (0/1).", Cmd_HudKeys },
		{ "bhoptools_viewpunch", "Remove viewpunch, e.g. from fall damage (0/1, on by default).", Cmd_Viewpunch },
	};
	for (int i = 0; i < 3; ++i) {
		ConCommandX& c = g_cmds[i];
		c.vtable = *reinterpret_cast<void**>(echo);
		c.name = defs[i].name;
		c.help = defs[i].help;
		c.callback = reinterpret_cast<void*>(defs[i].fn);
		c.bits = 0x02;  // new-style callback taking const CCommand&
		VFunc<void (*)(void*, void*)>(cvar, VT_Cvar_Register)(cvar, &c);
	}
}

void Attach(void* client)
{
	void** vt = *reinterpret_cast<void***>(client);
	const unsigned char* fn = static_cast<const unsigned char*>(vt[VT_Client_DispatchUserMessage]);
#ifdef _WIN32
	const bool expected = fn[0] == 0x48 && fn[1] == 0x8b && fn[2] == 0x0d && fn[7] == 0xe9;
#else
	const bool expected = fn[0] == 0x48 && fn[1] == 0x8d && fn[2] == 0x05 && fn[7] == 0x48 && fn[8] == 0x8b && fn[9] == 0x38 && fn[10] == 0xe9;
#endif
	if (!expected) {
		CoreLog("speedo: DispatchUserMessage doesn't look as expected, HUD speed stays as the server sends it");
		return;
	}
	g_orig = reinterpret_cast<DispatchUserMessageFn>(const_cast<unsigned char*>(fn));
	g_client = client;
	// The rip-relative operand at the start of the entry is the usermessages pointer's address.
	int32_t disp;
	memcpy(&disp, fn + 3, 4);
#ifdef _WIN32
	g_userMessages = reinterpret_cast<void* const*>(fn + 7 + disp);
	if (!PatchPointer(&vt[VT_Client_DispatchUserMessage], reinterpret_cast<void*>(&Hooked_Dispatch))) {
		g_userMessages = nullptr;
		CoreLog("speedo: couldn't hook DispatchUserMessage, HUD speed stays as the server sends it");
	}
	const bool hooked = g_userMessages != nullptr;
#else
	g_userMessages = *reinterpret_cast<void* const*>(fn + 7 + disp);
	const bool hooked = g_userMessages && PatchPointer(&vt[VT_Client_DispatchUserMessage], reinterpret_cast<void*>(&Hooked_Dispatch));
	if (!hooked)
		CoreLog("speedo: couldn't hook DispatchUserMessage, HUD speed stays as the server sends it");
#endif
	if (hooked)
		g_hudSlots = InstallHudSlots();
}

#ifdef _WIN32
void Detach()
{
	if (g_userMessages) PatchPointer(&(*reinterpret_cast<void***>(g_client))[VT_Client_DispatchUserMessage], reinterpret_cast<void*>(g_orig));
	if (g_hudSlots) PatchPointer(&(*reinterpret_cast<void***>(g_engineIface))[kVT_IVEngineClient_TextMessageGet], reinterpret_cast<void*>(g_origTextMessageGet));
	g_hudSlots = false;
	g_userMessages = nullptr;
}
#endif

void SetEnabled(bool on)
{
	g_enabled = on;
	g_haveHud = false;
	g_keysLayout = -1;
}

DispatchUserMessageFn OriginalDispatch()
{
	return g_orig;
}

void Watch(const char* name, bool (*fn)(const BfRead&))
{
	g_watchName = name;
	g_watchMsg = -1;
	g_watchFn = fn;
}

int WatchedMessage()
{
	return g_watchMsg;
}

int UserMessageId(const char* name)
{
	return MessageId(name);
}

void Dispatch(int id, const void* data, int len)
{
	unsigned char buf[520] = {};  // zero padded: bf_read reads whole dwords
	if (!g_orig || !UserMessages() || id < 0 || len <= 0 || len > 512) return;
	memcpy(buf, data, len);
	BfRead msg = { buf, len, len * 8, 0, false, false, "bhoptools" };
	if (g_hudSlots && g_hudMsg < 0) g_hudMsg = MessageId("HudMsg");
	CallOriginal(g_client, id, &msg);
}

void SetHeld(bool held)
{
	g_held = held;
	if (held) g_haveHud = false;
}

void OnCommand(void* input, int sequence)
{
	if (!input) return;
	const char* cmd = VFunc<char* (*)(void*, int)>(input, VT_IInput_GetUserCmd)(input, sequence);
	if (!cmd || *reinterpret_cast<const int*>(cmd + UC_CommandNumber) != sequence) return;
	const float yaw = *reinterpret_cast<const float*>(cmd + UC_ViewAngles + 4);
	g_cmdButtons = *reinterpret_cast<const int*>(cmd + UC_Buttons);
	g_cmdYawDiff = g_haveCmd ? hudkeys::AngleDiff(yaw, g_cmdYaw) : 0.0f;
	g_cmdYaw = yaw;
	g_haveCmd = true;
	if (g_keysLayout < 0 || !g_userMessages || !OwnHud() || !settings::Get().hudkeys || MonotonicSeconds() - g_keysTime >= kKeysTimeout) return;
	const std::string text = hudkeys::Format(g_keysLayout, g_cmdButtons, g_cmdYawDiff);
	if (text != g_keysShown) ShowKeys(text);
}

// Re-predicted commands are recorded too, so the server's HUD still matches after a correction.
void OnMove(bool alive, const float velocity[3], bool firstRun)
{
	if (!alive) {
		g_count = g_next = 0;
		g_haveHud = false;
		g_keysLayout = -1;
		g_aliveTime = -1.0;
		return;
	}
	if (firstRun) g_aliveTime = MonotonicSeconds();
	const float h = sqrtf(velocity[0] * velocity[0] + velocity[1] * velocity[1]);
	g_speed2d[g_next] = (int)lroundf(h);
	g_speed3d[g_next] = (int)lroundf(sqrtf(h * h + velocity[2] * velocity[2]));
	g_next = (g_next + 1) % kHistory;
	if (g_count < kHistory) ++g_count;
	if (firstRun && g_userMessages && g_haveHud && MonotonicSeconds() - g_hudTime < kHudTimeout) Show();
}

}
