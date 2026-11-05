// Main thread only.
#pragma once

struct MouseSplit {
	int    accumX = 0;
	int    accumY = 0;
	double sampleTime = 0.0;
	double splitTime  = -1.0; // where the last consumer stopped; -1 = never
};

inline void MouseSplitAdd(MouseSplit& m, int dx, int dy, double now)
{
	m.accumX += dx;
	m.accumY += dy;
	m.sampleTime = now;
}

inline void MouseSplitClear(MouseSplit& m)
{
	m.accumX = m.accumY = 0;
}

// Assumes the motion was linear between the last split and the newest event; split == 0 takes all.
inline void MouseSplitTake(MouseSplit& m, int& outX, int& outY, double split)
{
	if (m.splitTime < 0.0) m.splitTime = m.sampleTime - 0.01;

	const double dt = m.splitTime - m.sampleTime;
	if ((dt < 0 ? -dt : dt) < 0.000001) {
		outX = outY = 0;
		return;
	}

	if (split == 0.0 || split >= m.sampleTime) {
		outX = m.accumX;
		outY = m.accumY;
		m.accumX = m.accumY = 0;
		m.splitTime = m.sampleTime;
		return;
	}

	if (split >= m.splitTime) {
		const float seg = (float)((split - m.splitTime) / (m.sampleTime - m.splitTime));
		outX = (int)(seg * (float)m.accumX);
		outY = (int)(seg * (float)m.accumY);
		m.accumX -= outX;
		m.accumY -= outY;
		m.splitTime = split;
		return;
	}

	outX = outY = 0;
}
