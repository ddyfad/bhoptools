// Decided on first sight of the command, then replaced by the server's ack.
#pragma once

#include <climits>

static const int RS_IMPULSE_RESTART  = 157;  // sm_r, sm_restart, sm_start
static const int RS_IMPULSE_SETSTART = 158;  // sm_sp, sm_ss, sm_setstart, sm_startpoint

// What a restart does, in bhoptools_restart and the restart ack.
static const int RS_TELEPORT = 1 << 0;  // restarts and teleports to the origin with zero velocity
static const int RS_ANGLES   = 1 << 1;  // and sets the view to pitch/yaw
static const int RS_START    = 1 << 2;  // the timer starts: gravity and laggedmovement become the given ones
static const int RS_STAMINA  = 1 << 3;  // shavit-misc zeroes the stamina
static const int RS_WALK     = 1 << 4;  // shavit-misc sets movetype walk
// bhoptools_restart only: what a setstart does to the destination.
static const int RS_SS_GROUND      = 1 << 5;  // on the ground: 1 unit above it becomes the destination, with its angles
static const int RS_SS_GROUND_KEEP = 1 << 6;  // on the ground: no change (it's for another track)
static const int RS_SS_AIR         = 1 << 7;  // in the air: only the angles change
static const int RS_SS_AIR_KEEP    = 1 << 8;  // in the air: no change

static inline bool RsIsImpulse(int impulse) { return impulse == RS_IMPULSE_RESTART || impulse == RS_IMPULSE_SETSTART; }

struct RsDest {
	int   flags = 0;
	float origin[3] = { 0.0f, 0.0f, 0.0f };
	float pitch = 0.0f, yaw = 0.0f;
	float gravity = 1.0f, lagged = 1.0f;
};

struct RsSetStart { int key; bool ground; float origin[3]; float pitch, yaw; };

struct RsRecord {
	int    key;
	bool   apply;
	RsDest dest;
	// laggedmovement before the first run set it, -1 if it didn't. It isn't in the prediction tables,
	// so a value the server didn't set stays until something puts it back.
	float  oldLagged;
};

static inline void RsApplySetStart(RsDest& d, const RsSetStart& s)
{
	if (!(d.flags & RS_TELEPORT)) return;
	if (s.ground && (d.flags & RS_SS_GROUND)) {
		d.origin[0] = s.origin[0];
		d.origin[1] = s.origin[1];
		d.origin[2] = s.origin[2] + 1.0f;
		d.pitch = s.pitch;
		d.yaw = s.yaw;
		// A setstart with a position replaces the start zone, whose centre an angles-only one would need.
		d.flags = (d.flags | RS_ANGLES) & ~(RS_START | RS_SS_AIR | RS_SS_AIR_KEEP);
	} else if (!s.ground && (d.flags & RS_SS_AIR)) {
		d.pitch = s.pitch;
		d.yaw = s.yaw;
		d.flags |= RS_ANGLES;
	} else if (!(d.flags & (s.ground ? RS_SS_GROUND_KEEP : RS_SS_AIR_KEEP))) {
		d.flags &= ~RS_TELEPORT;
	}
}

struct RsPredictor {
	enum { kLog = 32, kSetStarts = 8 };
	RsDest     state;
	int        stateCmd = INT_MIN; // which holds from the command after this one
	RsSetStart ss[kSetStarts];     // sorted by key
	int        ssCount = 0;
	RsRecord   log[kLog];
	int        logCount = 0;

	void Clear()
	{
		state = RsDest();
		stateCmd = INT_MIN;
		ssCount = 0;
		logCount = 0;
	}

	// The server's state already has our setstarts up to `cmd`.
	void SetState(int cmd, const RsDest& d)
	{
		state = d;
		stateCmd = cmd;
		int n = 0;
		for (int i = 0; i < ssCount; ++i)
			if (ss[i].key > cmd) ss[n++] = ss[i];
		ssCount = n;
	}

	// Replays of the setstart command update it; the oldest falls off when full.
	void SetStart(int key, bool ground, const float origin[3], float pitch, float yaw)
	{
		int i = 0;
		while (i < ssCount && ss[i].key < key) ++i;
		if (i == ssCount || ss[i].key != key) {
			if (ssCount == kSetStarts) {
				if (i == 0) return;
				for (int j = 1; j < ssCount; ++j) ss[j - 1] = ss[j];
				--ssCount;
				--i;
			}
			for (int j = ssCount; j > i; --j) ss[j] = ss[j - 1];
			++ssCount;
		}
		ss[i] = { key, ground, { origin[0], origin[1], origin[2] }, pitch, yaw };
	}

	RsDest Destination(int key) const
	{
		RsDest d = state;
		for (int i = 0; i < ssCount && ss[i].key < key; ++i)
			if (ss[i].key > stateCmd) RsApplySetStart(d, ss[i]);
		return d;
	}

	RsRecord* Find(int key)
	{
		for (int i = 0; i < logCount; ++i) if (log[i].key == key) return &log[i];
		return nullptr;
	}

	RsRecord* Add(int key, bool apply, const RsDest& d)
	{
		if (logCount == kLog) {
			for (int i = 1; i < kLog; ++i) log[i - 1] = log[i];
			--logCount;
		}
		RsRecord& r = log[logCount++];
		r.key = key;
		r.apply = apply;
		r.dest = d;
		r.oldLagged = -1.0f;
		return &r;
	}

	// The restart on command `key`, decided the first time it runs.
	RsRecord* Decide(int key, bool predicting)
	{
		if (RsRecord* r = Find(key)) return r;
		const RsDest d = Destination(key);
		return Add(key, predicting && (d.flags & RS_TELEPORT), d);
	}

	RsRecord* Ack(int key, bool done, const RsDest& d)
	{
		RsRecord* r = Find(key);
		if (!r) r = Add(key, done, d);
		r->apply = done;
		if (done) r->dest = d;
		return r;
	}
};
