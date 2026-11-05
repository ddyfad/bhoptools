#pragma once
#include <cstddef>

// client.so CInput::GetAccumulatedMouseDeltasAndResetAccumulators.
static const char SIG_GetAccumulatedMouseDeltas[] =
	"55 48 89 E5 41 55 41 54 53 48 89 FB 48 83 EC 18 F3 0F 10 47 0C F3 0F 11 06";

// HOOK_COPY_* is the length of whole prologue instructions with no RIP-relative operands.
static const size_t HOOK_COPY_GetAccumulatedMouseDeltas = 6;

static const int VT_CInput_CreateMove       = 3;  // (int sequence, bool active, float input_sample_frametime)
static const int VT_CInput_ExtraMouseSample = 4;  // (bool active, float frametime)
static const int VT_CInput_IN_SetSampleTime = 16; // (float frametime), host_frametime once per render frame

// client.so CGameMovement::PlayerRoughLandingEffects body, punch store; vtable index and field offsets wildcarded.
static const char SIG_RoughLanding_PunchStore[] =
	"FF 90 ? ? 00 00 49 8B 5C 24 08 66 0F EF C0 66 0F EF C9 F3 0F 5A 83 ? ? 00 00";
static const size_t OFF_RoughLanding_PatchSite = 6;

// Same body's epilogue, anchored on the PITCH clamp compare so a neighbouring epilogue can't match.
static const char SIG_RoughLanding_Epilogue[] = "0F 2F 05 ? ? ? ? 77 ? 48 83 C4 08 5B 41 5C 41 5D 5D C3";
static const size_t OFF_RoughLanding_Epilogue = 9;

// DT_Local m_vecPunchAngle RecvProp, found via a pointer to its name string.
static const size_t RecvProp_SIZE = 0x60;
static const ptrdiff_t OFF_RecvProp_m_RecvType  = 0x08;
static const ptrdiff_t OFF_RecvProp_m_ProxyFn   = 0x30;
static const ptrdiff_t OFF_RecvProp_m_nElements = 0x50;
static const int       DPT_Vector                = 2;

static const char SIG_CDownloadManager_UpdateProgressBar[] =
	"48 8B 4F 28 48 85 C9 0F 84 ? ? ? ? 55 48 89 E5 41 54 53 48 89 FB 48 81 EC 30 04 00 00";
static const size_t HOOK_COPY_CDownloadManager_UpdateProgressBar = 7;
static const ptrdiff_t OFF_CDownloadManager_m_activeRequest = 0x28;

// RequestContext_t: a sixth char[256] (cachedTimestamp) shifts byte counters to +0x618.
static const ptrdiff_t OFF_RequestContext_bAsHTTP       = 0x003;
static const ptrdiff_t OFF_RequestContext_nBytesTotal   = 0x618;
static const ptrdiff_t OFF_RequestContext_nBytesCurrent = 0x61C;

// Host_FilterTime's maxsd of host_frametime against MIN_FRAMETIME (0.001). It NOPs out.
static const char SIG_HostFilterTime_MinFrametime[] = "F2 0F 5F 0D ? ? ? ? 66 0F EF C0 F2 0F 5A C1 EB";

// CEngineVGui::UpdateCustomProgressBar vtable thunk, which holds the GameUI null check.
static const char SIG_CEngineVGui_UpdateCustomProgressBar_Thunk[] =
	"48 83 3D ? ? ? ? 00 48 89 F7 74 03 EB ? 90 C3";
// The body, trampolined; our own calls still go through the thunk.
static const char SIG_CEngineVGui_UpdateCustomProgressBar[] =
	"55 BA 00 04 00 00 48 89 E5 41 54 4C 8D A5 F0 FB FF FF 48 81 EC 18 04 00 00";
static const size_t HOOK_COPY_CEngineVGui_UpdateCustomProgressBar = 6;

// DownloadCache::PersistToDisk(this, RequestContext_t*).
static const char SIG_DownloadCache_PersistToDisk[] =
	"55 48 89 E5 41 57 41 56 41 55 41 54 49 89 FC 53 48 81 EC 28 11 00 00 48 8B 3F 48 85 FF";
static const size_t HOOK_COPY_DownloadCache_PersistToDisk = 6;

