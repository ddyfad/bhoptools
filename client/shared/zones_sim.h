// What shavit does with its zones once the trigger sim touches them.
#pragma once

#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// zones.inc
enum ZsType { ZS_START, ZS_END, ZS_RESPAWN, ZS_STOP, ZS_SLAY, ZS_FREESTYLE, ZS_SPEEDLIMIT, ZS_TELEPORT,
	ZS_SPAWN, ZS_EASYBHOP, ZS_SLIDE, ZS_AIRACCEL, ZS_STAGE, ZS_NOTIMERGRAVITY, ZS_GRAVITY, ZS_SPEEDMOD,
	ZS_NOJUMP, ZS_AUTOBHOP, ZS_TYPES };
enum ZsForm { ZS_FORM_BOX, ZS_FORM_MULTIPLE, ZS_FORM_TELEPORT, ZS_FORM_BUTTON };
static const int ZS_MAX = 128; // MAX_ZONES

// bhoptools_zplayer flags; must match the server plugin.
static const int ZSP_AUTO           = 1 << 0;
static const int ZSP_STYLE_AUTOBHOP = 1 << 1;
static const int ZSP_STYLE_EASYBHOP = 1 << 2;
static const int ZSP_PAUSED         = 1 << 3;
static const int ZSP_ENFORCE        = 1 << 4;
static const int ZSP_STOPPED        = 1 << 5;
static const int ZSP_TEAM           = 1 << 6;

static const int ZS_IN_JUMP = 1 << 1;
static const int ZS_MOVETYPE_ISOMETRIC = 1, ZS_MOVETYPE_WALK = 2, ZS_MOVETYPE_LADDER = 9;

struct ZsZone {
	int   type = 0, track = 0, form = ZS_FORM_BOX, data = 0, hammerId = 0;
	float origin[3] = { 0, 0, 0 };
	float half[3] = { 0, 0, 0 };
	float dest[3] = { 0, 0, 0 };
	int   sim = -1; // hooked forms: the trigger sim's index of the map trigger
};

struct ZsSet {
	std::vector<ZsZone> zones;
	bool active = false;
	int  unknownTypes = 0; // types with a hooked trigger the sim doesn't have
	int  types = 0;
};

struct ZsSettings {
	int   track = 0, flags = 0;
	float gravity = 1.0f, airaccelerate = 0.0f;
	int   prespeed = 0, prespeedType = 0, blockPrejump = 0, noZAxisSpeed = 0;
	float limit = 0.0f;
};

// Per command, so in every sim snapshot.
struct ZsState {
	uint8_t in[ZS_MAX];
	uint8_t linked[ZS_MAX];
	uint8_t stale[ZS_MAX];
	float startLimit = 0.0f;
	float airaccelerate = 0.0f;
	int   lastMoveType = -1;
	bool  prevGround = false;
	int   teleportKey = INT_MIN / 2;

	ZsState() { ClearTouches(); }
	void ClearTouches()
	{
		memset(in, 0, sizeof in);
		memset(linked, 0, sizeof linked);
		memset(stale, 0, sizeof stale);
	}
};

struct ZsSettingsLog {
	struct Entry { int fromCmd; ZsSettings s; };
	std::vector<Entry> log;
	bool known = false;

	void Clear() { log.clear(); known = false; }

	void Add(int fromCmd, const ZsSettings& s)
	{
		size_t at = log.size();
		while (at > 0 && log[at - 1].fromCmd > fromCmd) --at;
		if (at > 0 && log[at - 1].fromCmd == fromCmd) log[at - 1].s = s;
		else log.insert(log.begin() + at, Entry{ fromCmd, s });
		if (log.size() > 16) log.erase(log.begin());
		known = true;
	}

	const ZsSettings& For(int cmd) const
	{
		static const ZsSettings none;
		for (size_t i = log.size(); i-- > 0; )
			if (cmd == INT_MIN || log[i].fromCmd <= cmd) return log[i].s;
		return log.empty() ? none : log.front().s;
	}
};

static inline float ZsBits(int v)
{
	float f;
	memcpy(&f, &v, 4);
	return f;
}

static inline bool ZsKnown(const ZsSet& set, int type)
{
	return set.active && !(set.unknownTypes & (1 << type));
}

static inline bool ZsHas(const ZsSet& set, int type)
{
	return (set.types & (1 << type)) != 0;
}

// Shavit_InsideZoneGetID: the lowest index, -1 for none. track -1 is any.
static inline int ZsInsideId(const ZsSet& set, const ZsState& st, int type, int track)
{
	for (size_t i = 0; i < set.zones.size() && i < (size_t)ZS_MAX; ++i)
		if (st.in[i] && set.zones[i].type == type && (track == -1 || set.zones[i].track == track)) return (int)i;
	return -1;
}

static inline bool ZsInside(const ZsSet& set, const ZsState& st, int type, int track)
{
	return ZsInsideId(set, st, type, track) >= 0;
}

