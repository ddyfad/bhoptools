#pragma once

namespace cp {

// Static data in the client module only, so Linux runs it on the installer thread.
bool ResolveOffsets();

// Wraps the trigger sim's ProcessMovement hook, so on Linux tp::Attach() has to run first.
bool Attach();
#ifdef _WIN32
// Windows unhooks when the launcher exits; the Linux .so stays for the life of the game.
void Detach();
#endif

void OnLevelInit();

void SetTriggerPrediction(bool on);

void PrintState();

void AddInput(float dx, float dy);
void OnCreateMove(int sequence);

}