// DecompressBZipToDisk(const char* out, const char* src, char* data, int bytes).
static const char SIG_DecompressBZipToDisk[] =
	"55 48 89 E5 41 57 41 56 49 89 D6 BA 04 01 00 00 41 55 41 89 CD 41 54 4C 8D A5 A0 FC FE FF 53 48 81 EC 58 03 01 00";
static const size_t HOOK_COPY_DecompressBZipToDisk = 6;

// BZ2_bzread(BZFILE*, void*, int), statically linked bzlib; jz rel8 wildcarded.
static const char SIG_BZ2_bzread[] =
	"83 BF E8 13 00 00 04 74 ? 55 49 89 F8 89 D1 48 89 F2 48 89 E5 4C 89 C6 48 83 EC 10 48 8D 7D FC";
static const size_t HOOK_COPY_BZ2_bzread = 7;

// engine.so CClientState::ProcessServerInfo(this, SVC_ServerInfo*); call rel32 wildcarded.
static const char SIG_CClientState_ProcessServerInfo[] =
	"55 48 89 E5 41 55 41 54 49 89 FC 53 48 89 F3 48 83 EC 08 E8 ? ? ? ? 48 89 DE 4C 89 E7";
static const size_t HOOK_COPY_CClientState_ProcessServerInfo = 6;

static const ptrdiff_t OFF_SVC_ServerInfo_m_nMapMD5   = 0x38; // MD5Value_t, map lumps
static const ptrdiff_t OFF_SVC_ServerInfo_m_szMapName = 0x60; // const char*

// engine.so MD5_MapFile(MD5Value_t* out, const char* mapfile).
static const char SIG_MD5_MapFile[] =
	"55 48 89 E5 41 57 41 56 41 55 41 54 49 89 F4 53 48 8D B5 58 F7 FF FF";

// engine.so CDownloadManager::Queue(this, baseURL, urlPath, gamePath).
static const char SIG_CDownloadManager_Queue[] =
	"55 48 89 E5 41 57 49 89 D7 41 56 49 89 FE 48 89 CF 41 55 49 89 F5 41 54 49 89 CC 53";
static const size_t HOOK_COPY_CDownloadManager_Queue = 6;

// QueueInternal devirtualises slot 4 against the default, so swap the slot, don't patch the function.
static const int VT_CDownloadManager_SetupURLPath    = 4; // (RequestContext_t*, const char* urlPath), default ignores urlPath
static const int VT_CDownloadManager_OnDownloadError = 9; // (RequestContext_t*), empty by default
static const ptrdiff_t OFF_RequestContext_bIsBZ2   = 0x002;
static const ptrdiff_t OFF_RequestContext_urlPath  = 0x114;
static const ptrdiff_t OFF_RequestContext_gamePath = 0x314;

// engine.so IsValidFileForTransfer(const char*); call rel32 wildcarded.
static const char SIG_IsValidFileForTransfer[] =
	"55 48 89 E5 41 55 49 89 FD 41 54 53 48 81 EC 18 01 00 00 E8 ? ? ? ? 3D 03 01 00 00";
static const size_t HOOK_COPY_IsValidFileForTransfer = 6;

// client.so CHLClient vtable, reached via CreateInterface("VClient017").
static const int VT_CHLClient_LevelInitPreEntity = 5; // (const char* mapname)

// CGameMovement::ProcessMovement, reading gpGlobals->frametime.
static const char SIG_ProcessMovementGlobals[] =
	"4C 8D 25 ? ? ? ? 49 8D 8D ? ? 00 00 53 F3 0F 10 86 ? ? 00 00 48 89 D3 "
	"48 8B 15 ? ? ? ? 49 8B 04 24 F3 0F 10 60 10 F3 0F 59 C4";

// CPrediction::StartCommand storing the usercmd in m_pCurrentCommand.
static const char SIG_StartCommandStore[] = "E8 ? ? ? ? 4C 89 A3 ? ? ? ? 4C 89 E7 E8";
static const size_t OFF_StartCommandStore_Disp = 8;

// ClientModeShared::Init, which opens with GET_HUDELEMENT(CHudChat).
static const char SIG_ClientModeShared_Init[] =
	"55 48 8D 35 ? ? ? ? 48 89 E5 41 56 41 55 41 54 4C 8D 25 ? ? ? ? 53 48 89 FB 4C 89 E7 E8";
static const size_t OFF_ClientModeShared_Init_gHUD        = 0x14; // rel32
static const size_t OFF_ClientModeShared_Init_FindElement = 0x20; // call rel32
static const int VT_CBaseHudChat_ChatPrintf = 22; // (int client, int filter, const char* fmt, ...), 21 is Printf

