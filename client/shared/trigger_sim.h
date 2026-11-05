#pragma once

#include <cmath>
#include <algorithm>
#include <cfloat>
#include <climits>
#include <unordered_map>
#include <utility>

static const int   PRED_MAX_EVENTS   = 512;
static const int   PRED_RING_SIZE     = 256;
static const int   PRED_SERVICE_CAP    = 1024;
static const float PRED_NEVER          = -1e9f;
static const int   PRED_SCRIPT_STEPS   = 40000;

static const int PRED_MOVETYPE_NONE   = 0;
static const int PRED_MOVETYPE_WALK   = 2;
static const int PRED_MOVETYPE_PUSH   = 7;
static const int PRED_MOVETYPE_NOCLIP = 8;

static const int FL_ONGROUND     = 1 << 0;
static const int FL_DUCKING      = 1 << 1;
// Client layout: one bit above the server's FL_BASEVELOCITY (1 << 23).
static const int FL_BASEVELOCITY_CLIENT = 1 << 24;
static const int FL_CLIENT_BIT   = 1 << 8;

enum PredClass {
	PC_NONE, PC_TELEPORT, PC_PUSH, PC_GRAVITY, PC_MULTIPLE, PC_ONCE,
	PC_RELAY, PC_COUNTER, PC_BRANCH, PC_CASE, PC_DOOR, PC_BUTTON
};

enum PredInput {
	IN_NONE, IN_ENABLE, IN_DISABLE, IN_TOGGLE, IN_KILL, IN_TRIGGER, IN_ENABLEREFIRE, IN_CANCELPENDING,
	IN_CNT_ADD, IN_CNT_SUB, IN_CNT_MUL, IN_CNT_DIV, IN_CNT_SETVALUE, IN_CNT_SETVALUENOFIRE,
	IN_CNT_SETHITMAX, IN_CNT_SETHITMIN, IN_CNT_GETVALUE,
	IN_BR_SETVALUE, IN_BR_SETVALUETEST, IN_BR_TOGGLE, IN_BR_TOGGLETEST, IN_BR_TEST,
	IN_CASE_INVALUE,
	IN_DOOR_OPEN, IN_DOOR_CLOSE, IN_DOOR_TOGGLE, IN_DOOR_LOCK, IN_DOOR_UNLOCK
};

enum PredOutput {
	OUT_ONSTARTTOUCH, OUT_ONSTARTTOUCHALL, OUT_ONENDTOUCH, OUT_ONENDTOUCHALL,
	OUT_ONTOUCHING, OUT_ONNOTTOUCHING, OUT_ONTRIGGER,
	OUT_OUTVALUE, OUT_ONGETVALUE, OUT_ONHITMIN, OUT_ONHITMAX,
	OUT_ONTRUE, OUT_ONFALSE, OUT_ONDEFAULT,
	OUT_ONOPEN, OUT_ONFULLYOPEN, OUT_ONCLOSE, OUT_ONFULLYCLOSED, OUT_ONDAMAGED,
	OUT_ONCASE0
};

// m_toggle_state
enum PredDoorState { PDS_TOP, PDS_BOTTOM, PDS_UP, PDS_DOWN };

enum PredPlayerKey { PPK_NONE, PPK_BASEVELOCITY, PPK_GRAVITY, PPK_TARGETNAME, PPK_CLASSNAME };

static const int PRED_TARGET_ACTIVATOR = -2;
static const int PRED_TARGET_SCRIPT    = -3;

struct PredAction {
	int   output;
	int   target;
	int   input = IN_NONE;
	float param = 0.0f;
	bool  hasParam = false;
	float delay;
	int   timesToFire;

	int         playerKey = PPK_NONE;
	float       playerVec[3] = {0,0,0};
	std::string playerName;

	int         vsScope = -1;
	int         vsFunc = -1;
};

enum PredFilterKind { FK_NONE, FK_NAME, FK_CLASS, FK_MULTI, FK_UNSUPPORTED };

struct PredFilter {
	PredFilterKind kind = FK_UNSUPPORTED;
	bool negated = false;
	std::string param;
	std::vector<int> subs;
	bool multiAnd = true;
};

struct PredSimEnt {
	PredClass cls = PC_NONE;
	int   entity = -1;
	int   hammerId = 0;
	int   modelIndex = -1;
	PredXform xf = {};
	int   spawnflags = 0;
	int   filterIndex = -1;
	int   actionStart = 0;
	int   actionCount = 0;

	float speed = 0.0f;
	float pushDir[3] = {0,0,0};
	float gravity = 1.0f;
	float wait = 0.0f;

	bool  destValid = false;
	float destOrigin[3] = {0,0,0};
	float destAngles[3] = {0,0,0};
	bool  snapAngles = false;
	bool  landmarkValid = false;
	float landmarkOrigin[3] = {0,0,0};

	bool  initialDisabled = false;
	float initialCounter = 0.0f;
	float counterMin = 0.0f;
	float counterMax = 0.0f;
	bool  initialBranch = false;

	int   caseCount = 0;
	float caseValue[16] = {0};

	float doorTravel = 0.0f; // seconds from one end to the other
	bool  doorStartOpen = false;
	bool  doorUse = false;   // "use opens", and the sim can place it for FindUseEntity

	bool  sjBooster = false; // the SourceJump landfix touches it from the unlifted position
};

struct PredEntState {
	uint8_t disabled = 0;
	uint8_t removed = 0;
	uint8_t spent = 0; // SetTouch(NULL) after firing, but still linked
	uint8_t linked = 0;
	uint8_t stale = 0;
	uint8_t inList = 0;
	uint8_t hitMin = 0, hitMax = 0, branchValue = 0, waitForRefire = 0;
	float   counterValue = 0.0f;
	float   multiWaitUntil = PRED_NEVER;
	int     multiReadyTick = INT_MIN;
	uint8_t doorState = PDS_BOTTOM; // a door's m_bLocked is `disabled`
	int     doorDoneTick = INT_MAX; // the command whose frame ends the door's move or its wait at the top
};

struct PredEvent {
	int   fireTick;
	float fireTime;
	int   target;
	int   input;
	float value;
	int   action;
	bool  playerQueue;
	bool  fromEndTouch;
	int   addedTick;
};

struct PredWorld {
	std::vector<PredEntState> ents;
	std::vector<int>          actionTimesLeft;
	std::vector<PredEvent>    events;
	std::string               playerName;
	std::string               playerClass = "player"; // maps rename it too
	// The server's FL_BASEVELOCITY, which is never networked.
	bool                      baseVelFlag = false;
	// The player's m_flGravity after the command. The game restores its own copy on a replay, so one
	// a state sync corrected (gravityFix) is set again at the start of the next command.
	float                     gravity = 1.0f;
	bool                      gravityFix = false;
	bool                      movedLast = false;
	bool                      teleportedLast = false;
	bool                      duckedAtEnd = false;
	int                       rngTeleportTick = INT_MIN / 2;
	bool                      rngTeleportSeq = false;
	int                       mpbhopsBlock = -1;
	float                     mpbhopsPunish = 0.0f;
	// The SourceJump landfix's lift of the last jump, until it ends; 0 for none.
	float                     landfixLift = 0.0f;
	std::vector<int>          linkedList;
	ZsState                   zones;
};

struct PredCheckpointState {
	std::vector<PredEvent>           events;
	std::vector<std::pair<int, int>> multiReady; // sim index, ticks from the command after the save
	std::string playerName;
	std::string playerClass;
	bool        baseVelFlag = false;
	int         mpbhopsBlock = -1;
	float       mpbhopsPunish = 0.0f;
};

static bool PredParseVec3(const char* s, float out[3])
{
	out[0] = out[1] = out[2] = 0.0f;
	return sscanf(s, "%f %f %f", &out[0], &out[1], &out[2]) >= 1;
}

static PredClass PredClassify(const char* cls)
{
	if (!strcmp(cls, "trigger_teleport")) return PC_TELEPORT;
	if (!strcmp(cls, "trigger_push"))     return PC_PUSH;
	if (!strcmp(cls, "trigger_gravity"))  return PC_GRAVITY;
	if (!strcmp(cls, "trigger_multiple")) return PC_MULTIPLE;
	if (!strcmp(cls, "trigger_once"))     return PC_ONCE;
	if (!strcmp(cls, "logic_relay"))      return PC_RELAY;
	if (!strcmp(cls, "math_counter"))     return PC_COUNTER;
	if (!strcmp(cls, "logic_branch"))     return PC_BRANCH;
	if (!strcmp(cls, "logic_case"))       return PC_CASE;
	if (!strcmp(cls, "func_door") || !strcmp(cls, "func_door_rotating")) return PC_DOOR;
	if (!strcmp(cls, "func_button"))      return PC_BUTTON;
	return PC_NONE;
}

static bool PredIsTriggerClass(PredClass c)
{
	return c == PC_TELEPORT || c == PC_PUSH || c == PC_GRAVITY || c == PC_MULTIPLE || c == PC_ONCE;
}

static int PredOutputId(const char* name)
{
	if (!strcasecmp(name, "OnStartTouch"))    return OUT_ONSTARTTOUCH;
	if (!strcasecmp(name, "OnStartTouchAll")) return OUT_ONSTARTTOUCHALL;
	if (!strcasecmp(name, "OnEndTouch"))      return OUT_ONENDTOUCH;
	if (!strcasecmp(name, "OnEndTouchAll"))   return OUT_ONENDTOUCHALL;
	if (!strcasecmp(name, "OnTouching"))      return OUT_ONTOUCHING;
	if (!strcasecmp(name, "OnNotTouching"))   return OUT_ONNOTTOUCHING;
	if (!strcasecmp(name, "OnTrigger"))       return OUT_ONTRIGGER;
	if (!strcasecmp(name, "OutValue"))        return OUT_OUTVALUE;
	if (!strcasecmp(name, "OnGetValue"))      return OUT_ONGETVALUE;
	if (!strcasecmp(name, "OnHitMin"))        return OUT_ONHITMIN;
	if (!strcasecmp(name, "OnHitMax"))        return OUT_ONHITMAX;
	if (!strcasecmp(name, "OnTrue"))          return OUT_ONTRUE;
	if (!strcasecmp(name, "OnFalse"))         return OUT_ONFALSE;
	if (!strcasecmp(name, "OnDefault"))       return OUT_ONDEFAULT;
	if (!strcasecmp(name, "OnOpen"))          return OUT_ONOPEN;
	if (!strcasecmp(name, "OnFullyOpen"))     return OUT_ONFULLYOPEN;
	if (!strcasecmp(name, "OnClose"))         return OUT_ONCLOSE;
	if (!strcasecmp(name, "OnFullyClosed"))   return OUT_ONFULLYCLOSED;
	if (!strcasecmp(name, "OnDamaged"))       return OUT_ONDAMAGED;
	if (!strncasecmp(name, "OnCase", 6)) {
		int n = atoi(name + 6);
		if (n >= 1 && n <= 16) return OUT_ONCASE0 + (n - 1);
	}
	return -1;
}

// A class's own input first: logic_branch's Toggle flips its value, not m_bDisabled.
static int PredInputId(PredClass targetCls, const char* name)
{
	switch (targetCls) {
	case PC_RELAY:
		if (!strcasecmp(name, "Trigger"))       return IN_TRIGGER;
		if (!strcasecmp(name, "EnableRefire"))  return IN_ENABLEREFIRE;
		if (!strcasecmp(name, "CancelPending")) return IN_CANCELPENDING;
		break;
	case PC_COUNTER:
		if (!strcasecmp(name, "Add"))            return IN_CNT_ADD;
		if (!strcasecmp(name, "Subtract"))       return IN_CNT_SUB;
		if (!strcasecmp(name, "Multiply"))       return IN_CNT_MUL;
		if (!strcasecmp(name, "Divide"))         return IN_CNT_DIV;
		if (!strcasecmp(name, "SetValue"))       return IN_CNT_SETVALUE;
		if (!strcasecmp(name, "SetValueNoFire")) return IN_CNT_SETVALUENOFIRE;
		if (!strcasecmp(name, "SetHitMax"))      return IN_CNT_SETHITMAX;
		if (!strcasecmp(name, "SetHitMin"))      return IN_CNT_SETHITMIN;
		if (!strcasecmp(name, "GetValue"))       return IN_CNT_GETVALUE;
		break;
	case PC_BRANCH:
		if (!strcasecmp(name, "SetValue"))     return IN_BR_SETVALUE;
		if (!strcasecmp(name, "SetValueTest")) return IN_BR_SETVALUETEST;
		if (!strcasecmp(name, "Toggle"))       return IN_BR_TOGGLE;
		if (!strcasecmp(name, "ToggleTest"))   return IN_BR_TOGGLETEST;
		if (!strcasecmp(name, "Test"))         return IN_BR_TEST;
		break;
	case PC_CASE:
		if (!strcasecmp(name, "InValue")) return IN_CASE_INVALUE;
		break;
	case PC_DOOR:
		if (!strcasecmp(name, "Open"))   return IN_DOOR_OPEN;
		if (!strcasecmp(name, "Close"))  return IN_DOOR_CLOSE;
		if (!strcasecmp(name, "Toggle")) return IN_DOOR_TOGGLE;
		if (!strcasecmp(name, "Lock"))   return IN_DOOR_LOCK;
		if (!strcasecmp(name, "Unlock")) return IN_DOOR_UNLOCK;
		// A door has no Enable or Disable.
		if (!strcasecmp(name, "Kill") || !strcasecmp(name, "KillHierarchy")) return IN_KILL;
		return IN_NONE;
	case PC_BUTTON:
		// Lock doesn't stop OnDamaged, so only a kill matters.
		if (!strcasecmp(name, "Kill") || !strcasecmp(name, "KillHierarchy")) return IN_KILL;
		return IN_NONE;
	default:
		break;
	}
	if (!strcasecmp(name, "Enable"))  return IN_ENABLE;
	if (!strcasecmp(name, "Disable")) return IN_DISABLE;
	if (!strcasecmp(name, "Toggle"))  return IN_TOGGLE;
	if (!strcasecmp(name, "Kill") || !strcasecmp(name, "KillHierarchy")) return IN_KILL;
	return IN_NONE;
}

