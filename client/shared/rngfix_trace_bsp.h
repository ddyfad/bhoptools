#pragma once

#include <cmath>
#include <cstdint>
#include <algorithm>
#include <vector>

#include "rngfix_api.h"
#include "prediction.h"

static const int   RNG_MASK_PLAYERSOLID = 0x1 | 0x2 | 0x8 | 0x4000 | 0x10000 | 0x2000000;
static const float RNG_DIST_EPSILON     = 0.03125f;

struct RngBspSweep {
	float start[3];
	float end[3];
	float ext[3];
	float fraction = 1.0f;
	float normal[3] = {0, 0, 0};
	bool  startSolid = false;
	bool  allSolid = false;
};

// cmodel.cpp CM_ClipBoxToBrush, without fractionleftsolid.
static void RngClipBoxToBrush(const PredMap& map, const PredBrushSpan& b, RngBspSweep& t)
{
	const float NEVER_UPDATED = -9999.0f;
	float enterfrac = NEVER_UPDATED, leavefrac = 1.0f;
	bool getout = false, startout = false;
	const BspPlane* clip = nullptr;

	for (int i = 0; i < b.planeCount; ++i) {
		const BspPlane& pl = map.brushPlanes[(size_t)b.firstPlane + i];
		const float* n = pl.normal;
		float dist = fabsf(n[0]) * t.ext[0] + fabsf(n[1]) * t.ext[1] + fabsf(n[2]) * t.ext[2];
		dist = pl.dist + dist;
		const float d1 = (t.start[0] * n[0] + t.start[1] * n[1] + t.start[2] * n[2]) - dist;
		const float d2 = (t.end[0] * n[0] + t.end[1] * n[1] + t.end[2] * n[2]) - dist;

		if (d1 > 0.0f) {
			startout = true;
			if (d2 > 0.0f) return;
		} else {
			if (d2 <= 0.0f) continue;
			getout = true;
		}

		if (d1 > d2) {
			float f = d1 - RNG_DIST_EPSILON;
			if (f < 0.0f) f = 0.0f;
			f = f / (d1 - d2);
			if (f > enterfrac) { enterfrac = f; clip = &pl; }
		} else {
			const float f = (d1 + RNG_DIST_EPSILON) / (d1 - d2);
			if (f < leavefrac) leavefrac = f;
		}
	}

	if (!startout) {
		t.startSolid = true;
		if (!getout) { t.allSolid = true; t.fraction = 0.0f; }
		return;
	}

	if (enterfrac < leavefrac && enterfrac > NEVER_UPDATED && enterfrac < t.fraction) {
		if (enterfrac < 0.0f) enterfrac = 0.0f;
		t.fraction = enterfrac;
		t.normal[0] = clip->normal[0];
		t.normal[1] = clip->normal[1];
		t.normal[2] = clip->normal[2];
	}
}

class RngBspTracer : public RngTracer {
public:
	explicit RngBspTracer(const PredMap* map = nullptr) { SetMap(map); }

	void SetMap(const PredMap* map)
	{
		map_ = map;
		first_ = count_ = 0;
		bounds_.clear();
		cellStart_.clear();
		items_.clear();
		always_.clear();
		nx_ = ny_ = 0;
		if (!map || map->modelBrushes.empty()) return;
		first_ = map->modelBrushes[0].first;
		count_ = map->modelBrushes[0].second;
		if (count_ <= 0) return;

		// vbsp gives every brush all six axial planes, as sides or bevels.
		bounds_.resize((size_t)count_);
		float lo[2] = { 1e30f, 1e30f }, hi[2] = { -1e30f, -1e30f };
		for (int i = 0; i < count_; ++i) {
			const PredBrushSpan& b = map->brushes[(size_t)first_ + i];
			Box& bx = bounds_[(size_t)i];
			int found = 0;
			for (int c = 0; c < 3; ++c) { bx.mins[c] = -1e30f; bx.maxs[c] = 1e30f; }
			for (int k = 0; k < b.planeCount; ++k) {
				const BspPlane& pl = map->brushPlanes[(size_t)b.firstPlane + k];
				for (int c = 0; c < 3; ++c) {
					if (pl.normal[c] == 1.0f)  { bx.maxs[c] = pl.dist;  found |= 1 << (c * 2); }
					if (pl.normal[c] == -1.0f) { bx.mins[c] = -pl.dist; found |= 2 << (c * 2); }
				}
			}
			bx.bounded = found == 0x3f && std::isfinite(bx.mins[0]) && std::isfinite(bx.maxs[0])
				&& std::isfinite(bx.mins[1]) && std::isfinite(bx.maxs[1]);
			if (bx.bounded) {
				lo[0] = fminf(lo[0], bx.mins[0]); lo[1] = fminf(lo[1], bx.mins[1]);
				hi[0] = fmaxf(hi[0], bx.maxs[0]); hi[1] = fmaxf(hi[1], bx.maxs[1]);
			}
		}

		if (lo[0] > hi[0]) {
			for (int i = 0; i < count_; ++i) always_.push_back(i);
		} else {
			x0_ = (int)floorf(lo[0] / cell_);
			y0_ = (int)floorf(lo[1] / cell_);
			nx_ = (int)floorf(hi[0] / cell_) - x0_ + 1;
			ny_ = (int)floorf(hi[1] / cell_) - y0_ + 1;
			if ((long long)nx_ * ny_ > 4000000) {
				nx_ = ny_ = 0;
				for (int i = 0; i < count_; ++i) always_.push_back(i);
			} else {
				std::vector<std::vector<int>> cells((size_t)nx_ * ny_);
				for (int i = 0; i < count_; ++i) {
					const Box& bx = bounds_[(size_t)i];
					if (!bx.bounded) { always_.push_back(i); continue; }
					const int cx0 = (int)floorf(bx.mins[0] / cell_) - x0_, cx1 = (int)floorf(bx.maxs[0] / cell_) - x0_;
					const int cy0 = (int)floorf(bx.mins[1] / cell_) - y0_, cy1 = (int)floorf(bx.maxs[1] / cell_) - y0_;
					if ((long long)(cx1 - cx0 + 1) * (cy1 - cy0 + 1) > 64) { always_.push_back(i); continue; }
					for (int y = cy0; y <= cy1; ++y)
						for (int x = cx0; x <= cx1; ++x) cells[(size_t)y * nx_ + x].push_back(i);
				}
				cellStart_.resize(cells.size() + 1);
				for (size_t c = 0; c < cells.size(); ++c) {
					cellStart_[c] = (int)items_.size();
					items_.insert(items_.end(), cells[c].begin(), cells[c].end());
				}
				cellStart_[cells.size()] = (int)items_.size();
			}
		}
		stamp_.assign((size_t)count_, 0u);
		stampNow_ = 0;
	}

