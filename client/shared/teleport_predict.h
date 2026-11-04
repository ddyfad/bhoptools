#pragma once

#include <string>

#include "rngfix_api.h"
#include "server_movement.h"
#include "zones_sim.h"

namespace tp {

bool Init();

#ifdef _WIN32
// Inside the caller's Detours transaction.
void Attach();
// Windows unhooks when the launcher exits; the Linux .so stays for the life of the game.
void Detach();
#else
bool Attach();
#endif

// False until the client module's tables are mapped.
bool ResolveNetvars();

// Game thread only: Linux calls it from Attach, Windows from the
// game's window proc so cfgs can use them before the first move.
void RegisterCommands();

// Any thread; the checkpoint predictor picks a change up on its next command.
bool Enabled();
void SetEnabled(bool on);

void SetEventQueueFix(bool on);

void SetUntouchEachCommand(bool on);

void SetMpbhopsLoaded(bool on);

// Set around the wrapped ProcessMovement: the command number it runs (the tickbase if that's
// unusable) and the usercmd number, -1 when not keyed by command number. INT_MIN and -1 outside it.
void SetRunningCommand(int key, int cmdNumber);

void SetCheckpointPrinter(void (*print)());


// From LevelInitPreEntity: a reconnect to the same map keeps the level name.
void OnLevelInit();

}

// The checkpoint predictor's ProcessMovement hook wraps the trigger sim's, so its restore
// runs first and its save after.
// After a predicted checkpoint restore, on every run of that command.
void TeleportPredict_OnCheckpointTeleport();
// Sets the view without the snap filter taking it for a server snap.
bool TeleportPredict_SetOwnViewAngles(float pitch, float yaw, float roll);
// bhoptools_tsnap: a teleport on `cmd` turned the view on the server, which sent no snap. Turns the view
// now, keeping the turn made since, unless we predicted that snap.
void TeleportPredict_ServerViewSnap(int cmd, float pitch, float yaw, float roll);
// bhoptools_log: corrections, unpredicted snaps, state sync fixes, jumps and every [bhoptools] console
// message, to bhoptools_log.txt in the game folder.
bool TeleportPredict_ToggleLog();
void TeleportPredict_LogLine(const char* line);
void TeleportPredict_PredLog(const char* fmt, ...)
#ifndef _WIN32
    __attribute__((format(printf, 1, 2)))
#endif
    ;
// The player's m_flLaggedMovementValue, networked but not predicted; null if it wasn't found.
float* TeleportPredict_LaggedMovement(void* player);
float TeleportPredict_TickInterval();
int TeleportPredict_WaterLevel(void* player);
// m_surfaceFriction, 1 if it wasn't found.
float TeleportPredict_SurfaceFriction(void* player);
// m_flStamina, 0 if it wasn't found.
float TeleportPredict_Stamina(void* player);
// The player's hull traced `distance` down, as rngfix and the landfixes trace; false when nothing can trace.
bool TeleportPredict_TraceDown(const float origin[3], float distance, bool ducked, bool onGround, bool firstRun, RngTraceResult& tr);
// Whether the move just predicted teleported the player the way the server's CBaseEntity::Teleport
// would: a trigger_teleport, or with `velocityOnly` also one of rngfix's TeleportEntity fixes that only
// set the velocity.
bool TeleportPredict_MoveTeleported(bool velocityOnly);
// Call once the wrapped ProcessMovement has returned.
void TeleportPredict_OnCheckpointSave(int cpIndex);
// Before or after OnCheckpointTeleport in the same command. `kz`: kzcheckpoints,
// whose teleport keeps touches, events and names.
void TeleportPredict_OnCheckpointLoad(int cpIndex, bool kz);

void TeleportPredict_SetRngfixSettings(const RngSettings& s);
const RngSettings& TeleportPredict_RngfixSettings();

// fromCmd: the first command they applied to, INT_MIN for all (and forget earlier ones).
void TeleportPredict_SetMoveSettings(float runspeed, int landfix, bool easybhop, int fromCmd);
void TeleportPredict_SetHazeSkip(const std::vector<int>& entities);
// The server's caps say its landfix is the SourceJump plugin's: bhoptools_move's landfix 3 is then its lift.
void TeleportPredict_SetSourceJumpLandfix(bool on);
const MoveSettings& TeleportPredict_MoveSettings();


// State sync (CAPS_STATE): the sim ignores the server's state until the checkpoint
// predictor turns it on for a server that sends it.
void TeleportPredict_SetStateSync(bool on);
// The server's state after our command `cmd`, patched into the sim's snapshots. -1 when there's
// nothing to patch, else how many fields the prediction had wrong.
int TeleportPredict_SyncEntity(int cmd, int hammerId, int flags, float value, int wait);
int TeleportPredict_SyncPlayer(int cmd, float gravity, bool baseVelFlag, const char* name, const char* cls);

void TeleportPredict_WatchZones(bool on);
void TeleportPredict_ClearZones();
void TeleportPredict_SetZoneSettings(int fromCmd, const ZsSettings& s);
// shavit-core's OnPlayerRunCmd zone work on `c`, before bhoptools' teleports. False when zones aren't
// predicted; inStart and freestyle are -1 where unknown.
bool TeleportPredict_ZonesPreCmd(void* player, int cmdNumber, ZsCmd& c, int* inStart, int* freestyle);
