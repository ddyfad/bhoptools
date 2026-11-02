// Mirrors shavit-core's style key blocking.
#pragma once

#include <climits>

// bhoptools_keys flags; must match the server plugin.
static const int SK_BLOCK_W        = 1 << 0;
static const int SK_BLOCK_A        = 1 << 1;
static const int SK_BLOCK_S        = 1 << 2;
static const int SK_BLOCK_D        = 1 << 3;
static const int SK_AD_ONLY        = 1 << 4;
static const int SK_HSW            = 1 << 5;  // force_hsw > 0
static const int SK_SHSW           = 1 << 6;  // force_hsw 2
static const int SK_GROUNDKEYS     = 1 << 7;
static const int SK_PLEFT          = 1 << 8;  // block_pleft > 0
static const int SK_PRIGHT         = 1 << 9;  // block_pright > 0
static const int SK_TIMER          = 1 << 10; // timer running and not paused
static const int SK_NOZONES        = 1 << 11; // shavit-zones isn't loaded
static const int SK_INSTART        = 1 << 12; // in a start zone of the timer's track
static const int SK_FREESTYLE      = 1 << 13;
static const int SK_ALLKEYS        = 1 << 14;
static const int SK_SIMPLERLADDERS = 1 << 15;
static const int SK_PAUSED         = 1 << 16; // paused with shavit_core_pause_movement 0 or 3: shavit returns first

static const int SK_IN_FORWARD   = 1 << 3;
static const int SK_IN_BACK      = 1 << 4;
static const int SK_IN_LEFT      = 1 << 7;
static const int SK_IN_RIGHT     = 1 << 8;
static const int SK_IN_MOVELEFT  = 1 << 9;
static const int SK_IN_MOVERIGHT = 1 << 10;

static const int SK_MOVETYPE_NOCLIP = 8;
static const int SK_MOVETYPE_LADDER = 9;

// bCanUseAllKeys and iKeyCombo (-1 none, A/D-only: 0 A, 1 D; surf HSW: 0 W+A/S+D, 1 W+D/S+A).
struct SkState { bool allKeys; int combo; };

struct SkMove { float forward, side; int buttons; };

// In shavit-core's order. onGround is m_hGroundEntity set; it and moveType are from before the move.
static inline void SkApply(int flags, SkState& st, SkMove& m, bool onGround, int moveType)
{
	if (flags & SK_PAUSED) return;

	if ((flags & SK_TIMER) && ((flags & SK_NOZONES) || (!(flags & SK_INSTART)
			&& (((flags & SK_PLEFT) && (m.buttons & SK_IN_LEFT)) || ((flags & SK_PRIGHT) && (m.buttons & SK_IN_RIGHT)))))) {
		m.forward = 0.0f;
		m.side = 0.0f;
	}

	if (moveType == SK_MOVETYPE_LADDER && (flags & SK_SIMPLERLADDERS)) st.allKeys = true;
	else if (onGround) st.allKeys = false;

	if (st.allKeys || moveType == SK_MOVETYPE_NOCLIP || moveType == SK_MOVETYPE_LADDER || (flags & SK_FREESTYLE)) return;
	if (onGround && !(flags & SK_GROUNDKEYS)) return;

	if ((flags & SK_BLOCK_W) && ((m.buttons & SK_IN_FORWARD) || m.forward > 0.0f)) {
		m.forward = 0.0f;
		m.buttons &= ~SK_IN_FORWARD;
	}
	if ((flags & SK_BLOCK_A) && ((m.buttons & SK_IN_MOVELEFT) || m.side < 0.0f)) {
		m.side = 0.0f;
		m.buttons &= ~SK_IN_MOVELEFT;
	}
	if ((flags & SK_BLOCK_S) && ((m.buttons & SK_IN_BACK) || m.forward < 0.0f)) {
		m.forward = 0.0f;
		m.buttons &= ~SK_IN_BACK;
	}
	if ((flags & SK_BLOCK_D) && ((m.buttons & SK_IN_MOVERIGHT) || m.side > 0.0f)) {
		m.side = 0.0f;
		m.buttons &= ~SK_IN_MOVERIGHT;
	}

	if (flags & SK_AD_ONLY) {
		const int combo = ((m.buttons & SK_IN_MOVELEFT) && m.side < 0.0f) ? 0
			: ((m.buttons & SK_IN_MOVERIGHT) && m.side > 0.0f) ? 1 : -1;
		if (combo != -1) {
			if (st.combo == -1) st.combo = combo;
			if (combo != st.combo) {
				m.side = 0.0f;
				m.buttons &= ~(SK_IN_MOVELEFT | SK_IN_MOVERIGHT);
			}
		}
	}

	if (flags & SK_HSW) {
		// All four are read before any is cleared.
		const bool fwd   = (m.buttons & SK_IN_FORWARD) && m.forward > 0.0f;
		const bool left  = (m.buttons & SK_IN_MOVELEFT) && m.side < 0.0f;
		const bool back  = (m.buttons & SK_IN_BACK) && m.forward < 0.0f;
		const bool right = (m.buttons & SK_IN_MOVERIGHT) && m.side > 0.0f;
		// Surf HSW only picks its pair outside the start zone; inside it's plain HSW.
		if ((flags & SK_SHSW) && !(flags & SK_INSTART)) {
			const int combo = ((fwd && left) || (back && right)) ? 0 : ((fwd && right) || (back && left)) ? 1 : -1;
			if (st.combo == -1 && combo != -1) st.combo = combo;
			if (combo != st.combo || combo == -1) {
				m.forward = 0.0f;
				m.side = 0.0f;
				m.buttons &= ~(SK_IN_FORWARD | SK_IN_MOVELEFT | SK_IN_MOVERIGHT | SK_IN_BACK);
			}
		} else {
			if ((back && (left || right)) || (fwd && !(left || right))) {
				m.forward = 0.0f;
				m.buttons &= ~(SK_IN_FORWARD | SK_IN_BACK);
			}
			if ((left || right) && !fwd) {
				m.side = 0.0f;
				m.buttons &= ~(SK_IN_MOVELEFT | SK_IN_MOVERIGHT);
			}
		}
	}
}

