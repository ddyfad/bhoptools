// Mirrors Nimmy2222/shavit-syncstyle 1.2.0's OnPlayerRunCmd.
#pragma once

#include <climits>
#include <cmath>

static const int AS_IN_JUMP      = 1 << 1;
static const int AS_IN_MOVELEFT  = 1 << 9;
static const int AS_IN_MOVERIGHT = 1 << 10;

static const int AS_MOVETYPE_NONE   = 0;
static const int AS_MOVETYPE_NOCLIP = 8;
static const int AS_MOVETYPE_LADDER = 9;

// Where syncstyle's OnPlayerRunCmd runs (plugin load order); bhoptools_autosync, must match the server plugin.
static const int AS_BEFORE_CORE     = 0; // before shavit-core's key blocking
static const int AS_BEFORE_BHOPTOOLS = 1; // after it, before bhoptools' teleports, restarts, noclip and angle shifts
static const int AS_AFTER_BHOPTOOLS  = 2;

// `known` false: no command logged yet, so the last angles are taken to be the command's own.
struct AsState { float angles[3]; int groundTicks; bool known; };

struct AsMove { float angles[3]; float side; int buttons; };

// Read before the move.
struct AsPlayer { bool onGround; int moveType; int waterLevel; };

// shavit's GetAngleDiff, in SourcePawn's 32-bit floats.
static inline float AsAngleDiff(float current, float previous)
{
	const float diff = current - previous;
	return diff - 360.0f * (float)(int)floorf((diff + 180.0f) / 360.0f);
}

static inline bool AsRuns(const AsPlayer& p)
{
	return p.moveType != AS_MOVETYPE_NONE && p.moveType != AS_MOVETYPE_NOCLIP
		&& p.moveType != AS_MOVETYPE_LADDER && p.waterLevel < 2;
}

static inline float AsStep(AsState& st, float angles[3], int buttons, bool onGround)
{
	if (onGround) {
		if (++st.groundTicks == 1 && (buttons & AS_IN_JUMP)) st.groundTicks = 0;
	} else {
		st.groundTicks = 0;
	}
	const float last[3] = { st.known ? st.angles[0] : angles[0], st.known ? st.angles[1] : angles[1],
		st.known ? st.angles[2] : angles[2] };
	const float diff = AsAngleDiff(angles[1], last[1]);
	for (int i = 0; i < 3; ++i) {
		st.angles[i] = angles[i];
		angles[i] = last[i];
	}
	st.known = true;
	return diff;
}

// `surfing()` is IsSurfing's trace, only made when everything else lets the sidemove through.
// False where the plugin returns early.
template <typename Surf>
static inline bool AsApply(AsState& st, AsMove& m, const AsPlayer& p, int teleTicks, Surf surfing)
{
	if (!AsRuns(p)) return false;
	const float diff = AsStep(st, m.angles, m.buttons, p.onGround);
	if (st.groundTicks == 0 && fabsf(diff) > 0.01f && teleTicks > 8 && !surfing()) {
		if (diff > 0.0f) {
			m.side = -400.0f;
			m.buttons = (m.buttons | AS_IN_MOVELEFT) & ~AS_IN_MOVERIGHT;
		} else {
			m.side = 400.0f;
			m.buttons = (m.buttons | AS_IN_MOVERIGHT) & ~AS_IN_MOVELEFT;
		}
	}
	return true;
}

// The teleport count is the commands the plugin ran on since the last command that saw it at 0: the server's
// (`zero`), or a teleport predicted since its report.
struct AsTracker {
	enum { kReports = 8, kRuns = 128 };
	struct Report { int cmd, seq; bool on; int order, zero; };
	struct Run {
		int cmd, seq;
		AsPlayer player;
		float angles[3];
		int buttons;
		bool ran;
		int zero;  // a predicted teleport makes this command the count's 0, INT_MIN for none
		AsState after;
	};

	Report reports[kReports];
	int reportCount = 0;
	int seq = 0;
	Run runs[kRuns];

	AsTracker() { Clear(); }

	void Clear()
	{
		reportCount = 0;
		for (Run& r : runs) r.cmd = INT_MIN;
	}