// Itanium puts two destructor slots first, so ProcessMovement is 2 (1 on MSVC).
static const int VT_IGameMovement_ProcessMovement = 2; // (CBasePlayer*, CMoveData*)
// From landfix.games.txt; the Linux server shares CCSGameMovement's layout.
static const int VT_IGameMovement_CheckJumpButton = 30;

// engine.so CreateInterface("VEngineClient014").
static const int VT_IVEngineClient_ServerCmd        = 6;
static const int VT_IVEngineClient_ClientCmdUnrestricted = 106;
static const int VT_IEngineSound_EmitAmbientSound = 11; // IEngineSoundClient003: (sample, volume, pitch, flags, soundtime)
static const int VT_IVEngineClient_GetViewAngles    = 19;
static const int VT_IVEngineClient_SetViewAngles    = 20;
static const int VT_IVEngineClient_IsInGame         = 26;
static const int VT_IVEngineClient_GetGameDirectory = 35;
static const int VT_IVEngineClient_GetLevelName     = 51;
static const int VT_IVEngineClient_GetNetChannelInfo = 72; // same as Windows, unchecked here

// engine.so CreateInterface("VEngineClientStringTable001"), checked by disassembly.
static const int VT_INetworkStringTableContainer_FindTable = 4; // (const char* name)
static const int VT_INetworkStringTable_GetStringUserData = 12; // (int index, int* length)
static const int VT_INetworkStringTable_FindStringIndex   = 13; // (const char* name), 0xFFFF if missing

// CHLClient::DispatchUserMessage; its first instructions give the usermessages dict.
static const int VT_Client_DispatchUserMessage = 36;
// CHLClient::EncodeUserCmdToBuffer, which forwards to the global input's.
static const int VT_Client_EncodeUserCmdToBuffer = 24;

// Itanium ConCommandBase: two dtors, IsCommand, IsFlagSet, AddFlags, GetName.
// CCvar (libvstdlib.so): 6 RegisterConCommand, 10 FindCommandBase.
static const int VT_CCB_IsCommand = 2;
static const int VT_CCB_GetName   = 5;
static const int VT_Cvar_Register = 6;
static const int VT_Cvar_FindBase = 10;

// IHandleEntity's destructor takes two slots here, so GetRefEHandle is 3.
static const int VT_IHandleEntity_GetRefEHandle = 3;

// typedescription_t.fieldSizeInBytes; inputFunc is a 16 byte member pointer here.
static const ptrdiff_t OFF_typedescription_fieldSizeInBytes = 0x48;

// vguimatsurface.so CMatSystemSurface, CreateInterface("VGUI_Surface030"). HFont is 32 bits here.
static const int VT_Surface_DrawSetTextFont   = 17; // (HFont)
static const int VT_Surface_DrawPrintText     = 22; // (const wchar_t*, int len, int type), advances the text pos
static const int VT_Surface_DrawGetTextPos    = 21; // (int&, int&)
static const int VT_Surface_GetCharABCwide    = 73; // (HFont, int ch, int& a, int& b, int& c): bearing, width, rest of the advance
static const int VT_Surface_CreateFont        = 66;
static const int VT_Surface_SetFontGlyphSet   = 67; // (HFont, name, tall, weight, blur, scanlines, flags, rangeMin, rangeMax)
static const int VT_Surface_GetFontTall       = 69; // (HFont)
static const int VT_Surface_GetFontName       = 130; // (HFont)
static const int VT_Surface_GetFontFamilyName = 131; // (HFont)

// Chain from GetFontName/GetFontFamilyName down to CLinuxFont, one sig per step.
static const char SIG_Surface_FontManagerCall[] =
	"55 48 89 E5 41 54 41 89 F4 48 83 EC 08 E8 ? ? ? ? 44 89 E6 4C 8B 65 F8 C9 48 89 C7 E9";
static const char SIG_FontManager_Getter[]   = "48 8D 05 ? ? ? ? C3";
static const char SIG_FontManager_Amalgam[]  = "48 8B 07 89 F6 48 8D 14 F6 31 F6 48 8D 3C D0 E9";
// CFontAmalgam's first-face accessor, and the lea that loads CLinuxFont::GetFamilyName.
static const char SIG_FontAmalgam_FirstFace[] =
	"85 F6 78 ? 3B 77 10 7D ? 89 F6 48 C1 E6 04 48 03 37 48 8B 7E 08";
