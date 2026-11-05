// High polling rate fix: the engine's message pump filtered the way Godot and PH3 do it, plus
// counters for what the pump costs (bhoptools_pumpstats). Game thread only, apart from Init.
#pragma once

#include <Windows.h>

namespace pollfix {

// drain reads the raw input queue into the engine's accumulators and returns the reports read;
// without one the fix stays unavailable and only the counters run.
// rawActive says every mouse report goes through OnRawMouse, which no-legacy mode needs for buttons.
void Init(int (*drain)(), bool (*rawActive)());
// Inside a Detours transaction.
void Attach();
void Detach();

// From CInputSystem::WindowProc: learns the game's thread and window, counts the message.
void OnWindowMessage(HWND hwnd, UINT msg);
void CountRaw(int reports);
// Every raw mouse report the game takes: in no-legacy mode its buttons and wheel are posted
// to the game window, since Windows no longer sends them as messages.
void OnRawMouse(const RAWMOUSE& m);
void RegisterCommands();

}
