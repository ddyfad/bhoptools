#pragma once

// The server's rngfix cvars. Nothing is predicted until the plugin has reported
// them, since guessing wrong causes the mispredictions this is meant to avoid.
struct RngSettings {
	bool known       = false;
	int  downhill    = 1;
	int  uphill      = 1;
	int  edge        = 1;
	int  triggerjump = 1;
	int  telehop     = 1;
	int  stairs      = 1;     // rngfix only applies it on surf_ maps
	int  oldSlopefix = 0;
	// Not rngfix's: shavit clears IN_JUMP from m_nOldButtons before rngfix reads it.
	bool autobhop    = false;

	bool Active() const { return known; }
};

struct RngTraceResult {
	float fraction   = 1.0f;
	float endpos[3]  = {0, 0, 0};
	float normal[3]  = {0, 0, 0};
	bool  startSolid = false;
	bool  allSolid   = false;
};

class RngTracer {
public:
	virtual ~RngTracer() {}
	// False when the backend can't trace; callers then skip the fix.
	virtual bool TraceHull(const float start[3], const float end[3],
	                       const float mins[3], const float maxs[3], RngTraceResult& out) = 0;
};