static const char SIG_FontAmalgam_FamilyLea[] = "48 8D 15";
static const size_t OFF_FontAmalgam_FamilyLea = 0x1e;
// CLinuxFont::GetFamilyName.
static const char SIG_LinuxFont_GetFamilyName[] = "48 8B 87 A0 08 00 00 48 85 C0 74 ? 48 8B 40 28 C3";

static const ptrdiff_t OFF_FontManager_Count   = 0x10;
static const size_t    FontAmalgam_SIZE        = 72;
static const ptrdiff_t OFF_FontAmalgam_Count   = 0x10;
static const size_t    FontRange_SIZE          = 16; // int first, int last, CLinuxFont*
static const ptrdiff_t OFF_LinuxFont_FTFace    = 0x8a0;

// client.so CHudChatHistory, via vgui::RichText::Paint.
static const int VT_CHudChatHistory_Paint = 131;

// Options tab: GameUI.so's vgui_controls, vgui2.so's IPanel/IInput and the surface above.
// tests/gameui_probe checks these against the installed game.
static const int VT_Panel_GetVPanel          = 0;
static const int VT_Panel_Paint              = 131;
static const int VT_Panel_PerformLayout      = 135;
static const int VT_Panel_OnMousePressed     = 107; // (MouseCode)
static const int VT_Label_SetText            = 214; // (const char*)
static const int VT_Label_GetText            = 216; // (char*, int)
static const ptrdiff_t OFF_Sheet_Pages       = 0x238; // CUtlVector of 16 byte entries, page first
static const ptrdiff_t OFF_Sheet_PageCount   = 0x248;
static const ptrdiff_t OFF_Sheet_Tabs        = 0x258; // CUtlVector<PageTab*>, one per page
static const ptrdiff_t OFF_Sheet_TabCount    = 0x268;
static const int VT_IPanel_GetAbsPos         = 11;
static const int VT_IPanel_SetVisible        = 15;
static const int VT_IPanel_IsVisible         = 16;
static const int VT_IPanel_GetChildCount     = 18;
static const int VT_IPanel_GetChild          = 19;
static const int VT_IInput_GetCursorPos      = 9;
static const int VT_Surface_DrawSetColor     = 10; // (int r, g, b, a)
static const int VT_Surface_DrawFilledRect   = 12;
static const int VT_Surface_DrawOutlinedRect = 14;
static const int VT_Surface_DrawSetTextColor = 18; // (int r, g, b, a)
static const int VT_Surface_DrawSetTextPos   = 20;
static const char RTTI_OptionsSubMultiplayer[] = "22COptionsSubMultiplayer";
static const char RTTI_PropertySheet[]         = "N4vgui13PropertySheetE";

// materialsystem.so CShaderSystem::LoadTexture's leading-slash check (see singleletter_fix.cpp).
static const char SIG_ShaderSystem_LoadTexture_SlashSkip[] =
	"80 38 2F 49 89 C6 0F 84 ? ? ? ? 80 78 01 2F 0F 84 ? ? ? ?";
static const size_t OFF_SlashSkip_Je1    = 6;
static const size_t OFF_SlashSkip_Index  = 14; // disp8 of the second cmp
static const size_t OFF_SlashSkip_Je2    = 16;
static const char SIG_SlashSkip_Increment[] = "49 83 C6 01";

static const char RTTI_CSPlayer[] = "10C_CSPlayer";
static const int VT_CSPlayer_Interpolate       = 143; // (float currentTime)
static const int VT_CSPlayer_ShouldInterpolate = 210;
static const int VT_CSPlayer_GetObserverTarget = 307;
static const int VT_CSPlayer_CalcInEyeCamView  = 358; // (Vector& eye, QAngle& angles, float& fov)
// GetClientInterpAmount: max(cl_interp, cl_interp_ratio / cl_updaterate). No vtable entry.
static const char SIG_GetClientInterpAmount[] =
	"55 48 89 E5 41 54 48 83 EC 18 0F B6 05 ? ? ? ? 84 C0 0F 84 ? ? ? ? 4C 8B 25 ? ? ? ? 4D 85 E4 0F 84 ? ? ? ? "
	"48 8B 3D ? ? ? ? 48 8B 07 FF 90 A8 00 00 00 48 8B 3D ? ? ? ? F3 0F 11 45 E8";
