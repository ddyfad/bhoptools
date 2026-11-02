#pragma once

#include "rngfix_api.h"

#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace etrace {

const unsigned int kMaskPlayerSolid = 0x1 | 0x4000 | 0x10000 | 0x2 | 0x2000000 | 0x8;
static_assert(kMaskPlayerSolid == 0x201400Bu, "MASK_PLAYERSOLID");

// TRACE_EVERYTHING doesn't pass static props through the filter, same as SourceMod's
// TR_TraceHullFilter.
enum TraceType { kTraceEverything = 0, kTraceWorldOnly = 1 };

// NUM_SERIAL_NUM_BITS is 16 in the x64 SDK (it was 0xfff/0x1fff before).
const uint32_t kEntEntryMask   = 0xFFFF;
const uint32_t kInvalidEHandle = 0xFFFFFFFFu;

// VectorAligned. The engine's SSE paths load all four floats.
struct alignas(16) VectorAlignedX { float x, y, z, w; };

// Ray_t. No m_pWorldAxisTransform in this SDK.
struct alignas(16) RayX {
	VectorAlignedX start;
	VectorAlignedX delta;
	VectorAlignedX startOffset;
	VectorAlignedX extents;
	bool           isRay;
	bool           isSwept;
};
static_assert(offsetof(RayX, start) == 0x00 && offsetof(RayX, delta) == 0x10
	&& offsetof(RayX, startOffset) == 0x20 && offsetof(RayX, extents) == 0x30
	&& offsetof(RayX, isRay) == 0x40 && offsetof(RayX, isSwept) == 0x41
	&& sizeof(RayX) == 0x50 && alignof(RayX) == 16, "Ray_t layout");

// Ray_t::Init step for step, so results match the engine's own rays.
inline void InitRay(RayX& r, const float start[3], const float end[3],
                    const float mins[3], const float maxs[3])
{
	memset(&r, 0, sizeof(r));
	r.delta.x = end[0] - start[0];
	r.delta.y = end[1] - start[1];
	r.delta.z = end[2] - start[2];
	const float dl = r.delta.x * r.delta.x + r.delta.y * r.delta.y + r.delta.z * r.delta.z;
	r.isSwept = dl != 0;

	r.extents.x = (maxs[0] - mins[0]) * 0.5f;
	r.extents.y = (maxs[1] - mins[1]) * 0.5f;
	r.extents.z = (maxs[2] - mins[2]) * 0.5f;
	const float el = r.extents.x * r.extents.x + r.extents.y * r.extents.y + r.extents.z * r.extents.z;
	r.isRay = el < 1e-6;

	r.startOffset.x = (mins[0] + maxs[0]) * 0.5f;
	r.startOffset.y = (mins[1] + maxs[1]) * 0.5f;
	r.startOffset.z = (mins[2] + maxs[2]) * 0.5f;
	r.start.x = start[0] + r.startOffset.x;
	r.start.y = start[1] + r.startOffset.y;
	r.start.z = start[2] + r.startOffset.z;
	r.startOffset.x *= -1.0f;
	r.startOffset.y *= -1.0f;
	r.startOffset.z *= -1.0f;
}

struct PlaneX {
	float   normal[3];
	float   dist;
	uint8_t type;
	uint8_t signbits;
	uint8_t pad[2];
};
static_assert(sizeof(PlaneX) == 20, "cplane_t size");

struct SurfaceX {
	const char*    name;
	short          surfaceProps;
	unsigned short flags;
};
static_assert(sizeof(SurfaceX) == 16, "csurface_t size");

// CBaseTrace + CGameTrace, flattened. CBaseTrace ends at 0x38 with no tail padding, so
// Itanium's reuse of a non-POD base's tail padding can't move anything; MSVC never reuses it.
struct GameTraceX {
	float          startpos[3];
	float          endpos[3];
	PlaneX         plane;
	float          fraction;
	int            contents;
	unsigned short dispFlags;
	bool           allsolid;
	bool           startsolid;
	float          fractionleftsolid;
	SurfaceX       surface;
	int            hitgroup;
	short          physicsbone;
	void*          ent;
	int            hitbox;
};
static_assert(offsetof(GameTraceX, endpos) == 0x0C && offsetof(GameTraceX, plane) == 0x18
	&& offsetof(GameTraceX, fraction) == 0x2C && offsetof(GameTraceX, contents) == 0x30
	&& offsetof(GameTraceX, dispFlags) == 0x34 && offsetof(GameTraceX, allsolid) == 0x36
	&& offsetof(GameTraceX, startsolid) == 0x37 && offsetof(GameTraceX, fractionleftsolid) == 0x38
	&& offsetof(GameTraceX, surface) == 0x40 && offsetof(GameTraceX, hitgroup) == 0x50
	&& offsetof(GameTraceX, physicsbone) == 0x54 && offsetof(GameTraceX, ent) == 0x58
	&& offsetof(GameTraceX, hitbox) == 0x60 && sizeof(GameTraceX) == 0x68, "CGameTrace layout");

