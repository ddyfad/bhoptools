#pragma once

namespace options {

// Game thread. False until GameUI is loaded; call again later.
bool Install();
#ifdef _WIN32
void Uninstall();
#endif

}