// Server reports and the commands run since. A command starts from the state its report left,
// carried over every command logged after it, so a report changes the prediction of commands
// already predicted too, and replays give the same answer as the first run.
struct SkTracker {
	enum { kReports = 8, kRuns = 128 };
	struct Report { int cmd, flags, seq; SkState state; };
	struct Run { int cmd, seq; bool onGround; int moveType; int zoneMask, zoneBits; SkMove move; SkState after; };

	Report reports[kReports];
	int reportCount = 0;
	int seq = 0;
	Run runs[kRuns];

	SkTracker() { Clear(); }

	void Clear()
	{
		reportCount = 0;
		for (Run& r : runs) r.cmd = INT_MIN;
	}

	// The state after command `cmd`, in order of commands; one out of order replaces what it overtook.
	void AddReport(int cmd, int flags, int combo)
	{
		while (reportCount > 0 && reports[reportCount - 1].cmd >= cmd) --reportCount;
		if (reportCount == kReports) {
			for (int i = 1; i < kReports; ++i) reports[i - 1] = reports[i];
			--reportCount;
		}
		reports[reportCount++] = { cmd, flags, ++seq, { (flags & SK_ALLKEYS) != 0, combo } };
	}

	const Report* ReportBefore(int cmd) const
	{
		for (int i = reportCount - 1; i >= 0; --i) if (reports[i].cmd < cmd) return &reports[i];
		return nullptr;
	}

	// Commands missing from the log (not run, or overwritten) are taken to change nothing.
	SkState StateBefore(const Report& r, int cmd)
	{
		const Run& prev = runs[(cmd - 1) & (kRuns - 1)];
		if (r.cmd == cmd - 1) return r.state;
		if (prev.cmd == cmd - 1 && prev.seq == r.seq) return prev.after;
		SkState st = r.state;
		for (int c = cmd - r.cmd > kRuns ? cmd - kRuns + 1 : r.cmd + 1; c < cmd; ++c) {
			Run& run = runs[c & (kRuns - 1)];
			if (run.cmd != c) continue;
			SkMove m = run.move;
			SkApply((r.flags & ~run.zoneMask) | run.zoneBits, st, m, run.onGround, run.moveType);
			run.after = st;
			run.seq = r.seq;
		}
		return st;
	}

	// zoneMask: the zone flags predicted for this command, which replace the report's.
	void Apply(int cmd, SkMove& m, bool onGround, int moveType, int zoneMask = 0, int zoneBits = 0)
	{
		Run& run = runs[cmd & (kRuns - 1)];
		run.cmd = cmd;
		run.seq = 0;
		run.onGround = onGround;
		run.moveType = moveType;
		run.zoneMask = zoneMask;
		run.zoneBits = zoneBits & zoneMask;
		run.move = m;
		const Report* r = ReportBefore(cmd);
		if (!r) return;
		SkState st = StateBefore(*r, cmd);
		SkApply((r->flags & ~zoneMask) | run.zoneBits, st, m, onGround, moveType);
		run.after = st;
		run.seq = r->seq;
	}
};
