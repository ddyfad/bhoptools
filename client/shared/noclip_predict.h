// Reapplied on every replay of the impulse's command.
#pragma once

static const int NC_IMPULSE_TOGGLE = 154;  // sm_nc, sm_noclip, sm_noclipme, sm_prac, sm_practice
static const int NC_IMPULSE_ON     = 155;  // +noclip
static const int NC_IMPULSE_OFF    = 156;  // -noclip

static const int NC_MOVETYPE_WALK   = 2;
static const int NC_MOVETYPE_NOCLIP = 8;

// bhoptools_noclip flags: what shavit-misc would allow right now.
static const int NC_ALLOW_ENTER = 1 << 0;  // no stop-timer warning menu
static const int NC_ALLOW_LEAVE = 1 << 1;
static const int NC_ALLOW_PLUS  = 1 << 2;  // +noclip has its own access check and warning
static const int NC_ALLOW_ALL   = NC_ALLOW_ENTER | NC_ALLOW_LEAVE | NC_ALLOW_PLUS;

static inline bool NcIsImpulse(int impulse) { return impulse >= NC_IMPULSE_TOGGLE && impulse <= NC_IMPULSE_OFF; }

// What shavit-misc sets, -1 for no change.
static inline int NcResult(int impulse, int moveType, int allow)
{
	switch (impulse) {
	case NC_IMPULSE_TOGGLE:
		if (moveType != NC_MOVETYPE_NOCLIP) return (allow & NC_ALLOW_ENTER) ? NC_MOVETYPE_NOCLIP : -1;
		return (allow & NC_ALLOW_LEAVE) ? NC_MOVETYPE_WALK : -1;
	case NC_IMPULSE_ON:
		return moveType != NC_MOVETYPE_NOCLIP && (allow & NC_ALLOW_PLUS) ? NC_MOVETYPE_NOCLIP : -1;
	case NC_IMPULSE_OFF:
		return moveType == NC_MOVETYPE_NOCLIP ? NC_MOVETYPE_WALK : -1;
	}
	return -1;
}

struct NcRecord { int key; bool apply; };

struct NcLog {
	enum { kSize = 32 };
	NcRecord r[kSize];
	int count = 0;

	void Clear() { count = 0; }

	NcRecord* Find(int key)
	{
		for (int i = 0; i < count; ++i) if (r[i].key == key) return &r[i];
		return nullptr;
	}

	// Dropping the oldest is fine: a replay never goes back 32 noclip presses.
	NcRecord* Add(int key, bool apply)
	{
		if (count == kSize) {
			for (int i = 1; i < kSize; ++i) r[i - 1] = r[i];
			--count;
		}
		NcRecord& n = r[count++];
		n.key = key;
		n.apply = apply;
		return &n;
	}
};

static inline int NcPredict(NcLog& log, int key, int impulse, int moveType, bool predicting, int allow)
{
	NcRecord* rec = log.Find(key);
	if (!rec) rec = log.Add(key, predicting && NcResult(impulse, moveType, allow) >= 0);
	if (!rec->apply) return moveType;
	const int to = NcResult(impulse, moveType, NC_ALLOW_ALL);
	return to >= 0 ? to : moveType;
}

static inline void NcAck(NcLog& log, int key, bool done)
{
	NcRecord* rec = log.Find(key);
	if (!rec) rec = log.Add(key, done);
	rec->apply = done;
}
