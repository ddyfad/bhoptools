// rawinput2 precise: raw mouse reports stamped on their own thread, so each tick gets its own slice.
#pragma once

#include <Windows.h>

namespace rawprecise {

void Init(double (*platFloatTime)());
// Inside a Detours transaction.
void Attach();
void Detach();
// From the unloading thread, before the hooks come off.
void Shutdown();

// True for Shutdown's message, which the caller should not pass on.
bool OnWindowMessage(HWND hwnd, UINT msg);
// Once a frame. drain empties the game thread's raw queue before switching.
void Update(bool rawActive, int (*drain)());
bool On();
// Hands the buttons and wheel since the last call to pollfix, returns the reports.
int FlushButtons();
// Movement up to split (Plat_FloatTime), or all of it when split is 0.
void Take(int& dx, int& dy, double split);
void RegisterCommands();

}