// Slack in case this build's trace_t is larger than the SDK's.
struct alignas(16) TraceBufferX {
	GameTraceX tr;
	unsigned char slack[512];
};

typedef bool (*EntityHandleFn)(void* ent, uint32_t* handle);

// ITraceFilter. No virtual destructor, so slot 0 is ShouldHitEntity and slot 1 GetTraceType
// on both ABIs. The engine reads nothing but the vtable.
class PlayerFilter {
public:
	virtual bool ShouldHitEntity(void* ent, int /*contentsMask*/)
	{
		if (!ignorePlayers || !ent) return true;
		uint32_t h;
		// Can't tell, so hit it: a missed player shows up as startsolid in Validate.
		if (!handleOf || !handleOf(ent, &h)) return true;
		if (h == kInvalidEHandle) return true;
		const int index = (int)(h & kEntEntryMask);
		return !(index >= 1 && index <= maxClients);
	}
	virtual TraceType GetTraceType() const { return kTraceEverything; }

	EntityHandleFn handleOf = nullptr;
	bool     ignorePlayers  = true;
	int      maxClients     = 64;
};

enum ValidateResult { kValidatePending, kValidateOk, kValidateFailed };

// One bad result, not a broken ABI, so it doesn't fail the tracer.
static const char* const kWhyBadFraction = "fraction out of range";

class EngineTracer : public RngTracer {
public:
	bool TraceHull(const float start[3], const float end[3],
	               const float mins[3], const float maxs[3], RngTraceResult& out) override
	{
		return TraceMask(start, end, mins, maxs, kMaskPlayerSolid, out);
	}

	bool TraceMask(const float start[3], const float end[3], const float mins[3], const float maxs[3],
	               unsigned int mask, RngTraceResult& out)
	{
		if (state_ != kValidateOk || !InGame()) return false;
		const int mc = MaxClients();
		if (mc < 1 || mc > 255) return false;
		maxClients_ = mc;

		TraceBufferX buf;
		PlayerFilter f;
		const char* why = nullptr;
		if (!Trace(start, end, mins, maxs, mask, true, buf, f, &why)) {
			if (why == kWhyBadFraction) return false;
			state_ = kValidateFailed;
			Logf("rngfix: engine trace failed: %s; engine trace off", why);
			return false;
		}
		const GameTraceX& t = buf.tr;
		out.fraction   = t.fraction;
		out.startSolid = t.startsolid;
		out.allSolid   = t.allsolid;
		for (int i = 0; i < 3; ++i) {
			out.endpos[i] = t.endpos[i];
			out.normal[i] = t.plane.normal[i];
		}
		return true;
	}

	// Only a grounded player gives a decisive answer. Failed lasts until the game restarts,
	// since it means the engine call doesn't match the ABI assumed here.
	ValidateResult Validate(const float origin[3], bool onGround)
	{
		if (state_ != kValidatePending) return state_;
		if (!InGame()) return kValidatePending;
		const int mc = MaxClients();
		if (mc < 1 || mc > 255) return Fail("GetMaxClients returned %d", mc);
		maxClients_ = mc;
		if (!onGround) return kValidatePending;

		const char* why = nullptr;
		TraceBufferX all, own;
		PlayerFilter fAll, fOwn;

		// A box inside the player's own hull must trace both ways and not start solid with
		// players filtered.
		const float inMins[3] = { -15, -15, 1 }, inMaxs[3] = { 15, 15, 36 };
		if (!Trace(origin, origin, inMins, inMaxs, kMaskPlayerSolid, false, all, fAll, &why)) return TraceFailed(why);
		if (!Trace(origin, origin, inMins, inMaxs, kMaskPlayerSolid, true, own, fOwn, &why)) return TraceFailed(why);
		// Stuck in something, or inconclusive: the local player may be missing from the trace
		// while something else (a dropped weapon) overlaps the box.
		if (own.tr.startsolid) return kValidatePending;
		if (Dist(own.tr.endpos, origin) > 0.05f)
			return Fail("zero-length trace ended at %.2f %.2f %.2f, not the player origin",
				own.tr.endpos[0], own.tr.endpos[1], own.tr.endpos[2]);

		const float gMins[3] = { -15.5f, -15.5f, 0 }, gMaxs[3] = { 15.5f, 15.5f, 36 };
		const float down[3] = { origin[0], origin[1], origin[2] - 64 };
		TraceBufferX g;
		PlayerFilter fg;
		if (!Trace(origin, down, gMins, gMaxs, kMaskPlayerSolid, true, g, fg, &why)) return TraceFailed(why);
		// Stuck, standing on a player, or not on walkable ground within 6 units.
		if (g.tr.startsolid || g.tr.allsolid || g.tr.fraction > 0.1f) return kValidatePending;
		if (!CheckHit(g.tr, origin, down, &why)) return Fail("ground sweep: %s", why);
		if (g.tr.plane.normal[2] < 0.7f) return kValidatePending;

		// Exercises the m_IsRay path.
		const float zero[3] = { 0, 0, 0 };
		const float up[3] = { origin[0], origin[1], origin[2] + 1 };
		TraceBufferX p;
		PlayerFilter fp;
		if (!Trace(up, down, zero, zero, kMaskPlayerSolid, true, p, fp, &why)) return TraceFailed(why);
		if (!p.tr.startsolid && p.tr.fraction < 1 && !CheckHit(p.tr, up, down, &why))
			return Fail("point ray: %s", why);

		state_ = kValidateOk;
		return state_;
	}