	// What applies after command `cmd`; zero <= 0 for no teleport known.
	void AddReport(int cmd, bool on, int order, int zero)
	{
		while (reportCount > 0 && reports[reportCount - 1].cmd >= cmd) --reportCount;
		if (reportCount == kReports) {
			for (int i = 1; i < kReports; ++i) reports[i - 1] = reports[i];
			--reportCount;
		}
		reports[reportCount++] = { cmd, ++seq, on, order, zero > 0 ? zero : INT_MIN };
	}

	int ReportBefore(int cmd) const
	{
		for (int i = reportCount - 1; i >= 0; --i) if (reports[i].cmd < cmd) return i;
		return -1;
	}

	int OrderFor(int cmd) const
	{
		const int ri = ReportBefore(cmd);
		return ri >= 0 ? reports[ri].order : AS_BEFORE_BHOPTOOLS;
	}

	// Commands missing from the log are taken to change nothing.
	AsState StateBefore(int ri, int cmd)
	{
		const Report& r = reports[ri];
		const Run& prev = runs[(cmd - 1) & (kRuns - 1)];
		if (prev.cmd == cmd - 1 && prev.seq == r.seq) return prev.after;
		AsState st = { { 0, 0, 0 }, 0, false };
		const int from = cmd - r.cmd > kRuns ? cmd - kRuns + 1 : r.cmd + 1;
		if (from == r.cmd + 1 && ri > 0) {
			st = StateBefore(ri - 1, from);
		} else {
			for (int c = from - 1; c > cmd - kRuns; --c) {
				const Run& run = runs[c & (kRuns - 1)];
				if (run.cmd == c) { st = run.after; break; }
			}
		}
		for (int c = from; c < cmd; ++c) {
			Run& run = runs[c & (kRuns - 1)];
			if (run.cmd != c) continue;
			float angles[3] = { run.angles[0], run.angles[1], run.angles[2] };
			run.ran = r.on && AsRuns(run.player);
			if (run.ran) AsStep(st, angles, run.buttons, run.player.onGround);
			run.after = st;
			run.seq = r.seq;
		}
		return st;
	}

	int TeleTicks(const Report& r, int cmd) const
	{
		int zero = r.zero;
		for (int c = cmd - r.cmd > kRuns ? cmd - kRuns + 1 : r.cmd + 1; c <= cmd; ++c) {
			const Run& run = runs[c & (kRuns - 1)];
			if (run.cmd == c && run.zero > zero) zero = run.zero;
		}
		if (zero == INT_MIN || cmd - zero >= kRuns) return INT_MAX;
		int n = 0;
		for (int c = zero; c < cmd && n <= 8; ++c) {
			const Run& run = runs[c & (kRuns - 1)];
			if (run.cmd != c || run.ran) ++n;
		}
		return n;
	}

	// `zeroNow`: `cmd` if a teleport predicted on it came before the plugin, else INT_MIN.
	template <typename Surf>
	bool Apply(int cmd, AsMove& m, const AsPlayer& p, int zeroNow, Surf surfing)
	{
		Run& run = runs[cmd & (kRuns - 1)];
		run.cmd = cmd;
		run.seq = 0;
		run.player = p;
		for (int i = 0; i < 3; ++i) run.angles[i] = m.angles[i];
		run.buttons = m.buttons;
		run.ran = false;
		run.zero = zeroNow;
		run.after = { { 0, 0, 0 }, 0, false };
		const int ri = ReportBefore(cmd);
		if (ri < 0) return false;
		const Report& r = reports[ri];
		AsState st = StateBefore(ri, cmd);
		if (r.on) run.ran = AsApply(st, m, p, TeleTicks(r, cmd), surfing);
		run.after = st;
		run.seq = r.seq;
		return run.ran;
	}

	// A teleport predicted after the plugin ran on `cmd`: the next command sees the count at 0.
	void NoteTeleport(int cmd)
	{
		Run& run = runs[cmd & (kRuns - 1)];
		if (run.cmd == cmd && run.zero < cmd + 1) run.zero = cmd + 1;
	}
};
