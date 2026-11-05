#pragma once

#include "engine_trace_layout.h"

namespace etrace {

// nullptr when an interface or vtable slot isn't where expected. Not usable until
// Validate() passes; the BSP tracer stands in until then.
EngineTracer* CreateEngineTracer();

// Sees every TraceRay the game makes through the client's IEngineTrace, after it ran. Use it only once a tracer
// from CreateEngineTracer has passed Validate(), which proves the slot. On the game thread.
typedef void (*TraceWatchFn)(const RayX& ray, unsigned int mask, const GameTraceX& tr);
bool WatchTraces(TraceWatchFn fn);
#ifdef _WIN32
void UnwatchTraces();
#endif

// The entity index of a trace's hit entity, -1 if it can't be told.
int EntityIndex(void* ent);

}
