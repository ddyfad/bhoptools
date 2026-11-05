// The player's settings, kept in a small key=value file between launches: %APPDATA%\bhoptools on
// Windows, $XDG_CONFIG_HOME/bhoptools (~/.config) on Linux; bhoptools-dev for the dev build. Game thread only.
#pragma once

#include <functional>
#include <string>

struct Settings {
	bool rawinput  = true;
	bool predict   = true;
	bool viewpunch = true;  // true removes it
	bool fastdl    = true;
	bool speedo    = true;
	bool hudkeys   = true;
	bool jsframe   = false;  // client-drawn compact trainers redrawn every frame instead of every tick
	bool chatglyphs = true;  // Linux only
	bool pollfix   = true;   // Windows only: no legacy mouse messages in game, the message pump filtered in menus
};

namespace settings {

Settings& Get();
void Load();
// Applies a change made to Get() through the OS side's hook, and saves it.
void Changed();
void SetApply(std::function<void()> fn);
// The folder settings.cfg lives in, empty if there's none.
std::string Dir();

// Mouse interpolation decides m_rawinput: on makes it 1 if it's 0, off makes it 0. Called on
// each level load, after config.cfg has set it, and on every change. With nothing saved yet the
// setting takes m_rawinput's value at the first level load.
void ApplyRawInput(bool levelLoad);

}