// CEventAction: ESC-separated if there's an ESC anywhere, else comma-separated.
static void PredSplitOutput(const char* value, std::vector<std::string>& fields)
{
	fields.clear();
	const char sep = strchr(value, 0x1b) ? (char)0x1b : ',';
	std::string cur;
	for (const char* p = value; ; ++p) {
		if (*p == sep || *p == 0) {
			fields.push_back(cur);
			cur.clear();
			if (*p == 0) break;
		} else {
			cur.push_back(*p);
		}
	}
}

// CEventAction: a missing or zero count means every time (EVENT_FIRE_ALWAYS).
static int PredTimesToFire(const std::vector<std::string>& fields)
{
	const int n = fields.size() > 4 && !fields[4].empty() ? atoi(fields[4].c_str()) : -1;
	return n == 0 ? -1 : n;
}

static void PredAngleVectorsForward(const float ang[3], float fwd[3])
{
	float sp, cp, sy, cy;
	PredSinCos(ang[1], &sy, &cy);
	PredSinCos(ang[0], &sp, &cp);
	fwd[0] = cp * cy;
	fwd[1] = cp * sy;
	fwd[2] = -sp;
}

static void PredTriggerWorldBounds(const PredMap& map, const PredSimEnt& e, float pad, float mins[3], float maxs[3])
{
	const BspModel& m = map.models[e.modelIndex];
	for (int c = 0; c < 3; ++c) { mins[c] = 1e30f; maxs[c] = -1e30f; }
	for (int k = 0; k < 8; ++k) {
		const float l[3] = { (k & 1) ? m.maxs[0] + pad : m.mins[0] - pad, (k & 2) ? m.maxs[1] + pad : m.mins[1] - pad,
			(k & 4) ? m.maxs[2] + pad : m.mins[2] - pad };
		for (int c = 0; c < 3; ++c) {
			const float w = l[0] * e.xf.m[c][0] + l[1] * e.xf.m[c][1] + l[2] * e.xf.m[c][2] + e.xf.m[c][3];
			mins[c] = fminf(mins[c], w);
			maxs[c] = fmaxf(maxs[c], w);
		}
	}
}

static int PredApplySpawnFixup(PredClass cls, int sf)
{
	if (cls == PC_TELEPORT) return sf;
	if (sf & 0x810) sf |= 0x02;
	if (sf & 0x20)  sf |= 0x01;
	if (sf & 0x200) sf |= 0x01;
	return sf;
}

// m_bNegated is a FIELD_BOOLEAN keyfield: strtol != 0, so the fgd's "Disallow ..." text is false.
static bool PredFilterNegated(const char* v)
{
	return strtol(v, nullptr, 10) != 0;
}

static bool PredWildcardMatch(const char* pat, const char* s)
{
	size_t n = strlen(pat);
	if (n && pat[n - 1] == '*') return strncasecmp(pat, s, n - 1) == 0;
	return strcasecmp(pat, s) == 0;
}

static std::string PredLower(const char* s)
{
	std::string out(s);
	for (char& c : out)
		if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
	return out;
}

struct PredNameIndex {
	const PredMap* map = nullptr;
	std::unordered_map<std::string, std::vector<int>> exact;

	void Build(const PredMap& m)
	{
		map = &m;
		exact.clear();
		for (int i = 0; i < (int)m.entities.size(); ++i) {
			const char* n = m.entities[i].Get("targetname");
			if (n[0]) exact[PredLower(n)].push_back(i);
		}
	}

	void Find(const char* query, std::vector<int>& out) const
	{
		out.clear();
		if (!query || !query[0]) return;
		const size_t n = strlen(query);
		if (query[n - 1] != '*') {
			auto it = exact.find(PredLower(query));
			if (it != exact.end()) out = it->second;
			return;
		}
		for (int i = 0; i < (int)map->entities.size(); ++i) {
			const char* name = map->entities[i].Get("targetname");
			if (name[0] && PredWildcardMatch(query, name)) out.push_back(i);
		}
	}

	int First(const char* query) const
	{
		std::vector<int> hits;
		Find(query, hits);
		return hits.empty() ? -1 : hits[0];
	}
};

static void PredBuildFilters(PredMap& map, const PredNameIndex& names, std::vector<int>& filterEntity,
	std::vector<int>& entFilter)
{
	entFilter.assign(map.entities.size(), -1);
	for (int i = 0; i < (int)map.entities.size(); ++i) {
		const char* c = map.entities[i].Get("classname");
		if (strncmp(c, "filter_", 7) != 0) continue;
		const PredMapEntity& e = map.entities[i];
		PredFilter f;
		f.negated = PredFilterNegated(e.Get("Negated", "0"));
		if (!strcmp(c, "filter_activator_name"))       { f.kind = FK_NAME;  f.param = e.Get("filtername"); }
		else if (!strcmp(c, "filter_activator_class")) { f.kind = FK_CLASS; f.param = e.Get("filterclass"); }
		else if (!strcmp(c, "filter_multi"))           { f.kind = FK_MULTI; f.multiAnd = atoi(e.Get("FilterType", "0")) == 0; }
		else                                             f.kind = FK_UNSUPPORTED;
		entFilter[i] = (int)map.filters.size();
		filterEntity.push_back(i);
		map.filters.push_back(f);
	}

	for (int fi = 0; fi < (int)map.filters.size(); ++fi) {
		if (map.filters[fi].kind != FK_MULTI) continue;
		const PredMapEntity& e = map.entities[filterEntity[fi]];
		for (int k = 1; k <= 10; ++k) {
			char key[16];
			snprintf(key, sizeof(key), "Filter%02d", k);
			const char* name = e.Get(key);
			if (!name[0]) continue;
			const int ent = names.First(name);
			const int found = ent >= 0 ? entFilter[ent] : -1;
			if (found >= 0) map.filters[fi].subs.push_back(found);
			else            map.filters[fi].kind = FK_UNSUPPORTED;
		}
	}
}

// depth guards a filter_multi cycle, which would otherwise hang the game.
static bool PredFilterPasses(const PredMap& map, int fi, const char* playerName, const char* playerClass, int depth = 0)
{
	if (fi < 0 || fi >= (int)map.filters.size()) return true;
	if (depth > 16) return false;
	const PredFilter& f = map.filters[fi];
	bool impl;
	switch (f.kind) {
	case FK_NAME:  impl = PredWildcardMatch(f.param.c_str(), playerName); break;
	case FK_CLASS: impl = PredWildcardMatch(f.param.c_str(), playerClass); break;
	case FK_MULTI: {
		if (f.subs.empty()) { impl = true; break; }
		impl = f.multiAnd;
		for (int s : f.subs) {
			bool p = PredFilterPasses(map, s, playerName, playerClass, depth + 1);
			impl = f.multiAnd ? (impl && p) : (impl || p);
		}
		break; }
	default:
		return false;
	}
	return f.negated ? !impl : impl;
}

// The engine pads trigger bounds by a unit; binning adds one more.
static void PredBuildGrid(PredMap& map)
{
	PredGrid& g = map.grid;
	g = PredGrid();
	std::vector<int> trig;
	std::vector<float> box;
	float lo[2] = { 1e30f, 1e30f }, hi[2] = { -1e30f, -1e30f };
	for (int i = 0; i < (int)map.sim.size(); ++i) {
		const PredSimEnt& e = map.sim[i];
		if (!PredIsTriggerClass(e.cls) || e.modelIndex < 0 || (size_t)e.modelIndex >= map.models.size()) continue;
		float mins[3], maxs[3];
		PredTriggerWorldBounds(map, e, 1.0f, mins, maxs);
		bool finite = true;
		for (int c = 0; c < 2; ++c) {
			mins[c] -= 1.0f; maxs[c] += 1.0f;
			finite = finite && std::isfinite(mins[c]) && std::isfinite(maxs[c]) && fabsf(mins[c]) < 1e6f && fabsf(maxs[c]) < 1e6f;
		}
		if (!finite) { g.always.push_back(i); continue; }
		trig.push_back(i);
		box.insert(box.end(), { mins[0], mins[1], maxs[0], maxs[1] });
		lo[0] = fminf(lo[0], mins[0]); lo[1] = fminf(lo[1], mins[1]);
		hi[0] = fmaxf(hi[0], maxs[0]); hi[1] = fmaxf(hi[1], maxs[1]);
	}
	if (trig.empty()) { g.cellStart.assign(1, 0); return; }
	g.x0 = (int)floorf(lo[0] / g.cell);
	g.y0 = (int)floorf(lo[1] / g.cell);
	g.nx = (int)floorf(hi[0] / g.cell) - g.x0 + 1;
	g.ny = (int)floorf(hi[1] / g.cell) - g.y0 + 1;
	std::vector<std::vector<int>> cells((size_t)g.nx * g.ny);
	for (size_t t = 0; t < trig.size(); ++t) {
		const float* b = &box[t * 4];
		const int cx0 = (int)floorf(b[0] / g.cell) - g.x0, cx1 = (int)floorf(b[2] / g.cell) - g.x0;
		const int cy0 = (int)floorf(b[1] / g.cell) - g.y0, cy1 = (int)floorf(b[3] / g.cell) - g.y0;
		if ((long)(cx1 - cx0 + 1) * (cy1 - cy0 + 1) > 256) { g.always.push_back(trig[t]); continue; }
		for (int y = cy0; y <= cy1; ++y)
			for (int x = cx0; x <= cx1; ++x) cells[(size_t)y * g.nx + x].push_back(trig[t]);
	}
	g.cellStart.resize(cells.size() + 1);
	for (size_t c = 0; c < cells.size(); ++c) {
		g.cellStart[c] = (int)g.items.size();
		g.items.insert(g.items.end(), cells[c].begin(), cells[c].end());
	}
	g.cellStart[cells.size()] = (int)g.items.size();
	std::sort(g.always.begin(), g.always.end());
}

static bool PredIsScriptInput(const char* in)
{
	return !strcasecmp(in, "RunScriptCode") || !strcasecmp(in, "CallScriptFunction");
}

struct PredScriptIndex {
	std::vector<int> entScope; // -1 none, -2 a scope we can't know
	std::vector<std::unordered_map<std::string, int>> compiled; // -1 unsupported
};

// InputRunScript swaps backticks for quotes. nexttoken cuts a field after 255 chars, so
// longer code isn't what the game runs.
static bool PredScriptParam(const std::vector<std::string>& fields, std::string& param)
{
	param = fields.size() > 2 ? fields[2] : std::string();
	if (param.size() > 255) return false;
	if (!strcasecmp(fields[1].c_str(), "RunScriptCode"))
		for (char& c : param)
			if (c == '`') c = '"';
	return true;
}

static std::string PredScriptKey(const std::string& input, const std::string& param)
{
	return PredLower(input.c_str()) + '\x1f' + param;
}

static void PredScriptTargets(const PredMap& map, const PredNameIndex& names, int self, const std::string& target,
	std::vector<int>& out)
{
	out.clear();
	if (!strcasecmp(target.c_str(), "!self") || !strcasecmp(target.c_str(), "!caller")) { out.push_back(self); return; }
	if (target.empty() || target[0] == '!') return;
	names.Find(target.c_str(), out);
	if (!out.empty()) return;
	for (int i = 0; i < (int)map.entities.size(); ++i)
		if (PredWildcardMatch(target.c_str(), map.entities[i].Get("classname"))) out.push_back(i);
}

