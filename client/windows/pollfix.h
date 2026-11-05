// High polling rate fix: the engine's message pump filtered the way Godot and PH3 do it, plus
// counters for what the pump costs (bhoptools_pumpstats). Game thread only, apart from Init.
#pragma once

#include <Windows.h>

namespace pollfix {

// drain reads the raw input queue into the engine's accumulators and returns the reports read;
// without one the fix stays unavailable and only the counters run.
void Init(int (*drain)());
// Inside a Detours transaction.
void Attach();
void Detach();

// From CInputSystem::WindowProc: learns the game's thread and window, counts the message.
void OnWindowMessage(HWND hwnd, UINT msg);
void CountRaw(int reports);
void RegisterCommands();

}
