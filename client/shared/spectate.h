// Smooth first-person spectating, from zastools. The watched player is always kept on the client's
// interpolation list and drawn at least 100 ms back.
#pragma once

namespace spectate {

// Game thread. False until the client's player class is found; call again later.
bool Install();
#ifdef _WIN32
void Uninstall();
#endif

}