// Compiles every logic_script's files, then whatever code outputs send to a script scope,
// so that code which may redefine functions is known before any is trusted.
static void PredBuildScripts(PredMap& map, const PredNameIndex& names, PredScriptIndex& idx)
{
	map.vsPrograms.clear();
	idx.compiled.clear();
	idx.entScope.assign(map.entities.size(), -1);
	auto newProgram = [&](int ent) {
		map.vsPrograms.emplace_back();
		idx.compiled.emplace_back();
		idx.entScope[ent] = (int)map.vsPrograms.size() - 1;
		return idx.entScope[ent];
	};

	std::vector<std::string> files;
	for (int i = 0; i < (int)map.entities.size(); ++i) {
		const PredMapEntity& e = map.entities[i];
		const bool logicScript = !strcmp(e.Get("classname"), "logic_script");
		// Only logic_script's files are loaded; another entity's own scripts are unknown.
		if (!logicScript) { if (e.Get("vscripts")[0]) idx.entScope[i] = -2; continue; }
		vs::Program& prog = map.vsPrograms[newProgram(i)];
		PredSplitVScripts(e.Get("vscripts"), files);
		for (const std::string& name : files) {
			if (name.empty()) continue;
			const PredScriptFile* sf = nullptr;
			for (const PredScriptFile& f : map.scriptFiles)
				if (f.name == name) { sf = &f; break; }
			if (!sf)                                            prog.MakeDynamic("script not loaded");
			else if (sf->state == PredScriptFile::UNREADABLE) prog.MakeDynamic("unreadable script");
			else if (sf->state == PredScriptFile::LOADED)     vs::CompileFile(prog, sf->text.c_str(), sf->text.size());
		}
	}

	std::vector<std::string> fields;
	std::vector<int> targets;
	std::vector<uint8_t> broken(map.entities.size(), 0);
	std::string param;
	for (int i = 0; i < (int)map.entities.size(); ++i) {
		for (const PredKv& kv : map.entities[i].kv) {
			if (kv.value.find(',') == std::string::npos && kv.value.find('\x1b') == std::string::npos) continue;
			PredSplitOutput(kv.value.c_str(), fields);
			if (fields.size() < 2) continue;
			const char* in = fields[1].c_str();
			const bool script = PredIsScriptInput(in);
			// Inputs after which the scope's functions aren't what the files define.
			const bool breaks = !strcasecmp(in, "RunScriptFile") || !strcasecmp(in, "Kill")
				|| !strcasecmp(in, "KillHierarchy") || !strcasecmp(in, "TerminateScriptScope");
			if (!script && !breaks) continue;
			PredScriptTargets(map, names, i, fields[0], targets);
			for (int t : targets) {
				if (breaks) { broken[t] = 1; continue; }
				int pi = idx.entScope[t];
				if (pi == -2) continue;
				if (pi < 0) pi = newProgram(t); // an entity without scripts still gets a scope
				const bool fits = PredScriptParam(fields, param);
				const std::string key = PredScriptKey(fields[1], param);
				if (idx.compiled[pi].count(key)) continue;
				vs::Program& prog = map.vsPrograms[pi];
				int fn = -1;
				if (!fits) prog.MakeDynamic("code longer than an output field");
				else if (!strcasecmp(in, "RunScriptCode")) fn = vs::CompileSnippet(prog, param);
				else fn = vs::CompileCall(prog, param);
				idx.compiled[pi][key] = fn;
			}
		}
	}

	bool builtin = false;
	for (int i = 0; i < (int)map.entities.size(); ++i) {
		const int pi = idx.entScope[i];
		if (pi < 0) continue;
		if (broken[i]) map.vsPrograms[pi].MakeDynamic("scope killed or reloaded by an output");
		builtin = builtin || map.vsPrograms[pi].TaintsBuiltin();
	}
	for (vs::Program& prog : map.vsPrograms) {
		if (builtin) prog.MakeDynamic("a script redefines a builtin");
		vs::Link(prog);
	}

	for (const PredScriptFile& sf : map.scriptFiles) {
		if (sf.state == PredScriptFile::LOADED) map.stats.vsScripts++;
		else if (sf.state == PredScriptFile::UNREADABLE) map.stats.vsUnreadable++;
		else map.stats.vsMissing++;
	}
	for (int i = 0; i < (int)map.entities.size(); ++i) {
		const int pi = idx.entScope[i];
		if (pi < 0 || strcmp(map.entities[i].Get("classname"), "logic_script") != 0) continue;
		const vs::Program& prog = map.vsPrograms[pi];
		for (int fi = 0; fi < (int)prog.funcs.size(); ++fi) {
			const vs::Function& f = prog.funcs[fi];
			if (f.name.empty() || prog.Find(f.name.c_str()) != fi) continue;
			map.stats.vsFunctions++;
			if (f.unsupported) map.stats.vsUnsupported++;
		}
	}
}

static int PredScriptLookup(const PredMap& map, const PredScriptIndex& idx, int ent, const std::vector<std::string>& fields)
{
	const int pi = ent >= 0 && ent < (int)idx.entScope.size() ? idx.entScope[ent] : -1;
	if (pi < 0) return -1;
	std::string param;
	if (!PredScriptParam(fields, param)) return -1;
	const auto it = idx.compiled[pi].find(PredScriptKey(fields[1], param));
	if (it == idx.compiled[pi].end() || it->second < 0) return -1;
	const vs::Program& prog = map.vsPrograms[pi];
	if (prog.funcs[it->second].unsupported) return -1;
	// AcceptInput first calls Input<name> from the scope, which may swallow the input.
	if (prog.Find(("Input" + fields[1]).c_str()) >= 0) return -1;
	return it->second;
}

static void PredBuildMpbhops(PredMap& map);

// SourceJump's landfix.sp: a trigger_multiple output that keeps setting the activator's basevelocity.
static bool PredSjBoostOutput(const char* value)
{
	std::vector<std::string> fields;
	PredSplitOutput(value, fields);
	if (fields.size() < 3) return false;
	std::string param = fields[2];
	const char* ws = " \t\r\n";
	param.erase(0, param.find_first_not_of(ws));
	param.erase(param.find_last_not_of(ws) + 1);
	const int times = fields.size() > 4 ? atoi(fields[4].c_str()) : 0;
	return strncasecmp(param.c_str(), "basevelocity", 12) == 0 && strcasecmp(param.c_str(), "basevelocity 0 0 0") != 0 && times != 1;
}

// Its boosters: trigger_push without the once flag pushing level or down, and trigger_multiple with a
// repeatable basevelocity output, unparented and at most 4u thick. Thickness is m_vecMaxs - m_vecMins,
// and the engine spreads a brush model's bounds by a unit each side.
static bool PredSjBooster(const PredMapEntity& ent, PredClass cls, const BspModel& m)
{
	if (ent.Get("parentname")[0]) return false;
	if ((m.maxs[2] + 1.0f) - (m.mins[2] - 1.0f) > 4.0f + 0.01f) return false;
	if (cls == PC_PUSH) {
		if (atoi(ent.Get("spawnflags", "0")) & 0x80) return false;
		const float pitch = (float)atof(ent.Get("pushdir", "0 0 0"));
		return sinf(pitch * 3.14159265f / 180.0f) >= -0.01f;
	}
	for (const auto& kv : ent.kv)
		if ((!strcasecmp(kv.key.c_str(), "OnStartTouch") || !strcasecmp(kv.key.c_str(), "OnEndTouch")) && PredSjBoostOutput(kv.value.c_str()))
			return true;
	return false;
}

static void PredBuildTriggers(PredMap& map)
{
	std::vector<int> simEntity, entSim;
	std::vector<int> filterEntity, entFilter;
	map.sim.clear();
	map.actions.clear();
	map.filters.clear();
	PredNameIndex names;
	names.Build(map);
	PredBuildFilters(map, names, filterEntity, entFilter);
	PredScriptIndex scripts;
	PredBuildScripts(map, names, scripts);

	entSim.assign(map.entities.size(), -1);
	for (int i = 0; i < (int)map.entities.size(); ++i) {
		PredClass c = PredClassify(map.entities[i].Get("classname"));
		if (c == PC_NONE) continue;
		PredSimEnt e;
		e.cls = c;
		e.entity = i;
		e.hammerId = atoi(map.entities[i].Get("hammerid", "0"));
		entSim[i] = (int)map.sim.size();
		simEntity.push_back(i);
		map.sim.push_back(e);
	}

	std::vector<int> hits;
	auto findSimByName = [&](const char* name, std::vector<int>& out) {
		names.Find(name, hits);
		out.clear();
		for (int ent : hits)
			if (entSim[ent] >= 0) out.push_back(entSim[ent]);
	};
	// m_hFilter is the first entity with that name, if it is a filter.
	auto findFilterByName = [&](const char* name) -> int {
		const int ent = names.First(name);
		return ent >= 0 ? entFilter[ent] : -1;
	};
	auto findEntity = [&](const char* name) -> const PredMapEntity* {
		const int ent = names.First(name);
		return ent >= 0 ? &map.entities[ent] : nullptr;
	};
	// Triggers aren't networked, so one that rides a moving parent can't be
	// placed on the client. A parent that never moves is fine.
	auto movesWithParent = [&](const PredMapEntity& ent) -> bool {
		const char* parentName = ent.Get("parentname");
		if (!parentName[0]) return false;
		const PredMapEntity* parent = findEntity(parentName);
		if (!parent || parent->Get("parentname")[0]) return true;
		static const char* const fixed[] = {
			"info_target", "info_teleport_destination", "info_null", "func_brush", "func_wall",
			"func_illusionary", "env_beam", "env_sprite", "prop_static", "prop_dynamic",
		};
		for (const char* c : fixed)
			if (!strcmp(parent->Get("classname"), c)) return false;
		return true;
	};
	auto findPoint = [&](const char* name, float origin[3], float angles[3]) -> bool {
		const PredMapEntity* ent = findEntity(name);
		if (!ent || movesWithParent(*ent)) return false;
		PredParseVec3(ent->Get("origin"), origin);
		PredParseVec3(ent->Get("angles"), angles);
		return true;
	};

	std::vector<std::string> fields;
	std::vector<int> scriptTargets;
	for (int s = 0; s < (int)map.sim.size(); ++s) {
		PredSimEnt& e = map.sim[s];
		const PredMapEntity& ent = map.entities[simEntity[s]];

		e.spawnflags = PredApplySpawnFixup(e.cls, atoi(ent.Get("spawnflags", "0")));
		e.initialDisabled = atoi(ent.Get("StartDisabled", "0")) != 0;

		const char* model = ent.Get("model");
		e.modelIndex = model[0] == '*' ? atoi(model + 1) : -1;
		float org[3], ang[3];
		PredParseVec3(ent.Get("origin", "0 0 0"), org);
		PredParseVec3(ent.Get("angles", "0 0 0"), ang);
		PredAngleMatrix(ang, org, e.xf);
		if (PredIsTriggerClass(e.cls)) e.filterIndex = findFilterByName(ent.Get("filtername"));

		bool skip = false;
		switch (e.cls) {
		case PC_TELEPORT: {
			float o[3], a[3];
			e.destValid = findPoint(ent.Get("target"), o, a);
			if (e.destValid) { memcpy(e.destOrigin, o, sizeof o); memcpy(e.destAngles, a, sizeof a); }
			e.landmarkValid = findPoint(ent.Get("landmark"), o, a);
			if (e.landmarkValid) memcpy(e.landmarkOrigin, o, sizeof o);
			// Touch returns when the target names nothing, but the touch outputs still fire.
			if (!e.destValid && findEntity(ent.Get("target"))) skip = true;
			const PredMapEntity* lm = findEntity(ent.Get("landmark"));
			if (lm && movesWithParent(*lm)) skip = true;

			e.snapAngles = e.destValid && !e.landmarkValid && !(e.spawnflags & 0x20);
			break; }
		case PC_PUSH: {
			e.speed = (float)atof(ent.Get("speed", "0"));
			if (e.speed == 0.0f) e.speed = 100.0f;
			// Rotate pushdir into entity space and back like the server, so rounding matches.
			float dir[3], fwd[3], local[3];
			PredParseVec3(ent.Get("pushdir", "0 0 0"), dir);
			PredAngleVectorsForward(dir, fwd);
			for (int c = 0; c < 3; ++c) local[c] = fwd[0] * e.xf.m[0][c] + fwd[1] * e.xf.m[1][c] + fwd[2] * e.xf.m[2][c];
			for (int c = 0; c < 3; ++c) e.pushDir[c] = local[0] * e.xf.m[c][0] + local[1] * e.xf.m[c][1] + local[2] * e.xf.m[c][2];
			break; }
		case PC_GRAVITY:
			// 0 when missing, like the server.
			e.gravity = (float)atof(ent.Get("gravity", "0"));
			break;
		case PC_MULTIPLE:
			e.wait = (float)atof(ent.Get("wait", "0"));
			if (e.wait == 0.0f) e.wait = 0.2f;
			break;
		case PC_ONCE:
			e.wait = -1.0f;
			break;
		case PC_COUNTER:
			e.initialCounter = (float)atof(ent.Get("startvalue", "0"));
			e.counterMin = (float)atof(ent.Get("min", "0"));
			e.counterMax = (float)atof(ent.Get("max", "0"));
			break;
		case PC_BRANCH:
			e.initialBranch = atoi(ent.Get("InitialValue", "0")) != 0;
			break;
		case PC_CASE:
			for (int k = 0; k < 16; ++k) {
				char key[16];
				snprintf(key, sizeof(key), "Case%02d", k + 1);
				const char* v = ent.Get(key);
				if (v[0]) { e.caseValue[k] = (float)atof(v); e.caseCount = k + 1; }
			}
			break;
		case PC_DOOR: {
			// Only the timing of its outputs is modelled; the door itself is networked.
			e.speed = (float)atof(ent.Get("speed", "0"));
			if (e.speed == 0.0f) e.speed = 100.0f;
			e.wait = (float)atof(ent.Get("wait", "0"));
			e.initialDisabled = (e.spawnflags & 0x800) != 0; // starts locked
			const bool spawnOpen = atoi(ent.Get("spawnpos", "0")) == 1;
			float dist;
			if (!strcmp(ent.Get("classname"), "func_door_rotating")) {
				dist = fabsf((float)atof(ent.Get("distance", "0")));
				// The obsolete start-open flag swaps its angles and leaves it at the bottom.
				e.doorStartOpen = spawnOpen && !(e.spawnflags & 0x1);
			} else {
				if (e.modelIndex <= 0 || (size_t)e.modelIndex >= map.models.size()) { skip = true; break; }
				const BspModel& m = map.models[e.modelIndex];
				float moveAngles[3], dir[3];
				PredParseVec3(ent.Get("movedir", "0 0 0"), moveAngles);
				PredAngleVectorsForward(moveAngles, dir);
				// m_vecPosition2: the model's size along movedir, less the lip.
				dist = 0.0f;
				for (int c = 0; c < 3; ++c) dist += fabsf(dir[c] * (m.maxs[c] - m.mins[c]));
				dist = fabsf(dist - (float)atof(ent.Get("lip", "0")));
				e.doorStartOpen = spawnOpen || (e.spawnflags & 0x1);
			}
			e.doorTravel = dist / e.speed;
			// A master, a parent or starting open leaves it somewhere, or in a state, the sim can't follow.
			e.doorUse = (e.spawnflags & 0x100) && !e.doorStartOpen && !ent.Get("master", "")[0] && !movesWithParent(ent)
				&& e.modelIndex > 0 && (size_t)e.modelIndex < map.models.size();
			break; }
		case PC_BUTTON:
			// Only its OnDamaged, which our own shots fire (PredNoteShot). Without "damage activates" it takes
			// no damage, and on a parent the sim can't place it.
			if (!(atoi(ent.Get("spawnflags", "0")) & 0x200) || e.modelIndex <= 0 || (size_t)e.modelIndex >= map.models.size()
				|| movesWithParent(ent)) skip = true;
			break;
		default:
			break;
		}
		if (PredIsTriggerClass(e.cls) && (e.modelIndex < 0 || movesWithParent(ent))) skip = true;
		if (skip) { e.cls = PC_NONE; continue; }
		if (e.cls == PC_PUSH || e.cls == PC_MULTIPLE)
			e.sjBooster = e.modelIndex < (int)map.models.size() && PredSjBooster(ent, e.cls, map.models[e.modelIndex]);

		e.actionStart = (int)map.actions.size();
		for (const auto& kv : ent.kv) {
			int outId = PredOutputId(kv.key.c_str());
			if (outId < 0) continue;
			PredSplitOutput(kv.value.c_str(), fields);
			if (fields.size() < 2) continue;

			if (PredIsScriptInput(fields[1].c_str())) {
				PredScriptTargets(map, names, simEntity[s], fields[0], scriptTargets);
				if (scriptTargets.empty()) map.stats.vsSkipped++;
				for (int t : scriptTargets) {
					const int fn = PredScriptLookup(map, scripts, t, fields);
					if (fn < 0) { map.stats.vsSkipped++; continue; }
					PredAction a;
					a.output = outId;
					a.target = PRED_TARGET_SCRIPT;
					a.delay = fields.size() > 3 ? (float)atof(fields[3].c_str()) : 0.0f;
					a.timesToFire = PredTimesToFire(fields);
					a.vsScope = scripts.entScope[t];
					a.vsFunc = fn;
					map.actions.push_back(a);
					map.stats.vsPredicted++;
				}
				continue;
			}

			if (strcasecmp(fields[0].c_str(), "!activator") == 0) {
				if (strcasecmp(fields[1].c_str(), "AddOutput") != 0) continue;
				if (fields.size() < 3) continue;

				const std::string& kvs = fields[2];
				size_t sp = kvs.find(' ');
				std::string key = kvs.substr(0, sp);
				std::string val = sp == std::string::npos ? "" : kvs.substr(sp + 1);
				while (!val.empty() && val[0] == ' ') val.erase(val.begin());

				PredAction a;
				a.output = outId;
				a.target = PRED_TARGET_ACTIVATOR;
				a.delay = fields.size() > 3 ? (float)atof(fields[3].c_str()) : 0.0f;
				a.timesToFire = PredTimesToFire(fields);

				if (strcasecmp(key.c_str(), "basevelocity") == 0) {
					a.playerKey = PPK_BASEVELOCITY;
					PredParseVec3(val.c_str(), a.playerVec);
				} else if (strcasecmp(key.c_str(), "gravity") == 0) {
					a.playerKey = PPK_GRAVITY;
					a.playerVec[0] = (float)atof(val.c_str());
				} else if (strcasecmp(key.c_str(), "targetname") == 0) {
					a.playerKey = PPK_TARGETNAME;
					a.playerName = val;
				} else if (strcasecmp(key.c_str(), "classname") == 0) {
					a.playerKey = PPK_CLASSNAME;
					a.playerName = val;
				} else {
					continue;
				}

				map.actions.push_back(a);
				continue;
			}

			std::vector<int> targets;
			// !self and !caller are both the entity firing the output here.
			if (!strcasecmp(fields[0].c_str(), "!self") || !strcasecmp(fields[0].c_str(), "!caller"))
				targets.assign(1, s);
			else
				findSimByName(fields[0].c_str(), targets);
			for (int t : targets) {
				int inId = PredInputId(map.sim[t].cls, fields[1].c_str());
				if (inId == IN_NONE) continue;
				PredAction a;
				a.output = outId;
				a.target = t;
				a.input = inId;
				a.hasParam = fields.size() > 2 && !fields[2].empty();
				a.param = a.hasParam ? (float)atof(fields[2].c_str()) : 0.0f;
				a.delay = fields.size() > 3 ? (float)atof(fields[3].c_str()) : 0.0f;
				a.timesToFire = PredTimesToFire(fields);
				map.actions.push_back(a);
			}
		}
		e.actionCount = (int)map.actions.size() - e.actionStart;
	}

	map.simByHammerId.clear();
	map.doors.clear();
	map.buttons.clear();
	for (int s = 0; s < (int)map.sim.size(); ++s) {
		const PredSimEnt& e = map.sim[s];
		if (e.cls == PC_NONE) continue;
		map.stats.active++;
		if (e.cls == PC_DOOR) map.doors.push_back(s);
		if (e.cls == PC_BUTTON) map.buttons.push_back(s);
		if (!e.hammerId) continue;
		auto it = map.simByHammerId.emplace(e.hammerId, s);
		if (!it.second) it.first->second = -1;
	}
	PredBuildGrid(map);
	PredBuildMpbhops(map);
}