// The zone's entity has a studio model, so the engine's touch test is the partition query alone, against
// bounds a unit wider than the box.
static inline bool ZsBoxTouches(const ZsZone& z, const float feet[3], const float mins[3], const float maxs[3])
{
	for (int c = 0; c < 3; ++c) {
		const float ext = (maxs[c] - mins[c]) * 0.5f;
		const float start = feet[c] + (mins[c] + maxs[c]) * 0.5f;
		const float lo = (z.origin[c] + -z.half[c]) - 1.0f;
		const float hi = (z.origin[c] + z.half[c]) + 1.0f;
		if (start - ext > hi || start + ext < lo) return false;
	}
	return true;
}

// `type` is the resolved prespeed_type.
static inline bool ZsClampPrestrafe(const ZsSettings& s, const ZsState& st, int type, float v[3])
{
	float limit = st.startLimit > 0.0f ? st.startLimit : s.limit;
	if (type < 4 && v[2] > 0.0f) limit /= 3.0f;
	const float xy = sqrtf(v[0] * v[0] + v[1] * v[1]);
	const float scale = limit / xy;
	if (!(scale < 1.0f)) return false;
	v[0] *= scale;
	v[1] *= scale;
	if (type != 5 && type != 6) v[2] *= scale;
	return true;
}

struct ZsCmd {
	float velocity[3]; // abs velocity
	int   buttons;
	float upmove;
	bool  onGround;    // m_hGroundEntity set
	bool  noclip;
	bool  velocitySet; // shavit-misc's DumbSetVelocity: velocity written, basevelocity zeroed
};

// shavit-core's OnPlayerRunCmd: shavit-misc's prespeed block from its Shavit_OnUserCmdPre, then the
// pre-jump block and no-jump zones.
static inline void ZsPreCmd(const ZsSet& set, const ZsSettings& s, const ZsState& st, ZsCmd& c)
{
	c.velocitySet = false;
	if (!set.active || (s.flags & ZSP_PAUSED) || !ZsKnown(set, ZS_START)) return;
	const bool inStart = ZsInside(set, st, ZS_START, s.track);

	if (inStart && !c.noclip && s.prespeed == 0) {
		const int type = s.prespeedType;
		if ((type == 2 || type == 3) && !st.prevGround && c.onGround && (c.buttons & ZS_IN_JUMP)) {
			c.velocity[0] = c.velocity[1] = c.velocity[2] = 0.0f;
			c.velocitySet = true;
		} else if (type == 1 || (type >= 3 && type != 6)) {
			// Type 6's jump limit counts server ticks since landing.
			c.velocitySet = ZsClampPrestrafe(s, st, type, c.velocity);
		}
	}

	if (inStart && s.blockPrejump && s.prespeed == 0 && (c.upmove > 0.0f || (c.buttons & ZS_IN_JUMP))) {
		c.upmove = 0.0f;
		c.buttons &= ~ZS_IN_JUMP;
	}

	if (ZsKnown(set, ZS_NOJUMP) && ZsInside(set, st, ZS_NOJUMP, s.track)) {
		c.upmove = 0.0f;
		c.buttons &= ~ZS_IN_JUMP;
	}
}

// shavit-core's PreThinkPost gravity. False when it can't be predicted.
static inline bool ZsPreThinkGravity(const ZsSet& set, const ZsSettings& s, ZsState& st, bool alive, int moveType, float* gravity)
{
	if (!alive) return true;
	const int last = st.lastMoveType;
	st.lastMoveType = moveType;
	if (!ZsKnown(set, ZS_GRAVITY) || !ZsKnown(set, ZS_NOTIMERGRAVITY)) return false;
	if (moveType != ZS_MOVETYPE_WALK && moveType != ZS_MOVETYPE_ISOMETRIC) return true;
	if (ZsInside(set, st, ZS_NOTIMERGRAVITY, s.track)) return true;
	float g = 0.0f;
	const int id = ZsInsideId(set, st, ZS_GRAVITY, s.track);
	if (id >= 0) g = ZsBits(set.zones[id].data);
	const float clientg = *gravity;
	if (g == 0.0f && s.gravity != 1.0f && (last == ZS_MOVETYPE_LADDER || clientg == 1.0f || clientg == 0.0f)) g = s.gravity;
	if (g != 0.0f) *gravity = g;
	return true;
}

// What sv_airaccelerate shavit-core's PreThinkPost sets; false when unknown.
static inline bool ZsAiraccelerate(const ZsSet& set, const ZsSettings& s, const ZsState& st, float* aa)
{
	if (!ZsKnown(set, ZS_AIRACCEL)) return false;
	*aa = ZsInside(set, st, ZS_AIRACCEL, s.track) ? st.airaccelerate : s.airaccelerate;
	return true;
}

static inline bool ZsAutobhop(const ZsSet& set, const ZsSettings& s, const ZsState& st, bool* on)
{
	if (!ZsKnown(set, ZS_AUTOBHOP)) return false;
	*on = (s.flags & ZSP_AUTO) && ((s.flags & ZSP_STYLE_AUTOBHOP) || ZsInside(set, st, ZS_AUTOBHOP, s.track));
	return true;
}

