// rawinput2 precise: the mouse's raw input on a thread of ours, each report stamped as it arrives,
// so each tick takes exactly the movement from its own slice of time instead of a share of the
// frame's spread evenly. Game thread only, apart from Init and Shutdown.
#pragma once

#include <Windows.h>

namespace rawprecise {

// Starts the input thread. platFloatTime is the engine's clock the tick splits are given in.
void Init(double (*platFloatTime)());
// Inside a Detours transaction: RegisterRawInputDevices, so the game registering the mouse again
// while precise is on hands it to our window.
void Attach();
void Detach();
// From the unloading thread, before the hooks come off: the game thread gives the mouse back to
// the game's window, then the input thread stops.
void Shutdown();

// From CInputSystem::WindowProc. True for Shutdown's message, which the caller should not pass on.
bool OnWindowMessage(HWND hwnd, UINT msg);
// Once a frame: switches between our thread and the game's window when the setting or rawActive
// (m_rawinput, raw input supported) changes. drain empties the game thread's queue before the switch.
void Update(bool rawActive, int (*drain)());
// The mouse goes to our thread.
bool On();
// Passes the buttons and wheel that arrived since the last call to pollfix, returns the reports.
int FlushButtons();
// The movement up to split (Plat_FloatTime seconds), or all of it when split is 0.
void Take(int& dx, int& dy, double split);
void RegisterCommands();

}