struct PredRuntime {
	PredMap* map = nullptr;
	PredWorld world;
	std::vector<PredWorld> ring;
	std::vector<int>       ringKey;
	int  prevKey = INT_MIN;
	int  prevTick = INT_MIN;
	bool initialized = false;

	bool  viewSnapPending = false;
	float viewSnapAngles[3] = {0,0,0};

	bool eventQueueFix = false;
	// SourceJump's eventqueue makes a func_button's attacker its activator before OnDamaged, so our own shots
	// on damage-activated buttons can be predicted.
	bool buttonShots = false;
	// Our first-run shots that hit one: the command and the button's sim index (PredNoteShot).
	static const int kShots = 8;
	int  shotKey[kShots];
	int  shotButton[kShots];
	int  shotNext = 0;

	bool  rngfixTelehop = true;
	int   teleports = 0;
	bool  telehopAir = false;
	bool  telehopBlocked = false;
	float telehopVelZ = 0.0f, telehopBaseVelZ = 0.0f, telehopGravity = 1.0f;

	// rngfix's g_iTick: the command's tick from its ProcessMovement pre hook on, one less
	// before it (events serviced at the start of the command).
	int   rngTick = 0;
	// rngfix_sim.h's PostThink, run after the touch pass and before timed events. When set
	// it does the telehop fix itself, so the partial one below is skipped.
	void (*postThinkHook)(void* user, PredRuntime& rt, PredPlayerIO& io) = nullptr;
	void* postThinkUser = nullptr;
	// FindUseEntity's trace (MASK_NPCSOLID_BRUSHONLY | MASK_OPAQUE_AND_NPCS), a box of half size
	// `half`. False when it can't trace; the use is then predicted as if nothing were in the way.
	bool (*useTrace)(const float start[3], const float end[3], float half, float* fraction) = nullptr;
	// The player's hull traced down by `lift` (MASK_PLAYERSOLID), where the SourceJump landfix touches
	// boosters from. False when it can't trace; the origin is then taken straight down.
	bool (*landfixUnlift)(const float origin[3], float lift, const float mins[3], const float maxs[3], float out[3]) = nullptr;
	// This command ends the lift once its touches are done.
	bool landfixEndsLift = false;

	// serviceBase: the command whose start services a zero-tick player event added now.
	PredPlayerIO* io = nullptr;
	int  serviceBase = 0;
	bool firingEndTouch = false;

	std::vector<int>      candidates;
	std::vector<uint32_t> stamp;
	vs::Scratch           vsScratch;
	int                   vsSteps = 0; // script work left this command, so a hostile map can't stall a frame
	uint32_t              stampNow = 0;

	bool cpTeleportPending = false;
	const PredCheckpointState* cpLoad = nullptr;
	bool cpKz = false;
	bool mpbhops = false;
	bool stateSync = false;
	int mpbhopsStartDoor = -1;

	ZsSet            zoneSet;
	ZsSettingsLog    zoneSettings;
	std::vector<int> simZone;
	int  zoneTeleports = 0;
	// The ground shavit-misc saw in this command's OnPlayerRunCmd, which runs before the sim's PreMove.
	int  zonePreGroundKey = INT_MIN;
	bool zonePreGround = false;
};

// Same rounding as eventqueuefix, epsilon included.
static int PredDelayTicks(float delay, float interval)
{
	return (int)ceilf((delay - 1.192092896e-07f) / interval);
}

static void PredEnqueue(PredRuntime& rt, float delay, bool playerActivator, int target, int input, float value, int action = -1)
{
	PredWorld& w = rt.world;
	if ((int)w.events.size() >= PRED_MAX_EVENTS) return;
	PredEvent ev;
	ev.target = target;
	ev.input = input;
	ev.value = value;
	ev.action = action;
	ev.playerQueue = playerActivator && rt.eventQueueFix;
	ev.fireTick = ev.playerQueue ? rt.serviceBase + PredDelayTicks(delay, rt.io->interval) : 0;
	ev.fireTime = ev.playerQueue ? 0.0f : rt.io->curtime + delay;
	ev.fromEndTouch = rt.firingEndTouch;
	ev.addedTick = PredEventClock(*rt.io);
	w.events.push_back(ev);
}

// AddEventAction prepends, so an output's actions go out in reverse keyvalue order.
static void PredFireOutput(PredRuntime& rt, int entIdx, int outId, float value, bool playerActivator = true)
{
	const PredMap& map = *rt.map;
	PredWorld& w = rt.world;
	const PredSimEnt& e = map.sim[entIdx];
	for (int k = e.actionCount - 1; k >= 0; --k) {
		int ai = e.actionStart + k;
		const PredAction& a = map.actions[ai];
		if (a.output != outId) continue;
		if (a.timesToFire != -1 && w.actionTimesLeft[ai] == 0) continue;
		float v = a.hasParam ? a.param : value;
		PredEnqueue(rt, a.delay, playerActivator, a.target, a.input, v, ai);
		if (a.timesToFire != -1 && w.actionTimesLeft[ai] > 0) w.actionTimesLeft[ai]--;
	}
}

// A damage-activated button whose bounds (a unit wider) take the point, -1 for none.
static int PredButtonAt(const PredRuntime& rt, const float p[3])
{
	if (!rt.map) return -1;
	for (int i : rt.map->buttons) {
		float mins[3], maxs[3];
		PredTriggerWorldBounds(*rt.map, rt.map->sim[i], 1.0f, mins, maxs);
		if (p[0] >= mins[0] && p[0] <= maxs[0] && p[1] >= mins[1] && p[1] <= maxs[1] && p[2] >= mins[2] && p[2] <= maxs[2])
			return i;
	}
	return -1;
}

// Our first-run shot on command `key` hit `button`. The server fires its OnDamaged during that command, and
// eventqueuefix runs the zero-delay outputs at the start of the next, so PredPreMove fires it there on every run.
static void PredNoteShot(PredRuntime& rt, int key, int button)
{
	for (int k = 0; k < PredRuntime::kShots; ++k)
		if (rt.shotKey[k] == key && rt.shotButton[k] == button) return;
	rt.shotKey[rt.shotNext] = key;
	rt.shotButton[rt.shotNext] = button;
	rt.shotNext = (rt.shotNext + 1) % PredRuntime::kShots;
}

static void PredFireShots(PredRuntime& rt, int key)
{
	for (int k = 0; k < PredRuntime::kShots; ++k) {
		const int b = rt.shotButton[k];
		if (rt.shotKey[k] != key - 1 || rt.world.ents[b].removed) continue;
		// As if added during the shot's command: due from the start of this one.
		rt.serviceBase = PredEventClock(*rt.io);
		PredFireOutput(rt, b, OUT_ONDAMAGED, 0.0f, true);
	}
}

static float PredMaxDelay(const PredMap& map, int entIdx, int outId)
{
	const PredSimEnt& e = map.sim[entIdx];
	float m = 0.0f;
	for (int k = 0; k < e.actionCount; ++k) {
		const PredAction& a = map.actions[e.actionStart + k];
		if (a.output == outId && a.delay > m) m = a.delay;
	}
	return m;
}

static void PredCounterUpdate(PredRuntime& rt, int entIdx, float value)
{
	const PredSimEnt& e = rt.map->sim[entIdx];
	PredEntState& st = rt.world.ents[entIdx];
	if (e.counterMin != 0.0f || e.counterMax != 0.0f) {
		if (value >= e.counterMax) { if (!st.hitMax) { st.hitMax = 1; PredFireOutput(rt, entIdx, OUT_ONHITMAX, value); } }
		else st.hitMax = 0;
		if (value <= e.counterMin) { if (!st.hitMin) { st.hitMin = 1; PredFireOutput(rt, entIdx, OUT_ONHITMIN, value); } }
		else st.hitMin = 0;
		value = fminf(fmaxf(value, e.counterMin), e.counterMax);
	}
	st.counterValue = value;
	PredFireOutput(rt, entIdx, OUT_OUTVALUE, value);
}

// The pusher moves min(time left, frametime) each frame, so it ends in the frame that uses up the rest.
static int PredDoorTicks(const PredRuntime& rt, float seconds)
{
	return std::max(1, (int)ceilf(seconds / rt.io->interval - 1e-4f));
}

// The door is the activator, so its outputs go on the server's queue. OnFullyClosed's is
// m_hActivator, last set by a +use the sim doesn't see.
static void PredDoorHit(PredRuntime& rt, int i)
{
	const PredSimEnt& e = rt.map->sim[i];
	PredEntState& st = rt.world.ents[i];
	const bool top = st.doorState == PDS_UP;
	st.doorState = top ? PDS_TOP : PDS_BOTTOM;
	st.doorDoneTick = INT_MAX;
	// SetMoveDoneTime(m_flWait), where a negative wait never returns.
	if (top && !(e.spawnflags & 0x20) && e.wait >= 0.0f) st.doorDoneTick = PredEventClock(*rt.io) + PredDoorTicks(rt, e.wait);
	// The obsolete start-open flag swaps the two.
	const bool open = top != ((e.spawnflags & 0x1) != 0);
	PredFireOutput(rt, i, open ? OUT_ONFULLYOPEN : OUT_ONFULLYCLOSED, 0.0f, false);
}