	void Reset()
	{
		if (state_ != kValidateFailed) state_ = kValidatePending;
	}

	bool Ready() const  { return state_ == kValidateOk; }
	bool Failed() const { return state_ == kValidateFailed; }

protected:
	virtual bool RawTrace(const RayX& ray, unsigned int mask, PlayerFilter& filter, TraceBufferX& buf) = 0;
	virtual bool InGame() = 0;
	virtual int  MaxClients() = 0;
	virtual void Log(const char* msg) = 0;
	virtual EntityHandleFn HandleFn() = 0;

	void Logf(const char* fmt, ...)
	{
		char buf[256];
		va_list ap;
		va_start(ap, fmt);
		vsnprintf(buf, sizeof(buf), fmt, ap);
		va_end(ap);
		Log(buf);
	}

private:
	bool Trace(const float start[3], const float end[3], const float mins[3], const float maxs[3],
	           unsigned int mask, bool ignorePlayers, TraceBufferX& buf, PlayerFilter& f, const char** why)
	{
		RayX ray;
		InitRay(ray, start, end, mins, maxs);
		memset(&buf, 0, sizeof(buf));
		buf.tr.fraction = -1.0f;  // sentinel: the engine always writes it
		f.handleOf      = HandleFn();
		f.ignorePlayers = ignorePlayers;
		f.maxClients    = maxClients_;
		if (!RawTrace(ray, mask, f, buf)) {
			*why = "the engine call faulted";
			return false;
		}
		const float fr = buf.tr.fraction;
		if (!(fr >= 0.0f && fr <= 1.0f)) {  // catches NaN
			*why = fr == -1.0f ? "the engine didn't write the trace" : kWhyBadFraction;
			return false;
		}
		return true;
	}

	static bool CheckHit(const GameTraceX& t, const float start[3], const float end[3], const char** why)
	{
		float want[3];
		float len = 0;
		for (int i = 0; i < 3; ++i) {
			const float d = end[i] - start[i];
			want[i] = start[i] + t.fraction * d;
			len += d * d;
		}
		if (Dist(t.endpos, want) > 0.05f + 1e-4f * sqrtf(len)) {
			*why = "endpos isn't start + fraction * delta";
			return false;
		}
		const float n = sqrtf(t.plane.normal[0] * t.plane.normal[0]
			+ t.plane.normal[1] * t.plane.normal[1] + t.plane.normal[2] * t.plane.normal[2]);
		if (!(fabsf(n - 1.0f) < 0.01f)) {
			*why = "the plane normal isn't a unit vector";
			return false;
		}
		return true;
	}

	static float Dist(const float a[3], const float b[3])
	{
		const float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
		return sqrtf(x * x + y * y + z * z);
	}

	ValidateResult Fail(const char* fmt, ...)
	{
		char buf[200];
		va_list ap;
		va_start(ap, fmt);
		vsnprintf(buf, sizeof(buf), fmt, ap);
		va_end(ap);
		state_ = kValidateFailed;
		Logf("rngfix: engine trace failed: %s", buf);
		return state_;
	}

	ValidateResult TraceFailed(const char* why)
	{
		return why == kWhyBadFraction ? kValidatePending : Fail("%s", why);
	}

	ValidateResult state_      = kValidatePending;
	int            maxClients_ = 64;
};

}
