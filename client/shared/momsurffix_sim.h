#pragma once

#include "engine_trace_layout.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <xmmintrin.h>

// bhoptools_msf. Unknown means stock TryPlayerMove.
struct MsfSettings {
	bool  known            = false;
	int   bumpcount        = 8;
	float retrace          = 0.2f;
	bool  noclipWorkaround = true;
	bool  windowsServer    = false;
};

struct MsfMove {
	float* origin;
	float* velocity;
	float  frametime;
	bool   onGround;        // m_hGroundEntity isn't -1; the move doesn't change it
	bool   walking;
	float  surfaceFriction;
	float  bounce;
};

class MsfWorld {
public:
	virtual ~MsfWorld() {}
	// The player's hull grown to mins - subMins, maxs + addMaxs when those aren't null.
	virtual void Trace(const float start[3], const float end[3], const float* subMins, const float* addMaxs,
	                   etrace::GameTraceX& tr) = 0;
	virtual void AddToTouched(const etrace::GameTraceX& tr, float* velocity) = 0;
};

static const float kMsfEpsilon = 1.192092896e-07f;

inline bool MsfIsEqual(const float a[3], const float b[3]) { return a[0] == b[0] && a[1] == b[1] && a[2] == b[2]; }

inline bool MsfCloseEnough(const float a[3], const float b[3])
{
	return fabsf(a[0] - b[0]) <= kMsfEpsilon && fabsf(a[1] - b[1]) <= kMsfEpsilon && fabsf(a[2] - b[2]) <= kMsfEpsilon;
}

inline bool MsfCloseEnough(float a, float b) { return fabsf(a - b) <= kMsfEpsilon; }