// DoorGoUp and DoorGoDown. LinearMove from where the door is, so turning back takes as long as
// it has moved, and a door already there arrives before OnOpen or OnClose fires.
static void PredDoorGo(PredRuntime& rt, int i, bool up)
{
	const PredSimEnt& e = rt.map->sim[i];
	PredEntState& st = rt.world.ents[i];
	const int now = PredEventClock(*rt.io);
	const int full = PredDoorTicks(rt, e.doorTravel);
	int ticks = full;
	if (st.doorState == PDS_UP || st.doorState == PDS_DOWN) {
		const int left = std::max(0, st.doorDoneTick - now);
		ticks = (st.doorState == PDS_UP) == up ? left : full - left;
	}
	st.doorState = up ? PDS_UP : PDS_DOWN;
	st.doorDoneTick = now + std::max(1, ticks);
	if (e.doorTravel <= 0.0f) PredDoorHit(rt, i);
	PredFireOutput(rt, i, up ? OUT_ONOPEN : OUT_ONCLOSE, 0.0f, false);
}

// PhysicsPusher, after the frame's commands and before its event queue.
static void PredRunDoors(PredRuntime& rt)
{
	const int now = PredEventClock(*rt.io);
	for (int i : rt.map->doors) {
		const PredEntState& st = rt.world.ents[i];
		if (st.removed || st.doorState == PDS_BOTTOM || st.doorDoneTick > now) continue;
		if (st.doorState == PDS_TOP) PredDoorGo(rt, i, false);
		else PredDoorHit(rt, i);
	}
}

// Slab test of a box of half size `half` swept from start along dir (unit) against [mins, maxs]:
// the distance where it enters, or -1.
static float PredSweepEnter(const float start[3], const float dir[3], float len, float half,
	const float mins[3], const float maxs[3])
{
	float t0 = 0.0f, t1 = len;
	for (int c = 0; c < 3; ++c) {
		const float lo = mins[c] - half, hi = maxs[c] + half;
		if (fabsf(dir[c]) < 1e-8f) {
			if (start[c] < lo || start[c] > hi) return -1.0f;
			continue;
		}
		float a = (lo - start[c]) / dir[c], b = (hi - start[c]) / dir[c];
		if (a > b) std::swap(a, b);
		t0 = std::max(t0, a);
		t1 = std::min(t1, b);
		if (t0 > t1) return -1.0f;
	}
	return t0;
}

// CalcDistanceToLine. FindUseEntity passes forward itself as the line's second point.
static float PredDistanceToLine(const float p[3], const float a[3], const float b[3])
{
	float d[3], q[3];
	for (int c = 0; c < 3; ++c) d[c] = b[c] - a[c];
	const float div = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
	const float t = div < 0.00001f ? 0.0f : ((d[0] * p[0] + d[1] * p[1] + d[2] * p[2]) - (d[0] * a[0] + d[1] * a[1] + d[2] * a[2])) / div;
	for (int c = 0; c < 3; ++c) q[c] = p[c] - (a[c] + d[c] * t);
	return sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]);
}

