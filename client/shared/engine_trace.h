#pragma once

#include "engine_trace_layout.h"

namespace etrace {

// nullptr when an interface or vtable slot isn't where expected. Not usable until
// Validate() passes; the BSP tracer stands in until then.
EngineTracer* CreateEngineTracer();

}