	bool Ready() const { return map_ && count_ > 0; }

	bool TraceHull(const float start[3], const float end[3], const float mins[3], const float maxs[3],
		RngTraceResult& out) override
	{
		out = RngTraceResult();
		if (!Ready()) return false;
		for (int c = 0; c < 3; ++c)
			if (!std::isfinite(start[c]) || !std::isfinite(end[c])) return false;

		// Same as Ray_t::Init: sweep the box centre.
		RngBspSweep t;
		float offset[3], delta[3];
		for (int c = 0; c < 3; ++c) {
			t.ext[c] = (maxs[c] - mins[c]) * 0.5f;
			offset[c] = (mins[c] + maxs[c]) * 0.5f;
			delta[c] = end[c] - start[c];
			t.start[c] = start[c] + offset[c];
			t.end[c] = t.start[c] + delta[c];
		}

		float lo[3], hi[3];
		for (int c = 0; c < 3; ++c) {
			lo[c] = fminf(start[c], end[c]) + mins[c] - 1.0f;
			hi[c] = fmaxf(start[c], end[c]) + maxs[c] + 1.0f;
		}
		Gather(lo, hi);

		const PredMap& map = *map_;
		for (int i : cand_) {
			const PredBrushSpan& b = map.brushes[(size_t)first_ + i];
			if (!(b.contents & RNG_MASK_PLAYERSOLID)) continue;
			const Box& bx = bounds_[(size_t)i];
			if (bx.bounded) {
				bool miss = false;
				for (int c = 0; c < 3 && !miss; ++c) miss = hi[c] < bx.mins[c] || lo[c] > bx.maxs[c];
				if (miss) continue;
			}
			RngClipBoxToBrush(map, b, t);
			if (t.allSolid) break; // CM_TraceToLeaf stops here too
		}

		out.fraction = t.fraction;
		out.startSolid = t.startSolid;
		out.allSolid = t.allSolid;
		for (int c = 0; c < 3; ++c) {
			out.normal[c] = t.normal[c];
			const float centre = t.fraction == 1.0f ? t.start[c] + delta[c] : t.start[c] + t.fraction * delta[c];
			out.endpos[c] = centre - offset[c];
		}
		return true;
	}

private:
	struct Box { float mins[3]; float maxs[3]; bool bounded; };

	// Ascending brush order, so equal fractions resolve the same way every time.
	void Gather(const float lo[3], const float hi[3])
	{
		cand_.clear();
		if (++stampNow_ == 0) { std::fill(stamp_.begin(), stamp_.end(), 0u); stampNow_ = 1; }
		auto add = [&](int i) { if (stamp_[(size_t)i] != stampNow_) { stamp_[(size_t)i] = stampNow_; cand_.push_back(i); } };
		if (nx_ > 0) {
			const float fx0 = floorf(lo[0] / cell_) - x0_, fx1 = floorf(hi[0] / cell_) - x0_;
			const float fy0 = floorf(lo[1] / cell_) - y0_, fy1 = floorf(hi[1] / cell_) - y0_;
			const int cx0 = (int)fmaxf(fx0, 0.0f), cx1 = (int)fminf(fx1, (float)(nx_ - 1));
			const int cy0 = (int)fmaxf(fy0, 0.0f), cy1 = (int)fminf(fy1, (float)(ny_ - 1));
			for (int y = cy0; y <= cy1; ++y)
				for (int x = cx0; x <= cx1; ++x) {
					const size_t c = (size_t)y * nx_ + x;
					for (int k = cellStart_[c]; k < cellStart_[c + 1]; ++k) add(items_[(size_t)k]);
				}
		}
		for (int i : always_) add(i);
		std::sort(cand_.begin(), cand_.end());
	}

	const PredMap*    map_ = nullptr;
	int               first_ = 0, count_ = 0;
	std::vector<Box>  bounds_;
	float             cell_ = 256.0f;
	int               x0_ = 0, y0_ = 0, nx_ = 0, ny_ = 0;
	std::vector<int>  cellStart_, items_, always_;
	std::vector<int>  cand_;
	std::vector<uint32_t> stamp_;
	uint32_t          stampNow_ = 0;
};