inline float MsfDot(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

inline void MsfCopy(const float from[3], float to[3]) { to[0] = from[0]; to[1] = from[1]; to[2] = from[2]; }

inline void MsfZero(float v[3]) { v[0] = v[1] = v[2] = 0.0f; }

inline void MsfMA(const float start[3], float scale, const float dir[3], float out[3])
{
	out[0] = start[0] + dir[0] * scale;
	out[1] = start[1] + dir[1] * scale;
	out[2] = start[2] + dir[2] * scale;
}

inline void MsfNormalize(const float v[3], float out[3])
{
	const float s = v[0] * v[0] + v[1] * v[1] + v[2] * v[2] + 1.0e-10f;
	const float r = _mm_cvtss_f32(_mm_rsqrt_ss(_mm_set_ss(s)));
	const float inv = ((3.0f - (r * r) * s) * 0.5f) * r;
	out[0] = v[0] * inv;
	out[1] = v[1] * inv;
	out[2] = v[2] * inv;
}

inline void MsfCross(const float a[3], const float b[3], float out[3])
{
	const float x = a[1] * b[2] - a[2] * b[1];
	const float y = a[2] * b[0] - a[0] * b[2];
	const float z = a[0] * b[1] - a[1] * b[0];
	out[0] = x; out[1] = y; out[2] = z;
}

// `in` may be `out`.
inline int MsfClipVelocityEngine(const float in[3], const float n[3], float out[3], float overbounce)
{
	const float angle = n[2];
	int blocked = angle > 0.0f ? 1 : 0;
	if (angle == 0.0f) blocked |= 2;
	const float backoff = (in[0] * n[0] + in[1] * n[1] + in[2] * n[2]) * overbounce;
		const float into = -backoff > 0.0f ? -backoff : 0.0f;
	const float push = 0.03125f + into;
	const float o[3] = { n[0] * push + in[0], n[1] * push + in[1], n[2] * push + in[2] };
	const float adjust = o[0] * n[0] + o[1] * n[1] + o[2] * n[2];
	out[0] = o[0]; out[1] = o[1]; out[2] = o[2];
	if (adjust < 0.0f) {
		out[0] = o[0] - n[0] * adjust;
		out[1] = o[1] - n[1] * adjust;
		out[2] = o[2] - n[2] * adjust;
	}
	return blocked;
}

inline int MsfClipVelocityPlugin(const float in[3], const float n[3], float out[3], float overbounce)
{
	const float angle = n[2];
	int blocked = 0;
	if (angle > 0.0f) blocked |= 1;
	if (MsfCloseEnough(angle, 0.0f)) blocked |= 2;
	const float backoff = MsfDot(in, n) * overbounce;
	out[0] = in[0] - n[0] * backoff;
	out[1] = in[1] - n[1] * backoff;
	out[2] = in[2] - n[2] * backoff;
	const float adjust = MsfDot(out, n);
	if (adjust < 0.0f) {
		out[0] -= n[0] * adjust;
		out[1] -= n[1] * adjust;
		out[2] -= n[2] * adjust;
	}
	return blocked;
}

inline int MsfClipVelocity(const MsfSettings& s, const float in[3], const float n[3], float out[3], float overbounce)
{
	return s.windowsServer ? MsfClipVelocityPlugin(in, n, out, overbounce) : MsfClipVelocityEngine(in, n, out, overbounce);
}

inline bool MsfIsValidMovementTrace(MsfWorld& w, const etrace::GameTraceX& tr)
{
	if (tr.allsolid || tr.startsolid) return false;
	if (MsfCloseEnough(tr.fraction, 0.0f)) return false;
	const float* n = tr.plane.normal;
	if (fabsf(n[0]) > 1.0f || fabsf(n[1]) > 1.0f || fabsf(n[2]) > 1.0f) return false;
	etrace::GameTraceX stuck;
	w.Trace(tr.endpos, tr.endpos, nullptr, nullptr, stuck);
	return !stuck.startsolid && MsfCloseEnough(stuck.fraction, 1.0f);
}

// firstDest/firstTrace are the caller's (WalkMove's first trace, through StepMove); like the plugin, a
// bump whose end matches firstDest takes that trace itself as pm, and later traces then write into it.
inline int MsfTryPlayerMove(MsfWorld& w, const MsfSettings& s, const MsfMove& m,
                            const float* firstDest, etrace::GameTraceX* firstTrace)
{
	static const float zero[3] = { 0.0f, 0.0f, 0.0f };
	const int kMaxClipPlanes = 5;

	float original_velocity[3], primal_velocity[3], fixed_origin[3];
	float valid_plane[3] = { 0, 0, 0 }, new_velocity[3] = { 0, 0, 0 }, end[3] = { 0, 0, 0 }, dir[3] = { 0, 0, 0 };
	float allFraction = 0.0f, d = 0.0f, time_left = m.frametime, planes[kMaxClipPlanes][3] = {};
	int bumpcount, blocked = 0, numplanes = 0, numbumps = s.bumpcount, i, j, h;
	bool stuck_on_ramp = false, has_valid_plane = false;

	etrace::GameTraceX own;
	memset(&own, 0, sizeof(own));
	etrace::GameTraceX* pm = &own;
	float* vel = m.velocity;
	float* origin = m.origin;

	MsfCopy(vel, original_velocity);
	MsfCopy(vel, primal_velocity);
	MsfCopy(origin, fixed_origin);

	for (bumpcount = 0; bumpcount < numbumps; bumpcount++) {
		if (MsfDot(vel, vel) == 0.0f) break;

		if (stuck_on_ramp) {
			if (!has_valid_plane) {
				const float* plane_normal = pm->plane.normal;
				if (!MsfCloseEnough(plane_normal, zero) && !MsfIsEqual(valid_plane, plane_normal)) {
					MsfCopy(plane_normal, valid_plane);
					has_valid_plane = true;
				} else {
					for (i = numplanes; i-- > 0;) {
						if (!MsfCloseEnough(planes[i], zero) && fabsf(planes[i][0]) <= 1.0f && fabsf(planes[i][1]) <= 1.0f
								&& fabsf(planes[i][2]) <= 1.0f && !MsfIsEqual(valid_plane, planes[i])) {
							MsfCopy(planes[i], valid_plane);
							has_valid_plane = true;
							break;
						}
					}
				}
			}

			if (has_valid_plane) {
				if (valid_plane[2] >= 0.7f && valid_plane[2] <= 1.0f)
					MsfClipVelocity(s, vel, valid_plane, vel, 1.0f);
				else
					MsfClipVelocity(s, vel, valid_plane, vel, 1.0f + m.bounce * (1.0f - m.surfaceFriction));
				MsfCopy(vel, original_velocity);
			}
			// The plugin's workaround for its issue #1 (noclip): no retrace while barely falling.
			else if (!s.noclipWorkaround || (vel[2] < -6.25f || vel[2] > 0.0f)) {
				const float offsets[3] = { ((float)bumpcount * 2.0f) * -s.retrace, 0.0f, ((float)bumpcount * 2.0f) * s.retrace };
				int valid_planes = 0;
				MsfZero(valid_plane);

				for (i = 0; i < 3; i++) {
					for (j = 0; j < 3; j++) {
						for (h = 0; h < 3; h++) {
							const float offset[3] = { offsets[i], offsets[j], offsets[h] };
							float offset_mins[3], offset_maxs[3], buff[3], to[3];
							for (int k = 0; k < 3; k++) {
								offset_mins[k] = offset[k] * 0.5f;
								offset_maxs[k] = offset[k] * 0.5f;
							}
							for (int k = 0; k < 3; k++)
								if (offset[k] > 0.0f) offset_mins[k] /= 2.0f;
							for (int k = 0; k < 3; k++)
								if (offset[k] < 0.0f) offset_maxs[k] /= 2.0f;
							for (int k = 0; k < 3; k++) {
								buff[k] = fixed_origin[k] + offset[k];
								to[k] = end[k] - offset[k];
							}

							w.Trace(buff, to, offset_mins, offset_maxs, *pm);

							const float* plane_normal = pm->plane.normal;
							if (fabsf(plane_normal[0]) <= 1.0f && fabsf(plane_normal[1]) <= 1.0f && fabsf(plane_normal[2]) <= 1.0f
									&& pm->fraction > 0.0f && pm->fraction < 1.0f && !pm->startsolid) {
								valid_planes++;
								for (int k = 0; k < 3; k++) valid_plane[k] = valid_plane[k] + plane_normal[k];
							}
						}
					}
				}

				if (valid_planes != 0 && !MsfCloseEnough(valid_plane, zero)) {
					has_valid_plane = true;
					MsfNormalize(valid_plane, valid_plane);
					continue;
				}
			}

			if (has_valid_plane) {
				MsfMA(fixed_origin, s.retrace, valid_plane, fixed_origin);
			} else {
				stuck_on_ramp = false;
				continue;
			}
		}

		MsfMA(fixed_origin, time_left, vel, end);

		if (firstDest && MsfIsEqual(end, firstDest)) {
			pm = firstTrace;
		} else if (stuck_on_ramp && has_valid_plane) {
			w.Trace(fixed_origin, end, nullptr, nullptr, *pm);
			MsfCopy(valid_plane, pm->plane.normal);
		} else {
			w.Trace(origin, end, nullptr, nullptr, *pm);
		}

		if (bumpcount > 0 && !m.onGround && !MsfIsValidMovementTrace(w, *pm)) {
			has_valid_plane = false;
			stuck_on_ramp = true;
			continue;
		}

		if (pm->fraction > 0.0f) {
			if ((bumpcount == 0 || m.onGround) && numbumps > 0 && pm->fraction == 1.0f) {
				etrace::GameTraceX stuck;
				w.Trace(pm->endpos, pm->endpos, nullptr, nullptr, stuck);
				const bool isStuck = stuck.startsolid || stuck.fraction != 1.0f;
				if (isStuck && bumpcount == 0) {
					has_valid_plane = false;
					stuck_on_ramp = true;
					continue;
				} else if (isStuck) {
					MsfZero(vel);
					break;
				}
			}

			has_valid_plane = false;
			stuck_on_ramp = false;

			MsfCopy(vel, original_velocity);
			MsfCopy(pm->endpos, origin);
			MsfCopy(origin, fixed_origin);
			allFraction += pm->fraction;
			numplanes = 0;
		}

		if (MsfCloseEnough(pm->fraction, 1.0f)) break;

		w.AddToTouched(*pm, vel);

		if (pm->plane.normal[2] >= 0.7f) blocked |= 1;
		if (MsfCloseEnough(pm->plane.normal[2], 0.0f)) blocked |= 2;

		time_left -= time_left * pm->fraction;

		if (numplanes >= kMaxClipPlanes) {
			MsfZero(vel);
			break;
		}

		MsfCopy(pm->plane.normal, planes[numplanes]);
		numplanes++;

		if (numplanes == 1 && m.walking && m.onGround) {
			float clipped[3];
			if (planes[0][2] >= 0.7f) {
				MsfClipVelocity(s, original_velocity, planes[0], clipped, 1.0f);
				MsfCopy(clipped, original_velocity);
				MsfCopy(clipped, new_velocity);
			} else {
				MsfClipVelocity(s, original_velocity, planes[0], clipped, 1.0f + m.bounce * (1.0f - m.surfaceFriction));
				MsfCopy(clipped, new_velocity);
			}
			MsfCopy(new_velocity, vel);
			MsfCopy(new_velocity, original_velocity);
		} else {
			for (i = 0; i < numplanes; i++) {
				MsfClipVelocity(s, original_velocity, planes[i], vel, 1.0f);
				for (j = 0; j < numplanes; j++)
					if (j != i && MsfDot(vel, planes[j]) < 0.0f) break;
				if (j == numplanes) break;
			}

			if (i == numplanes) {
				if (numplanes != 2) {
					MsfZero(vel);
					break;
				}

				if (MsfCloseEnough(planes[0], planes[1])) {
					// Only x and y: the plugin leaves z as the last clip made it.
					MsfMA(original_velocity, 20.0f, planes[0], new_velocity);
					vel[0] = new_velocity[0];
					vel[1] = new_velocity[1];
					break;
				}

				MsfCross(planes[0], planes[1], dir);
				MsfNormalize(dir, dir);
				d = MsfDot(vel, dir);
				for (int k = 0; k < 3; k++) dir[k] *= d;
				MsfCopy(dir, vel);
			}

			d = MsfDot(vel, primal_velocity);
			if (d <= 0.0f) {
				MsfZero(vel);
				break;
			}
		}
	}

	if (MsfCloseEnough(allFraction, 0.0f)) MsfZero(vel);
	return blocked;
}

// CreateStuckTable's nudges, in its order; the 54th entry stays zero.
inline void MsfStuckOffset(int i, float out[3])
{
	static float table[54][3];
	static bool built = false;
	if (!built) {
		int n = 0;
		const auto put = [&](float x, float y, float z) { table[n][0] = x; table[n][1] = y; table[n][2] = z; ++n; };
		for (float z = -0.125f; z <= 0.125f; z += 0.125f) put(0, 0, z);
		for (float y = -0.125f; y <= 0.125f; y += 0.125f) put(0, y, 0);
		for (float x = -0.125f; x <= 0.125f; x += 0.125f) put(x, 0, 0);
		for (float x = -0.125f; x <= 0.125f; x += 0.25f)
			for (float y = -0.125f; y <= 0.125f; y += 0.25f)
				for (float z = -0.125f; z <= 0.125f; z += 0.25f) put(x, y, z);
		const float zi[3] = { 0.0f, 1.0f, 6.0f };
		for (float z : zi) put(0, 0, z);
		for (float y = -2.0f; y <= 2.0f; y += 2.0f) put(0, y, 0);
		for (float x = -2.0f; x <= 2.0f; x += 2.0f) put(x, 0, 0);
		for (float z : zi)
			for (float x = -2.0f; x <= 2.0f; x += 2.0f)
				for (float y = -2.0f; y <= 2.0f; y += 2.0f) put(x, y, z);
		built = true;
	}
	MsfCopy(table[i], out);
}

struct MsfStuckState {
	int   last      = 0;
	float checkTime = 0.0f;
};

class MsfStuckWorld {
public:
	virtual ~MsfStuckWorld() {}
	virtual bool Blocked(const float pos[3], etrace::GameTraceX& tr) = 0;
	virtual void AddToTouched(const etrace::GameTraceX& tr, float* velocity) = 0;
};

inline int MsfCheckStuck(MsfStuckWorld& w, MsfStuckState& s, float* origin, float* velocity, float now)
{
	etrace::GameTraceX tr;
	if (!w.Blocked(origin, tr)) {
		s.last = 0;
		return 0;
	}
	const float base[3] = { origin[0], origin[1], origin[2] };
	if ((double)s.checkTime >= (double)now - 0.05) return 1;
	s.checkTime = now;
	w.AddToTouched(tr, velocity);
	float offset[3];
	MsfStuckOffset(s.last++ % 54, offset);
	const float test[3] = { base[0] + offset[0], base[1] + offset[1], base[2] + offset[2] };
	etrace::GameTraceX tr2;
	if (w.Blocked(test, tr2)) return 1;
	s.last = 0;
	MsfCopy(test, origin);
	return 0;
}

// Finds IMoveHelper::sm_pSingleton in TryPlayerMove's MoveHelper()->AddToTouched call: a
// RIP-relative mov or lea (MSVC: mov rcx,[rip+d]; GCC: lea rax,[rip+d] then a load through it),
// then a call through vtable slot 2 ([reg+10h]) before any other call. `target` is the variable's
// offset from `code`.
inline bool MsfFindMoveHelperRef(const uint8_t* code, size_t len, ptrdiff_t* target)
{
	for (size_t i = 0; i + 7 <= len; ++i) {
		if ((code[i] != 0x48 && code[i] != 0x4C) || (code[i + 1] != 0x8B && code[i + 1] != 0x8D)) continue;
		if ((code[i + 2] & 0xC7) != 0x05) continue;
		int32_t disp;
		memcpy(&disp, code + i + 3, 4);
		const size_t next = i + 7;
		for (size_t k = next; k < next + 40 && k + 3 <= len; ++k) {
			if (code[k] == 0xE8) break;
			if (code[k] != 0xFF || ((code[k + 1] >> 3) & 7) != 2) continue;
			if ((code[k + 1] & 0xF8) == 0x50 && (code[k + 1] & 7) != 4 && code[k + 2] == 0x10) {
				*target = (ptrdiff_t)next + disp;
				return true;
			}
			break;
		}
	}
	return false;
}

inline bool MsfHasVcall(const uint8_t* code, size_t len, int disp)
{
	for (size_t i = 0; i + 3 <= len; ++i) {
		if (code[i] != 0xFF) continue;
		const uint8_t modrm = code[i + 1];
		if (((modrm >> 3) & 7) != 2) continue;
		size_t at = i + 2;
		if ((modrm & 7) == 4) ++at;  // SIB
		if ((modrm & 0xC0) == 0x40 && at < len && (int)(int8_t)code[at] == disp) return true;
		if ((modrm & 0xC0) == 0x80 && at + 4 <= len) {
			int32_t d32;
			memcpy(&d32, code + at, 4);
			if (d32 == disp) return true;
		}
	}
	return false;
}
