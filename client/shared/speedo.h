#pragma once

#include <cstddef>

// old_bf_read (64-bit), checked against CUserMessages::DispatchUserMessage.
struct BfRead {
	const unsigned char* data;
	int         bytes;
	int         bits;
	int         curBit;
	bool        overflow;
	bool        assertOnOverflow;
	const char* debugName;
};
static_assert(sizeof(BfRead) == 32 && offsetof(BfRead, bits) == 0xc && offsetof(BfRead, curBit) == 0x10, "bf_read layout");

namespace speedo {

typedef bool (*DispatchUserMessageFn)(void*, int, BfRead*);

// Main thread. `client` is CHLClient (VClient017).
void Attach(void* client);
#ifdef _WIN32
// Windows unhooks when the launcher exits; the Linux .so stays for the life of the game.
void Detach();
#endif

DispatchUserMessageFn OriginalDispatch();

void Watch(const char* name, void (*fn)(const BfRead&));

// The watched message's id, -1 until one has been dispatched.
int WatchedMessage();

// A user message's id, -1 if it isn't registered (yet).
int UserMessageId(const char* name);
// Hands the client a user message as if the server had sent it, past our own hook.
void Dispatch(int id, const void* data, int len);

// hud_speed and hud_keys. On the thread that runs console commands.
void RegisterCommands();

// Set by prediction; only the speed needs it.
void SetEnabled(bool on);

void OnMove(bool alive, const float velocity[3], bool firstRun);

void SetHeld(bool held);

// After CInput::CreateMove stored the command for `sequence`.
void OnCommand(void* input, int sequence);

}