// CBasePlayer::FindUseEntity over the sim's doors that are at the bottom, where they spawned. -1 for none.
static int PredFindUseDoor(const PredRuntime& rt, const PredPlayerIO& io)
{
	const PredMap& map = *rt.map;
	const float* ang = io.viewAngles;
	float sp, cp, sy, cy, sr, cr;
	PredSinCos(ang[0], &sp, &cp);
	PredSinCos(ang[1], &sy, &cy);
	PredSinCos(ang[2], &sr, &cr);
	const float fwd[3] = { cp * cy, cp * sy, -sp };
	const float up[3] = { cr * sp * cy + sr * sy, cr * sp * sy - sr * cy, cr * cp };
	const float eye[3] = { io.origin[0], io.origin[1], io.origin[2] + io.viewHeight };
	const float feetZ = io.origin[2] + io.mins[2], headZ = io.origin[2] + io.maxs[2];

	// The model's bounds for the trace, and its collision box (a unit wider) for the rest.
	struct Cand { int i; float mins[3], maxs[3], box[2][3]; };
	std::vector<Cand> cands;
	for (int i : map.doors) {
		const PredSimEnt& e = map.sim[i];
		const PredEntState& st = rt.world.ents[i];
		if (!e.doorUse || st.removed || st.doorState != PDS_BOTTOM) continue;
		Cand c;
		c.i = i;
		PredTriggerWorldBounds(map, e, 0.0f, c.mins, c.maxs);
		PredTriggerWorldBounds(map, e, 1.0f, c.box[0], c.box[1]);
		cands.push_back(c);
	}
	if (cands.empty()) return -1;
	auto nearest = [&](const Cand& c, float p[3]) {
		for (int k = 0; k < 3; ++k) p[k] = std::min(std::max(eye[k], c.box[0][k]), c.box[1][k]);
	};

	static const float kTangents[8] = { 0, 1, 0.57735026919f, 0.3639702342f, 0.267949192431f, 0.1763269807f, -0.1763269807f, -0.267949192431f };
	int found = -1;
	for (int t = 0; t < 8; ++t) {
		float dir[3];
		memcpy(dir, fwd, sizeof dir);
		const float len = t == 0 ? 1024.0f : 72.0f, half = t == 0 ? 0.0f : 16.0f;
		if (t > 0) {
			for (int k = 0; k < 3; ++k) dir[k] = fwd[k] - kTangents[t] * up[k];
			const float n = sqrtf(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
			for (int k = 0; k < 3; ++k) dir[k] /= n;
		}
		int hit = -1;
		float hitAt = len;
		for (const Cand& c : cands) {
			const float at = PredSweepEnter(eye, dir, len, half, c.mins, c.maxs);
			if (at >= 0.0f && at < hitAt) { hit = c.i; hitAt = at; }
		}
		if (hit < 0) continue;
		// Anything else hit first (the world, or another entity) is what the trace found.
		const float end[3] = { eye[0] + dir[0] * len, eye[1] + dir[1] * len, eye[2] + dir[2] * len };
		float fraction;
		if (rt.useTrace && rt.useTrace(eye, end, half, &fraction) && fraction * len < hitAt - 0.1f) continue;
		const float endZ = eye[2] + dir[2] * hitAt;
		float delta[3] = { dir[0] * hitAt, dir[1] * hitAt, 0.0f };
		delta[2] = endZ < feetZ ? feetZ - endZ : (endZ > headZ ? endZ - headZ : 0.0f);
		if (sqrtf(delta[0] * delta[0] + delta[1] * delta[1] + delta[2] * delta[2]) >= 80.0f) continue;
		if (t == 0) return hit;
		found = hit;
	}

	float nearestDist = FLT_MAX;
	if (found >= 0) {
		for (const Cand& c : cands) {
			if (c.i != found) continue;
			float p[3];
			nearest(c, p);
			nearestDist = PredDistanceToLine(p, eye, fwd);
		}
	}
	for (const Cand& c : cands) {
		float p[3], d[3];
		nearest(c, p);
		for (int k = 0; k < 3; ++k) d[k] = p[k] - eye[k];
		const float n = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
		if (n >= 80.0f) continue;
		if (n > 0.0f && (d[0] * fwd[0] + d[1] * fwd[1] + d[2] * fwd[2]) / n < 0.8f) continue;
		const float dist = PredDistanceToLine(p, eye, fwd);
		if (dist >= nearestDist) continue;
		// Not behind glass or a grate. The door's own brush is a unit inside p.
		float fraction;
		if (rt.useTrace && rt.useTrace(eye, p, 0.0f, &fraction) && fraction < 1.0f) continue;
		found = c.i;
		nearestDist = dist;
	}
	return found;
}

// CBasePlayer::PlayerUse in PreThink, before the command's move: on the command +use goes down,
// CBaseDoor::Use opens a door at the bottom. The plugin resends the doors' state after such a
// command, so a use the server saw differently is put right a round trip later.
static void PredPlayerUse(PredRuntime& rt, const PredPlayerIO& io)
{
	const int IN_USE = 1 << 5;
	if (!io.solid || !(io.buttons & IN_USE) || (io.oldButtons & IN_USE)) return;
	const int i = PredFindUseDoor(rt, io);
	if (i < 0 || rt.world.ents[i].disabled) return;
	PredDoorGo(rt, i, true);
}

// Removing an entity drops its touch links without EndTouch, like the engine.
static void PredSetLinked(PredWorld& w, int entIdx, bool on)
{
	PredEntState& st = w.ents[entIdx];
	if (st.linked == on) return;
	st.linked = on;
	if (on) { w.linkedList.push_back(entIdx); return; }
	for (size_t k = 0; k < w.linkedList.size(); ++k)
		if (w.linkedList[k] == entIdx) { w.linkedList.erase(w.linkedList.begin() + k); break; }
}

static void PredRemove(PredRuntime& rt, int entIdx)
{
	PredEntState& st = rt.world.ents[entIdx];
	if (entIdx < (int)rt.simZone.size() && rt.simZone[entIdx] >= 0) rt.world.zones.in[rt.simZone[entIdx]] = 0;
	st.removed = 1;
	PredSetLinked(rt.world, entIdx, false);
	st.inList = 0;
}

static void PredTouchNow(PredRuntime& rt, int entIdx);

static void PredAcceptInput(PredRuntime& rt, int entIdx, int input, float value)
{
	const PredSimEnt& e = rt.map->sim[entIdx];
	PredEntState& st = rt.world.ents[entIdx];
	if (st.removed) return;
	switch (input) {
	case IN_ENABLE:
		if (st.disabled) { st.disabled = 0; if (PredIsTriggerClass(e.cls)) PredTouchNow(rt, entIdx); }
		break;
	case IN_DISABLE: st.disabled = 1; break;
	case IN_TOGGLE:
		st.disabled ^= 1;
		if (!st.disabled && PredIsTriggerClass(e.cls)) PredTouchNow(rt, entIdx);
		break;
	case IN_KILL: PredRemove(rt, entIdx); break;

	case IN_TRIGGER:
		if (st.disabled || st.waitForRefire) break;
		PredFireOutput(rt, entIdx, OUT_ONTRIGGER, value);
		if (e.spawnflags & 0x001) PredRemove(rt, entIdx);
		else if (!(e.spawnflags & 0x002)) {
			st.waitForRefire = 1;
			// The relay is its own activator, so this stays on the server's queue.
			PredEnqueue(rt, PredMaxDelay(*rt.map, entIdx, OUT_ONTRIGGER) + 0.001f, false, entIdx, IN_ENABLEREFIRE, 0.0f);
		}
		break;
	case IN_ENABLEREFIRE:
	case IN_CANCELPENDING: st.waitForRefire = 0; break;

	case IN_CNT_ADD:            if (!st.disabled) PredCounterUpdate(rt, entIdx, st.counterValue + value); break;
	case IN_CNT_SUB:            if (!st.disabled) PredCounterUpdate(rt, entIdx, st.counterValue - value); break;
	case IN_CNT_MUL:            if (!st.disabled) PredCounterUpdate(rt, entIdx, st.counterValue * value); break;
	// Dividing by zero still fires OutValue, with the value unchanged.
	case IN_CNT_DIV:            if (!st.disabled) PredCounterUpdate(rt, entIdx, value != 0.0f ? st.counterValue / value : st.counterValue); break;
	case IN_CNT_SETVALUE:       if (!st.disabled) PredCounterUpdate(rt, entIdx, value); break;
	case IN_CNT_SETVALUENOFIRE: st.counterValue = fminf(fmaxf(value, e.counterMin), e.counterMax); break;
	case IN_CNT_GETVALUE:       PredFireOutput(rt, entIdx, OUT_ONGETVALUE, st.counterValue); break;

	case IN_BR_SETVALUE:     st.branchValue = value != 0.0f; break;
	case IN_BR_SETVALUETEST: st.branchValue = value != 0.0f; PredFireOutput(rt, entIdx, st.branchValue ? OUT_ONTRUE : OUT_ONFALSE, value); break;
	case IN_BR_TOGGLE:       st.branchValue ^= 1; break;
	case IN_BR_TOGGLETEST:   st.branchValue ^= 1; PredFireOutput(rt, entIdx, st.branchValue ? OUT_ONTRUE : OUT_ONFALSE, value); break;
	case IN_BR_TEST:         PredFireOutput(rt, entIdx, st.branchValue ? OUT_ONTRUE : OUT_ONFALSE, value); break;

	case IN_CASE_INVALUE: {
		for (int k = 0; k < e.caseCount; ++k)
			if (e.caseValue[k] == value) { PredFireOutput(rt, entIdx, OUT_ONCASE0 + k, value); return; }
		PredFireOutput(rt, entIdx, OUT_ONDEFAULT, value);
		break; }

	case IN_DOOR_OPEN:
		if (st.doorState != PDS_TOP && st.doorState != PDS_UP && !st.disabled) PredDoorGo(rt, entIdx, true);
		break;
	case IN_DOOR_CLOSE:
		if (st.doorState != PDS_BOTTOM) PredDoorGo(rt, entIdx, false);
		break;
	case IN_DOOR_TOGGLE:
		if (st.disabled) break;
		if (st.doorState == PDS_BOTTOM) PredDoorGo(rt, entIdx, true);
		else if (st.doorState == PDS_TOP) PredDoorGo(rt, entIdx, false);
		break;
	case IN_DOOR_LOCK:   st.disabled = 1; break;
	case IN_DOOR_UNLOCK: st.disabled = 0; break;
	default:
		break;
	}
}

// Triggers are tested with the hull the command started with (PostThink sets it).
static void PredSetHull(PredPlayerIO& io, bool duckedAtStart)
{
	io.mins[0] = -16.0f; io.mins[1] = -16.0f; io.mins[2] = 0.0f;
	io.maxs[0] =  16.0f; io.maxs[1] =  16.0f; io.maxs[2] = duckedAtStart ? 45.0f : 62.0f;
}

static void PredApplyToPlayer(PredRuntime& rt, const PredAction& a)
{
	PredPlayerIO& io = *rt.io;
	switch (a.playerKey) {
	case PPK_BASEVELOCITY:
		io.baseVelocity[0] = a.playerVec[0];
		io.baseVelocity[1] = a.playerVec[1];
		io.baseVelocity[2] = a.playerVec[2];
		break;
	case PPK_GRAVITY:
		*io.gravity = a.playerVec[0];
		break;
	case PPK_TARGETNAME:
		rt.world.playerName = a.playerName;
		break;
	case PPK_CLASSNAME:
		rt.world.playerClass = a.playerName;
		break;
	default:
		break;
	}
}

// Runs on a copy of the player; only a run we can model is kept. GetAbsVelocity and SetAbsVelocity
// are the CMoveData velocity here (base velocity is kept apart from it outside the move).
static void PredRunScript(PredRuntime& rt, const PredAction& a)
{
	const PredMap& map = *rt.map;
	if (a.vsScope < 0 || a.vsScope >= (int)map.vsPrograms.size() || rt.vsSteps <= 0) return;
	PredPlayerIO& io = *rt.io;
	vs::Player pl;
	for (int c = 0; c < 3; ++c) {
		pl.vel[c] = io.velocity[c];
		pl.origin[c] = io.origin[c];
		pl.baseVel[c] = io.baseVelocity[c];
	}
	pl.gravity = *io.gravity;
	int used = 0;
	const int r = vs::Run(map.vsPrograms[a.vsScope], a.vsFunc, pl, rt.vsScratch, rt.vsSteps, &used);
	rt.vsSteps -= used;
	if (r == vs::R_UNMODELED) return;
	// A new origin is picked up as a game-code move by PredPreMove / PredPostMove.
	for (int c = 0; c < 3; ++c) {
		io.velocity[c] = pl.vel[c];
		io.origin[c] = pl.origin[c];
	}
	*io.gravity = pl.gravity;
}

// What removal does to a touch link depends on when it happens.
static void PredKill(PredRuntime& rt, const PredEvent& ev)
{
	PredEntState& st = rt.world.ents[ev.target];
	if (st.removed) return;
	if (st.linked && (ev.playerQueue || st.stale)) { st.removed = 1; return; }
	PredRemove(rt, ev.target);
}

static void PredDispatch(PredRuntime& rt, const PredEvent& ev)
{
	if (ev.target == PRED_TARGET_SCRIPT) {
		if (ev.action >= 0 && ev.action < (int)rt.map->actions.size())
			PredRunScript(rt, rt.map->actions[ev.action]);
		return;
	}
	if (ev.target == PRED_TARGET_ACTIVATOR) {
		if (ev.action >= 0 && ev.action < (int)rt.map->actions.size())
			PredApplyToPlayer(rt, rt.map->actions[ev.action]);
		return;
	}
	if (ev.input == IN_KILL) PredKill(rt, ev);
	else PredAcceptInput(rt, ev.target, ev.input, ev.value);
}

// eventqueuefix: a zero-tick event added while servicing fires in the same pass.
static void PredServicePlayerEvents(PredRuntime& rt)
{
	PredWorld& w = rt.world;
	const int tick = PredEventClock(*rt.io);
	rt.serviceBase = tick;
	int guard = 0;
	for (size_t i = 0; i < w.events.size() && guard < PRED_SERVICE_CAP; ) {
		if (!w.events[i].playerQueue || w.events[i].fireTick > tick) { ++i; continue; }
		const PredEvent ev = w.events[i];
		w.events.erase(w.events.begin() + i);
		guard++;
		PredDispatch(rt, ev);
	}
}

// CEventQueue order: earliest first, ties in the order added.
static void PredServiceTimedEvents(PredRuntime& rt)
{
	PredWorld& w = rt.world;
	const float curtime = rt.io->curtime;
	for (int guard = 0; guard < PRED_SERVICE_CAP; ++guard) {
		int best = -1;
		for (int i = 0; i < (int)w.events.size(); ++i) {
			const PredEvent& ev = w.events[i];
			if (ev.playerQueue || ev.fireTime > curtime) continue;
			if (best < 0 || ev.fireTime < w.events[best].fireTime) best = i;
		}
		if (best < 0) return;
		const PredEvent ev = w.events[best];
		w.events.erase(w.events.begin() + best);
		PredDispatch(rt, ev);
	}
}

// CBaseEntity::SetGroundEntity(NULL). SetGroundEntity returns early for the handle the player
// already has, so with only FL_ONGROUND cleared the move's CategorizePosition finds the same
// ground and leaves the flag clear: the player walks without being on the ground.
static void PredClearGround(PredPlayerIO& io)
{
	*io.flags &= ~FL_ONGROUND;
	if (io.groundEntity) *io.groundEntity = 0xFFFFFFFFu;
}

static void PredEffectTeleport(const PredSimEnt& e, PredPlayerIO& io)
{
	if (!e.destValid) return;
	if (e.landmarkValid) {
		for (int c = 0; c < 3; ++c) io.origin[c] = e.destOrigin[c] + (io.origin[c] - e.landmarkOrigin[c]);
	} else {
		io.origin[0] = e.destOrigin[0];
		io.origin[1] = e.destOrigin[1];
		io.origin[2] = e.destOrigin[2] - io.mins[2];
	}
	PredClearGround(io);
}

// ApplyAbsVelocityImpulse, with CheckEntityVelocity's clamp.
static void PredApplyImpulse(PredPlayerIO& io, const float imp[3])
{
	if (imp[0] == 0.0f && imp[1] == 0.0f && imp[2] == 0.0f) return;
	const float r = 4000.0f;
	float v[3] = { imp[0], imp[1], imp[2] };
	if (!(v[0] > -r && v[0] < r && v[1] > -r && v[1] < r && v[2] > -r && v[2] < r)) {
		const float speed = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
		if (!(speed < r * 100.0f)) return;
		for (int c = 0; c < 3; ++c) v[c] *= r / speed;
	}
	for (int c = 0; c < 3; ++c) io.velocity[c] += v[c];
}

static void PredEffectPush(PredRuntime& rt, int entIdx, const PredSimEnt& e)
{
	PredPlayerIO& io = *rt.io;
	PredWorld& w = rt.world;
	float push[3] = { e.speed * e.pushDir[0], e.speed * e.pushDir[1], e.speed * e.pushDir[2] };

	if (io.moveType == PRED_MOVETYPE_NONE || io.moveType == PRED_MOVETYPE_PUSH) return;
	if (e.spawnflags & 0x80) {
		PredApplyImpulse(io, push);
		if (e.pushDir[2] > 0.0f) PredClearGround(io);
		PredRemove(rt, entIdx);
		return;
	}

	if (io.moveType == PRED_MOVETYPE_NOCLIP) return;
	if (w.baseVelFlag)
		for (int c = 0; c < 3; ++c) push[c] += io.baseVelocity[c];

	if (push[2] > 0.0f && (*io.flags & FL_ONGROUND)) {
		PredClearGround(io);
		io.origin[2] += 1.0f;
	}
	for (int c = 0; c < 3; ++c) io.baseVelocity[c] = push[c];
	w.baseVelFlag = true;
	*io.flags |= FL_BASEVELOCITY_CLIENT;
}

// CPlayerMove::CheckMovingGround. The client runs its own copy keyed on its
// FL_BASEVELOCITY, so a push sets that too or the client converts it every tick.
static void PredCheckMovingGround(PredWorld& w, PredPlayerIO& io)
{
	if (!w.baseVelFlag) {
		const float k = (float)(1.0 + (double)io.interval * 0.5);
		const float imp[3] = { k * io.baseVelocity[0], k * io.baseVelocity[1], k * io.baseVelocity[2] };
		PredApplyImpulse(io, imp);
		io.baseVelocity[0] = io.baseVelocity[1] = io.baseVelocity[2] = 0.0f;
	}
	w.baseVelFlag = false;
	*io.flags &= ~FL_BASEVELOCITY_CLIENT;
}

static void PredEffectMulti(PredRuntime& rt, int entIdx, const PredSimEnt& e)
{
	const PredPlayerIO& io = *rt.io;
	PredEntState& st = rt.world.ents[entIdx];
	if (st.spent) return;
	if (rt.eventQueueFix ? PredEventClock(io) < st.multiReadyTick : io.curtime < st.multiWaitUntil) return;
	PredFireOutput(rt, entIdx, OUT_ONTRIGGER, 0.0f);
	if (e.wait > 0.0f) {
		if (rt.eventQueueFix) {
			const int k = PredDelayTicks(e.wait, io.interval) - 1;
			st.multiReadyTick = PredEventClock(io) + (k > 1 ? k : 1);
		} else {
			st.multiWaitUntil = io.curtime + e.wait;
		}
	} else {
		// SetTouch(NULL) now, SUB_Remove 0.1 s later on the server's clock.
		st.spent = 1;
		PredEnqueue(rt, 0.1f, false, entIdx, IN_KILL, 0.0f);
	}
}

static bool PredPassesFilters(const PredMap& map, const PredWorld& w, const PredSimEnt& e, PredPlayerIO& io)
{
	const int sf = e.spawnflags;
	const bool client = (*io.flags & FL_CLIENT_BIT) != 0;
	if (!((sf & 0x40) || ((sf & 0x01) && client)) || (sf & 0x20)) return false;
	return PredFilterPasses(map, e.filterIndex, w.playerName.c_str(), w.playerClass.c_str());
}

// Filters are checked once, when the link is made, and only a player that
// passed them gets OnEndTouch.
static void PredStartTouch(PredRuntime& rt, int i)
{
	PredEntState& st = rt.world.ents[i];
	if (!PredPassesFilters(*rt.map, rt.world, rt.map->sim[i], *rt.io)) return;
	const bool added = !st.inList;
	st.inList = 1;
	PredFireOutput(rt, i, OUT_ONSTARTTOUCH, 0.0f);
	if (added) PredFireOutput(rt, i, OUT_ONSTARTTOUCHALL, 0.0f);
}

static void PredEndTouch(PredRuntime& rt, int i)
{
	PredEntState& st = rt.world.ents[i];
	PredSetLinked(rt.world, i, false);
	if (i < (int)rt.simZone.size() && rt.simZone[i] >= 0) rt.world.zones.in[rt.simZone[i]] = 0;
	if (!st.inList) return;
	st.inList = 0;
	rt.firingEndTouch = true;
	PredFireOutput(rt, i, OUT_ONENDTOUCH, 0.0f);
	PredFireOutput(rt, i, OUT_ONENDTOUCHALL, 0.0f);
	rt.firingEndTouch = false;
}


static bool PredZonesOn(const PredRuntime& rt)
{
	return rt.zoneSet.active && rt.zoneSettings.known;
}

static const ZsSettings& PredZoneSettings(const PredRuntime& rt, const PredPlayerIO& io)
{
	return rt.zoneSettings.For(io.command);
}

static void PredRemoveTouchedList(PredRuntime& rt)
{
	PredWorld& w = rt.world;
	while (!w.linkedList.empty()) PredEndTouch(rt, w.linkedList.back());
	w.zones.ClearTouches();
}

static void PredZoneTeleport(PredRuntime& rt, int k)
{
	PredPlayerIO& io = *rt.io;
	const ZsZone& z = rt.zoneSet.zones[k];
	for (int c = 0; c < 3; ++c) io.origin[c] = z.dest[c];
	PredClearGround(io);
	// Same touch stamp bump as a trigger_teleport's.
	PredWorld& w = rt.world;
	for (int i : w.linkedList) w.ents[i].stale = i != z.sim;
	for (int j = 0; j < ZS_MAX; ++j) w.zones.stale[j] = w.zones.linked[j] && j != k;
	w.zones.teleportKey = PredSnapshotKey(io);
	++rt.zoneTeleports;
}

static void PredZoneStartTouch(PredRuntime& rt, int k)
{
	const ZsZone& z = rt.zoneSet.zones[k];
	const ZsSettings& s = PredZoneSettings(rt, *rt.io);
	if (!ZsStartTouchApplies(z, s)) return;
	if (z.type == ZS_TELEPORT) PredZoneTeleport(rt, k);
	ZsEnter(z, s, rt.world.zones, k);
}

static void PredZoneTouchPost(PredRuntime& rt, int k)
{
	const ZsZone& z = rt.zoneSet.zones[k];
	PredPlayerIO& io = *rt.io;
	const ZsSettings& s = PredZoneSettings(rt, io);
	if (!ZsStartTouchApplies(z, s)) return;
	if (z.type == ZS_TELEPORT) {
		PredZoneTeleport(rt, k);
		return;
	}
	if (z.type != ZS_START) return;
	const int since = PredSnapshotKey(io) - rt.world.zones.teleportKey;
	if (rt.eventQueueFix && since >= 0 && since <= 4) {
		PredRemoveTouchedList(rt);
		return;
	}
	if (ZsStartTimerClamp(z, s, rt.world.zones, io.solid, io.moveType == PRED_MOVETYPE_NOCLIP, io.velocity))
		io.baseVelocity[0] = io.baseVelocity[1] = io.baseVelocity[2] = 0.0f;
}

static void PredHookedZone(PredRuntime& rt, int i, bool start)
{
	if (i >= (int)rt.simZone.size() || rt.simZone[i] < 0 || !PredZonesOn(rt)) return;
	if (!PredPassesFilters(*rt.map, rt.world, rt.map->sim[i], *rt.io)) return;
	if (start) PredZoneStartTouch(rt, rt.simZone[i]);
	else PredZoneTouchPost(rt, rt.simZone[i]);
}

static void PredZonesTouch(PredRuntime& rt, const float feet[3])
{
	if (!PredZonesOn(rt)) return;
	const PredPlayerIO& io = *rt.io;
	const std::vector<ZsZone>& zones = rt.zoneSet.zones;
	ZsState& st = rt.world.zones;
	for (int k = 0; k < (int)zones.size() && k < ZS_MAX; ++k) {
		if (zones[k].form != ZS_FORM_BOX) continue;
		if (!ZsBoxTouches(zones[k], feet, io.mins, io.maxs)) {
			if (st.linked[k]) st.stale[k] = 1;
			continue;
		}
		st.stale[k] = 0;
		if (!st.linked[k]) {
			st.linked[k] = 1;
			PredZoneStartTouch(rt, k);
		}
		PredZoneTouchPost(rt, k);
	}
}

static void PredTouch(PredRuntime& rt, int i)
{
	const PredMap& map = *rt.map;
	const PredSimEnt& e = map.sim[i];
	PredPlayerIO& io = *rt.io;
	if (e.cls == PC_GRAVITY) { *io.gravity = e.gravity; return; } // GravityTouch has no filter
	if (!PredPassesFilters(map, rt.world, e, io)) return;
	switch (e.cls) {
	case PC_TELEPORT:
		if (e.destValid) {
			++rt.teleports;
			if (rt.world.rngTeleportTick == rt.rngTick - 1) rt.world.rngTeleportSeq = true;
			rt.world.rngTeleportTick = rt.rngTick;
			rt.world.zones.teleportKey = PredSnapshotKey(io);
			// Teleport's SetSolidFlags(FSOLID_NOT_SOLID) bumps the player's touchStamp, so every
			// link stamped so far goes stale. The teleport's own is stamped after its Touch.
			for (int k : rt.world.linkedList) rt.world.ents[k].stale = k != i;
		}
		PredEffectTeleport(e, io);
		if (e.snapAngles) {
			rt.viewSnapPending = true;
			memcpy(rt.viewSnapAngles, e.destAngles, sizeof rt.viewSnapAngles);
		}
		break;
	case PC_PUSH:     PredEffectPush(rt, i, e); break;
	case PC_MULTIPLE:
	case PC_ONCE:     PredEffectMulti(rt, i, e); break;
	default: break;
	}
}

static void PredLinkAndTouch(PredRuntime& rt, int i)
{
	PredEntState& st = rt.world.ents[i];
	st.stale = 0;
	if (!st.linked) {
		PredSetLinked(rt.world, i, true);
		PredStartTouch(rt, i);
		PredHookedZone(rt, i, true);
		if (st.removed) return;
	}
	PredTouch(rt, i);
	PredHookedZone(rt, i, false);
}

static void PredTouchNow(PredRuntime& rt, int i)
{
	const PredSimEnt& e = rt.map->sim[i];
	const PredPlayerIO& io = *rt.io;
	if (PredBoxInModel(*rt.map, e.modelIndex, e.xf, io.origin, io.mins, io.maxs))
		PredLinkAndTouch(rt, i);
}

static void PredGatherCandidates(PredRuntime& rt, const float feet[3], const float mins[3], const float maxs[3])
{
	const PredGrid& g = rt.map->grid;
	std::vector<int>& out = rt.candidates;
	out.clear();
	if (++rt.stampNow == 0) { std::fill(rt.stamp.begin(), rt.stamp.end(), 0u); rt.stampNow = 1; }
	auto add = [&](int i) { if (rt.stamp[i] != rt.stampNow) { rt.stamp[i] = rt.stampNow; out.push_back(i); } };
	if (g.nx > 0) {
		const float x0 = feet[0] + mins[0] - 1.0f, x1 = feet[0] + maxs[0] + 1.0f;
		const float y0 = feet[1] + mins[1] - 1.0f, y1 = feet[1] + maxs[1] + 1.0f;
		if (std::isfinite(x0) && std::isfinite(x1) && std::isfinite(y0) && std::isfinite(y1)) {
			const float fx0 = floorf(x0 / g.cell) - g.x0, fx1 = floorf(x1 / g.cell) - g.x0;
			const float fy0 = floorf(y0 / g.cell) - g.y0, fy1 = floorf(y1 / g.cell) - g.y0;
			const int cx0 = (int)fmaxf(fx0, 0.0f), cx1 = (int)fminf(fx1, (float)(g.nx - 1));
			const int cy0 = (int)fmaxf(fy0, 0.0f), cy1 = (int)fminf(fy1, (float)(g.ny - 1));
			for (int y = cy0; y <= cy1; ++y)
				for (int x = cx0; x <= cx1; ++x) {
					const size_t c = (size_t)y * g.nx + x;
					for (int k = g.cellStart[c]; k < g.cellStart[c + 1]; ++k) add(g.items[k]);
				}
		}
	}
	for (int i : g.always) add(i);
	for (int i : rt.world.linkedList) add(i);
	std::sort(out.begin(), out.end());
}

// Where landfix.sp touches boosters from while a lift lasts: the player's hull traced down by the lift.
static void PredSjUnlifted(const PredRuntime& rt, const float origin[3], const float mins[3], const float maxs[3], float out[3])
{
	const float lift = rt.world.landfixLift;
	if (!rt.landfixUnlift || !rt.landfixUnlift(origin, lift, mins, maxs, out)) {
		memcpy(out, origin, 3 * sizeof(float));
		out[2] -= lift;
	}
}

// Like PhysicsTouchTriggers, every trigger is found at the final origin before any
// is touched, so a teleport partway through doesn't change what the rest see.
static void PredRunTouch(PredRuntime& rt, PredPlayerIO& io)
{
	PredMap& map = *rt.map;
	PredWorld& w = rt.world;

	float queryOrigin[3] = { io.origin[0], io.origin[1], io.origin[2] };
	PredGatherCandidates(rt, queryOrigin, io.mins, io.maxs);
	float unlifted[3];
	bool haveUnlifted = false;
	// Teleports last: the server's order is its partition's, where the triggers sharing a pass with
	// a teleport were mostly touched before it.
	for (int pass = 0; pass < 2; ++pass)
		for (int i : rt.candidates) {
			const PredSimEnt& e = map.sim[i];
			PredEntState& st = w.ents[i];
			if ((e.cls == PC_TELEPORT) != (pass == 1)) continue;
			if (st.removed) { st.stale = st.linked; continue; }

			// The SourceJump landfix drops the lifted touch of a booster and adds the unlifted one, with
			// TR_ClipRayHullToEntity's MASK_ALL.
			const bool sj = e.sjBooster && w.landfixLift != 0.0f;
			if (sj && !haveUnlifted) {
				haveUnlifted = true;
				PredSjUnlifted(rt, queryOrigin, io.mins, io.maxs, unlifted);
			}
			const bool overlap = !st.disabled && (sj
				? PredBoxInModel(map, e.modelIndex, e.xf, unlifted, io.mins, io.maxs, -1)
				: PredBoxInModel(map, e.modelIndex, e.xf, queryOrigin, io.mins, io.maxs));
			if (overlap) PredLinkAndTouch(rt, i);
			else if (st.linked) st.stale = 1;
		}
	// shavit creates its zone triggers after the map's.
	PredZonesTouch(rt, queryOrigin);
}

// PhysicsCheckForEntityUntouch, which CEntityTouchManager runs once per server frame after all
// the commands of that frame: a link a later command touched again never ends. The touch list
// is newest first.
static void PredUntouch(PredRuntime& rt)
{
	PredWorld& w = rt.world;
	for (size_t k = w.linkedList.size(); k-- > 0; ) {
		const int i = w.linkedList[k];
		if (w.ents[i].stale) PredEndTouch(rt, i);
	}
	ZsState& z = w.zones;
	for (int k = 0; k < ZS_MAX; ++k)
		if (z.stale[k]) z.linked[k] = z.stale[k] = z.in[k] = 0;
}

static void PredZonesBind(PredRuntime& rt)
{
	ZsSet& set = rt.zoneSet;
	set.types = set.unknownTypes = 0;
	rt.simZone.assign(rt.map ? rt.map->sim.size() : 0, -1);
	for (int k = 0; k < (int)set.zones.size() && k < ZS_MAX; ++k) {
		ZsZone& z = set.zones[k];
		z.sim = -1;
		// A button zone only runs the timer, and never makes the player inside one.
		if (z.form == ZS_FORM_BUTTON) continue;
		set.types |= 1 << z.type;
		if (z.form == ZS_FORM_BOX) continue;
		if (rt.map && z.hammerId) {
			const auto it = rt.map->simByHammerId.find(z.hammerId);
			const PredClass want = z.form == ZS_FORM_TELEPORT ? PC_TELEPORT : PC_MULTIPLE;
			if (it != rt.map->simByHammerId.end() && it->second >= 0 && rt.map->sim[it->second].cls == want) {
				z.sim = it->second;
				rt.simZone[z.sim] = k;
			}
		}
		if (z.sim < 0) set.unknownTypes |= 1 << z.type;
	}
}

// shavit drops every player's zones when they reload; the new triggers are touched afresh.
static void PredZonesSet(PredRuntime& rt, const std::vector<ZsZone>& zones, bool active)
{
	rt.zoneSet.zones = zones;
	rt.zoneSet.active = active;
	PredZonesBind(rt);
	rt.world.zones.ClearTouches();
	for (PredWorld& w : rt.ring) w.zones.ClearTouches();
}

static const PredWorld& PredWorldBefore(const PredRuntime& rt, int key)
{
	if ((key <= rt.prevKey || !rt.initialized) && !rt.ringKey.empty()) {
		const int prev = key - 1;
		const int slot = ((prev % PRED_RING_SIZE) + PRED_RING_SIZE) % PRED_RING_SIZE;
		if (rt.ringKey[slot] == prev) return rt.ring[slot];
	}
	return rt.world;
}

static void PredResetWorld(const PredMap& map, PredWorld& w)
{
	w.linkedList.clear();
	w.events.clear();
	w.playerName.clear();
	w.playerClass = "player";
	w.baseVelFlag = false;
	w.gravity = 1.0f;
	w.gravityFix = false;
	w.movedLast = false;
	w.teleportedLast = false;
	w.duckedAtEnd = false;
	w.rngTeleportTick = INT_MIN / 2;
	w.rngTeleportSeq = false;
	w.mpbhopsBlock = -1;
	w.mpbhopsPunish = 0.0f;
	w.zones = ZsState();
	w.ents.assign(map.sim.size(), PredEntState());
	for (size_t i = 0; i < map.sim.size(); ++i) {
		w.ents[i].disabled = map.sim[i].initialDisabled ? 1 : 0;
		w.ents[i].counterValue = map.sim[i].initialCounter;
		w.ents[i].branchValue = map.sim[i].initialBranch ? 1 : 0;
		if (map.sim[i].doorStartOpen) w.ents[i].doorState = PDS_TOP;
	}
	w.actionTimesLeft.resize(map.actions.size());
	for (size_t i = 0; i < map.actions.size(); ++i)
		w.actionTimesLeft[i] = map.actions[i].timesToFire;
}

static void PredReset(PredRuntime& rt)
{
	delete rt.map;
	rt.map = nullptr;
	rt.ring.clear();
	rt.ringKey.clear();
	rt.prevKey = INT_MIN;
	rt.prevTick = INT_MIN;
	rt.initialized = false;
}

static void PredRestart(PredRuntime& rt)
{
	for (int k = 0; k < PredRuntime::kShots; ++k) rt.shotKey[k] = INT_MIN;
	rt.candidates.clear();
	rt.candidates.reserve(rt.map->sim.size());
	rt.stamp.assign(rt.map->sim.size(), 0u);
	rt.stampNow = 0;
	rt.ringKey.assign(PRED_RING_SIZE, INT_MIN);
	rt.prevKey = INT_MIN;
	rt.prevTick = INT_MIN;
	rt.initialized = false;
	PredResetWorld(*rt.map, rt.world);
	PredZonesBind(rt);
	// Reserve up front so commands never allocate. Copies don't keep capacity.
	auto reserve = [](PredWorld& w) {
		w.events.reserve(PRED_MAX_EVENTS);
		w.linkedList.reserve(64);
		w.playerName.reserve(64);
		w.playerClass.reserve(64);
	};
	reserve(rt.world);
	if (!rt.map->vsPrograms.empty()) rt.vsScratch.Reserve();
	rt.ring.assign(PRED_RING_SIZE, rt.world);
	for (PredWorld& w : rt.ring) reserve(w);
}

static void PredAdopt(PredRuntime& rt, PredMap* fresh)
{
	if (rt.map != fresh) delete rt.map;
	rt.map = fresh;
	PredRestart(rt);
}

static void PredCommandStart(PredRuntime& rt, int key)
{
	if (!rt.map) return;
	if (key <= rt.prevKey || !rt.initialized) {
		int prev = key - 1;
		int slot = ((prev % PRED_RING_SIZE) + PRED_RING_SIZE) % PRED_RING_SIZE;
		// No snapshot: history ran out (or, keyed by tickbase, the server moved it). Going
		// back to spawn state would stick, so carry on from the latest state.
		if (rt.ringKey[slot] == prev) rt.world = rt.ring[slot];
	}
	rt.initialized = true;
}

static void PredCommandEnd(PredRuntime& rt, int tick, int key)
{
	if (!rt.map) return;
	int slot = ((key % PRED_RING_SIZE) + PRED_RING_SIZE) % PRED_RING_SIZE;
	rt.ring[slot] = rt.world;
	rt.ringKey[slot] = key;
	rt.prevKey = key;
	rt.prevTick = tick;
}

// Called once the save command has run. Its EndTouch outputs happen later on
// the server, so they aren't in shavit's copy.
static void PredSaveCheckpoint(const PredRuntime& rt, PredCheckpointState& cp)
{
	const PredWorld& w = rt.world;
	const int base = rt.prevTick + 1;
	cp.events.clear();
	for (const PredEvent& ev : w.events) {
		if (!ev.playerQueue || (ev.fromEndTouch && ev.addedTick == rt.prevTick)) continue;
		PredEvent e = ev;
		e.fireTick -= base;
		cp.events.push_back(e);
	}
	cp.multiReady.clear();
	for (int i = 0; i < (int)w.ents.size(); ++i)
		if (w.ents[i].multiReadyTick > base) cp.multiReady.push_back(std::make_pair(i, w.ents[i].multiReadyTick - base));
	cp.playerName = w.playerName;
	cp.playerClass = w.playerClass;
	cp.baseVelFlag = w.baseVelFlag;
	cp.mpbhopsBlock = w.mpbhopsBlock;
	cp.mpbhopsPunish = w.mpbhopsPunish;
}

// shavit untouches everything, then eventqueuefix's events replace the player's,
// so those EndTouch outputs never run. `cp` is null if the sim never saw the save.
static void PredLoadCheckpoint(PredRuntime& rt, const PredCheckpointState* cp)
{
	PredWorld& w = rt.world;
	const int tick = PredEventClock(*rt.io);
	w.zones.ClearTouches();
	if (rt.eventQueueFix) {
		for (PredEntState& st : w.ents) {
			st.linked = st.inList = 0;
			st.multiReadyTick = INT_MIN;
		}
		w.linkedList.clear();
		size_t keep = 0;
		for (size_t i = 0; i < w.events.size(); ++i)
			if (!w.events[i].playerQueue) w.events[keep++] = w.events[i];
		w.events.resize(keep);
		if (cp) {
			for (const PredEvent& ev : cp->events) {
				if ((int)w.events.size() >= PRED_MAX_EVENTS) break;
				PredEvent e = ev;
				e.fireTick += tick;
				w.events.push_back(e);
			}
			for (const auto& m : cp->multiReady)
				if (m.first < (int)w.ents.size()) w.ents[m.first].multiReadyTick = tick + m.second;
		}
	} else {
		rt.serviceBase = tick;
		while (!w.linkedList.empty()) {
			PredEndTouch(rt, *std::min_element(w.linkedList.begin(), w.linkedList.end()));
		}
	}
	if (cp) {
		w.playerName = cp->playerName;
		w.playerClass = cp->playerClass;
		w.baseVelFlag = cp->baseVelFlag;
		w.mpbhopsBlock = cp->mpbhopsBlock;
		w.mpbhopsPunish = cp->mpbhopsPunish;
	}
}

static void PredPreMove(PredRuntime& rt, PredPlayerIO& io)
{
	if (!rt.map) return;
	rt.io = &io;
	rt.viewSnapPending = false;
	rt.vsSteps = PRED_SCRIPT_STEPS;
	rt.mpbhopsStartDoor = -1;
	const int teleports = rt.teleports;
	PredCommandStart(rt, PredSnapshotKey(io));
	if (rt.world.gravityFix) {
		rt.world.gravityFix = false;
		// A checkpoint restore (kzcheckpoints keeps gravity) has set it for this command.
		if (!(rt.cpTeleportPending && !rt.cpKz)) *io.gravity = rt.world.gravity;
	}
	rt.rngTick = io.tick - 1;
	if (rt.cpTeleportPending) {
		// FL_DUCKING is already restored, but the server still tests with last command's hull.
		PredSetHull(io, rt.world.duckedAtEnd);
		// kzcheckpoints teleports before the untouch and restore; only the flags come back.
		if (!rt.cpKz) PredLoadCheckpoint(rt, rt.cpLoad);
		else if (rt.cpLoad) rt.world.baseVelFlag = rt.cpLoad->baseVelFlag;
		rt.world.zones.teleportKey = PredSnapshotKey(io);
		rt.cpTeleportPending = false;
		rt.cpLoad = nullptr;
		rt.cpKz = false;
	}
	if (rt.eventQueueFix) {
		const float before[3] = { io.origin[0], io.origin[1], io.origin[2] };
		if (rt.buttonShots) PredFireShots(rt, PredSnapshotKey(io));
		PredServicePlayerEvents(rt);
		// The server's SetupMove sees a teleport from here this same command.
		if (io.moveFlags && (before[0] != io.origin[0] || before[1] != io.origin[1] || before[2] != io.origin[2]))
			*io.moveFlags |= 2;
	}
	rt.rngTick = io.tick;
	rt.world.rngTeleportSeq = false;
	PredCheckMovingGround(rt.world, io);
	// A trigger move sets m_bGameCodeMovedPlayer on the server, which re-runs
	// CategorizePosition; the client would keep the pre-teleport ground entity.
	if (rt.world.movedLast && io.moveFlags) *io.moveFlags |= 2;

	if (PredZonesOn(rt)) {
		ZsState& z = rt.world.zones;
		const int key = PredSnapshotKey(io);
		z.prevGround = rt.zonePreGroundKey == key ? rt.zonePreGround : (*io.flags & FL_ONGROUND) != 0;
		// PreThinkPost, after shavit's and bhoptools' OnPlayerRunCmd.
		if (ZsHas(rt.zoneSet, ZS_GRAVITY) || ZsHas(rt.zoneSet, ZS_NOTIMERGRAVITY))
			ZsPreThinkGravity(rt.zoneSet, PredZoneSettings(rt, io), z, io.solid, io.moveType, io.gravity);
	}
	PredPlayerUse(rt, io);

	// rngfix counts a teleport here as the previous tick's; two in a row are left alone.
	rt.telehopBlocked = rt.world.teleportedLast || rt.teleports != teleports;
	rt.telehopAir = !(*io.flags & FL_ONGROUND) && io.moveType == PRED_MOVETYPE_WALK;
	rt.telehopVelZ = io.velocity[2];
	rt.telehopBaseVelZ = io.baseVelocity[2];
	rt.telehopGravity = *io.gravity != 0.0f ? *io.gravity : 1.0f;
}

static void PredTelehop(const PredRuntime& rt, PredPlayerIO& io)
{
	const float finishGravity = *io.gravity != 0.0f ? *io.gravity : 1.0f;
	float z = rt.telehopVelZ;
	z -= rt.telehopGravity * 800.0f * 0.5f * io.interval;
	z += rt.telehopBaseVelZ * io.interval;
	z -= finishGravity * 800.0f * 0.5f * io.interval;
	io.velocity[2] = z;
}

static int  PredMpbhopsTouchedDoor(const PredRuntime& rt, const PredPlayerIO& io);
static void PredMpbhopsTouch(PredRuntime& rt, PredPlayerIO& io, int door);

static void PredPostMove(PredRuntime& rt, PredPlayerIO& io)
{
	if (!rt.map) return;
	rt.io = &io;
	rt.serviceBase = PredEventClock(io) + 1;
	const float before[3] = { io.origin[0], io.origin[1], io.origin[2] };
	const bool landed = rt.telehopAir && (*io.flags & FL_ONGROUND);
	const int teleports = rt.teleports;
	// The move's touch list is made before any trigger moves the player.
	const int door = PredMpbhopsTouchedDoor(rt, io);
	// A non-solid player (dead, observing) touches nothing and keeps its links.
	if (io.solid) PredRunTouch(rt, io);
	PredMpbhopsTouch(rt, io, rt.mpbhopsStartDoor);
	// AddToTouched keeps one entry per entity.
	if (door != rt.mpbhopsStartDoor) PredMpbhopsTouch(rt, io, door);
	const bool teleported = rt.teleports != teleports;
	// rngfix does this in PostThink, before any timed event.
	if (rt.postThinkHook) rt.postThinkHook(rt.postThinkUser, rt, io);
	else if (rt.rngfixTelehop && teleported && landed && !rt.telehopBlocked) PredTelehop(rt, io);
	rt.world.teleportedLast = teleported;
	if (io.frameEnd) PredUntouch(rt);
	PredRunDoors(rt);
	PredServiceTimedEvents(rt);
	rt.world.movedLast = before[0] != io.origin[0] || before[1] != io.origin[1] || before[2] != io.origin[2];
	rt.world.duckedAtEnd = (*io.flags & FL_DUCKING) != 0;
	rt.world.gravity = *io.gravity;
	if (rt.landfixEndsLift) rt.world.landfixLift = 0.0f;
	PredCommandEnd(rt, PredEventClock(io), PredSnapshotKey(io));
}

static const int PRED_SYNC_DISABLED = 1 << 0;
static const int PRED_SYNC_REMOVED  = 1 << 1;
static const int PRED_SYNC_SPENT    = 1 << 2;
static const int PRED_SYNC_BRANCH   = 1 << 3;
static const int PRED_SYNC_REFIRE   = 1 << 4;
static const int PRED_SYNC_HITMIN   = 1 << 5;
static const int PRED_SYNC_HITMAX   = 1 << 6;
// A door's m_toggle_state, two bits.
static const int PRED_SYNC_DOOR_SHIFT = 8;

static void PredSyncWorlds(PredRuntime& rt, int cmd, std::vector<PredWorld*>& out)
{
	out.clear();
	int base = -1;
	for (int s = 0; s < (int)rt.ringKey.size(); ++s) {
		const int k = rt.ringKey[s];
		if (k != INT_MIN && k >= cmd && k <= rt.prevKey && (base < 0 || k < rt.ringKey[base])) base = s;
	}
	if (base >= 0) {
		out.push_back(&rt.ring[base]);
		for (int s = 0; s < (int)rt.ringKey.size(); ++s)
			if (rt.ringKey[s] > rt.ringKey[base] && rt.ringKey[s] <= rt.prevKey) out.push_back(&rt.ring[s]);
	}
	out.push_back(&rt.world);
}

// ws[0] is the base. A newer world still holding the base's old value mispredicted it the same way;
// one that differs was changed by our own later commands, which the server hadn't run yet.
template <typename T, typename Field, typename OnSet>
static int PredSyncField(const std::vector<PredWorld*>& ws, Field field, const T& value, OnSet onSet)
{
	const T old = field(*ws[0]);
	if (old == value) return 0;
	for (PredWorld* w : ws)
		if (field(*w) == old) { field(*w) = value; onSet(*w); }
	return 1;
}

static int PredSyncEntity(PredRuntime& rt, int cmd, int hammerId, int flags, float value, int wait)
{
	if (!rt.map || !rt.stateSync || !hammerId) return -1;
	const auto it = rt.map->simByHammerId.find(hammerId);
	if (it == rt.map->simByHammerId.end() || it->second < 0) return -1;
	const int i = it->second;
	const PredClass cls = rt.map->sim[i].cls;
	std::vector<PredWorld*> ws;
	PredSyncWorlds(rt, cmd, ws);

	auto none = [](PredWorld&) {};
	auto syncFlag = [&](uint8_t PredEntState::*f, int bit, bool unlinks) {
		const uint8_t v = (flags & bit) ? 1 : 0;
		return PredSyncField(ws, [&](PredWorld& w) -> uint8_t& { return w.ents[i].*f; }, v, [&](PredWorld& w) {
			// The server ended that link when it happened, outputs and all.
			if (!unlinks || !v) return;
			PredSetLinked(w, i, false);
			w.ents[i].inList = w.ents[i].stale = 0;
		});
	};
	int fixed = syncFlag(&PredEntState::removed, PRED_SYNC_REMOVED, true);
	// The server doesn't keep the rest of a removed entity.
	if (flags & PRED_SYNC_REMOVED) return fixed;
	fixed += syncFlag(&PredEntState::disabled, PRED_SYNC_DISABLED, true);
	fixed += syncFlag(&PredEntState::spent, PRED_SYNC_SPENT, false);
	fixed += syncFlag(&PredEntState::branchValue, PRED_SYNC_BRANCH, false);
	fixed += syncFlag(&PredEntState::waitForRefire, PRED_SYNC_REFIRE, false);
	fixed += syncFlag(&PredEntState::hitMin, PRED_SYNC_HITMIN, false);
	fixed += syncFlag(&PredEntState::hitMax, PRED_SYNC_HITMAX, false);
	if (cls == PC_COUNTER)
		fixed += PredSyncField(ws, [&](PredWorld& w) -> float& { return w.ents[i].counterValue; }, value, none);
	// wait: how many of our commands after cmd can't activate it. Any ready tick up to cmd + 1 is ready.
	if (cls == PC_MULTIPLE && rt.eventQueueFix && wait >= 0) {
		const int old = ws[0]->ents[i].multiReadyTick;
		const int ready = wait == 0 && old <= cmd + 1 ? old : cmd + 1 + wait;
		fixed += PredSyncField(ws, [&](PredWorld& w) -> int& { return w.ents[i].multiReadyTick; }, ready, none);
	}
	// wait: the door's move, or its wait at the top, ends in the frame of command cmd + wait.
	if (cls == PC_DOOR) {
		const uint8_t state = (flags >> PRED_SYNC_DOOR_SHIFT) & 3;
		const int done = wait >= 0 ? cmd + wait : INT_MAX;
		fixed += PredSyncField(ws, [&](PredWorld& w) -> uint8_t& { return w.ents[i].doorState; }, state, none);
		fixed += PredSyncField(ws, [&](PredWorld& w) -> int& { return w.ents[i].doorDoneTick; }, done, none);
	}
	return fixed;
}

static int PredSyncPlayer(PredRuntime& rt, int cmd, float gravity, bool baseVelFlag, const std::string& name,
	const std::string& cls)
{
	if (!rt.map || !rt.stateSync) return -1;
	std::vector<PredWorld*> ws;
	PredSyncWorlds(rt, cmd, ws);
	auto none = [](PredWorld&) {};
	int fixed = PredSyncField(ws, [](PredWorld& w) -> std::string& { return w.playerName; }, name, none);
	fixed += PredSyncField(ws, [](PredWorld& w) -> std::string& { return w.playerClass; }, cls, none);
	fixed += PredSyncField(ws, [](PredWorld& w) -> bool& { return w.baseVelFlag; }, baseVelFlag, none);
	fixed += PredSyncField(ws, [](PredWorld& w) -> float& { return w.gravity; }, gravity,
		[](PredWorld& w) { w.gravityFix = true; });
	return fixed;
}

#include "mpbhops_sim.h"