static inline bool ZsEasybhop(const ZsSet& set, const ZsSettings& s, const ZsState& st, bool* on)
{
	if (!ZsKnown(set, ZS_EASYBHOP)) return false;
	*on = (s.flags & ZSP_STYLE_EASYBHOP) || ZsInside(set, st, ZS_EASYBHOP, s.track);
	return true;
}

// shavit-zones' StartTouchPost before the per-type effects: false when it returns early.
static inline bool ZsStartTouchApplies(const ZsZone& z, const ZsSettings& s)
{
	return !((s.flags & ZSP_ENFORCE) && z.type > ZS_END && z.track != s.track);
}

// The membership StartTouchPost sets and what shavit-core and shavit-misc keep from Shavit_OnEnterZone.
static inline void ZsEnter(const ZsZone& z, const ZsSettings& s, ZsState& st, int k)
{
	st.in[k] = 1;
	if (z.track != s.track) return;
	if (z.type == ZS_START) st.startLimit = (float)z.data;
	else if (z.type == ZS_AIRACCEL) st.airaccelerate = (float)z.data;
}

// Shavit_OnStartPre's clamp, from the start zone's TouchPost through StartTimer and CanStartTimer.
static inline bool ZsStartTimerClamp(const ZsZone& z, const ZsSettings& s, const ZsState& st, bool alive, bool noclip, float v[3])
{
	const bool startTimer = (s.flags & ZSP_STOPPED) || s.track != 0 || z.track == 0;
	if (!startTimer || !alive || !(s.flags & ZSP_TEAM) || s.prespeed != 0 || noclip) return false;
	if (s.noZAxisSpeed && v[2] != 0.0f) return false;
	const int type = s.prespeedType;
	return (type == 1 || type >= 3) && ZsClampPrestrafe(s, st, type, v);
}

// Hex float bits as SourcePawn's %x writes them, a negative cell with a minus sign.
static inline float ZsHexFloat(const char* s)
{
	const bool neg = *s == '-';
	if (neg) ++s;
	uint32_t bits = 0;
	for (; *s; ++s) {
		const char ch = *s;
		const int d = ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1;
		if (d < 0) break;
		bits = bits << 4 | (uint32_t)d;
	}
	if (neg) bits = 0u - bits;
	float f;
	memcpy(&f, &bits, 4);
	return std::isfinite(f) && fabsf(f) < 1e6f ? f : 0.0f;
}

// One zone's line of the published list: type track form data hammerid origin[3] half[3] dest[3].
static inline bool ZsParseZone(const char* const* f, int n, ZsZone& z)
{
	if (n < 14) return false;
	z.type = atoi(f[0]);
	z.track = atoi(f[1]);
	z.form = atoi(f[2]);
	z.data = atoi(f[3]);
	z.hammerId = atoi(f[4]);
	for (int c = 0; c < 3; ++c) {
		z.origin[c] = ZsHexFloat(f[5 + c]);
		z.half[c] = ZsHexFloat(f[8 + c]);
		z.dest[c] = ZsHexFloat(f[11 + c]);
	}
	z.sim = -1;
	return z.type >= 0 && z.type < ZS_TYPES;
}

// The published list: one zone per line, in shavit's index order. Nothing is kept unless every line parses.
static inline bool ZsParseList(const char* text, size_t len, std::vector<ZsZone>& out)
{
	out.clear();
	const char* s = text;
	const char* end = text + len;
	while (s < end) {
		const char* eol = (const char*)memchr(s, '\n', (size_t)(end - s));
		if (!eol) eol = end;
		std::string line(s, eol);
		s = eol + 1;
		std::vector<const char*> f;
		for (char* p = &line[0]; *p;) {
			f.push_back(p);
			while (*p && *p != ' ') ++p;
			if (*p) *p++ = 0;
		}
		ZsZone z;
		if (out.size() >= (size_t)ZS_MAX || !ZsParseZone(f.data(), (int)f.size(), z)) {
			out.clear();
			return false;
		}
		out.push_back(z);
	}
	return true;
}

// bhoptools_zplayer after its session and command: track flags gravity airaccelerate prespeed
// prespeed_type blockprejump nozaxisspeed limit.
static inline bool ZsParseSettings(const char* const* f, int n, ZsSettings& s)
{
	if (n < 9) return false;
	s.track = atoi(f[0]);
	s.flags = atoi(f[1]);
	s.gravity = ZsHexFloat(f[2]);
	s.airaccelerate = ZsHexFloat(f[3]);
	s.prespeed = atoi(f[4]);
	s.prespeedType = atoi(f[5]);
	s.blockPrejump = atoi(f[6]);
	s.noZAxisSpeed = atoi(f[7]);
	s.limit = ZsHexFloat(f[8]);
	return true;
}
