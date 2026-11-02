#pragma once
#include <cstddef>

// MSVC gives a class one destructor slot where Itanium has two, so indices after one are 1 lower.
static const int VT_IGameMovement_ProcessMovement = 1; // (CBasePlayer*, CMoveData*)
// From landfix.games.txt for the Windows server; checked by disassembly.
static const int VT_IGameMovement_CheckJumpButton = 29;

// engine.dll CreateInterface("VEngineClient014"), checked by disassembly.
static const int VT_IVEngineClient_ServerCmd        = 6;
static const int VT_IVEngineClient_ClientCmdUnrestricted = 106;
static const int VT_IEngineSound_EmitAmbientSound = 11; // IEngineSoundClient003: (sample, volume, pitch, flags, soundtime)
static const int VT_IVEngineClient_GetViewAngles    = 19;
static const int VT_IVEngineClient_SetViewAngles    = 20;
static const int VT_IVEngineClient_IsInGame         = 26;
static const int VT_IVEngineClient_GetGameDirectory = 35;
static const int VT_IVEngineClient_GetLevelName     = 51;
static const int VT_IVEngineClient_GetNetChannelInfo = 72;

// engine.dll CreateInterface("VEngineClientStringTable001"), checked by disassembly.
static const int VT_INetworkStringTableContainer_FindTable = 3; // (const char* name)
static const int VT_INetworkStringTable_GetStringUserData = 11; // (int index, int* length)
static const int VT_INetworkStringTable_FindStringIndex   = 12; // (const char* name), 0xFFFF if missing

// CHLClient::DispatchUserMessage; its first instructions give the usermessages dict.
static const int VT_Client_DispatchUserMessage = 36;
// CHLClient::CreateMove, which calls IInput::CreateMove on the global input.
static const int VT_Client_CreateMove = 21;

// MSVC ConCommandBase: one dtor, IsCommand, IsFlagSet, AddFlags, GetName.
// CCvar (vstdlib.dll, checked by disassembly): 6 RegisterConCommand, 10 FindCommandBase.
static const int VT_CCB_IsCommand = 1;
static const int VT_CCB_GetName   = 4;
static const int VT_Cvar_Register = 6;
static const int VT_Cvar_FindBase = 10;

// One destructor slot, so GetRefEHandle is 2.
static const int VT_IHandleEntity_GetRefEHandle = 2;

// CGameMovement::ProcessMovement, scaling gpGlobals->frametime by the lagged movement value.
static const char SIG_ProcessMovementGlobals[] =
	"48 8B 05 ? ? ? ? 48 8B D9 0F 29 74 24 30 41 B9 03 00 00 00 F3 0F 10 70 10 0F 28 C6 F3 0F 59 82 ? ? ? ? F3 0F 11 40 10";

// CPrediction::RunCommand storing the usercmd in m_pCurrentCommand, right after ResetInstanceCounters().
static const char SIG_StartCommandStore[] = "E8 ? ? ? ? 48 8B CE 48 89 B7 ? ? ? ? E8";
static const size_t OFF_StartCommandStore_Disp = 11;

// typedescription_t.fieldSizeInBytes; inputFunc is an 8 byte member pointer here (16 on Linux).
static const ptrdiff_t OFF_typedescription_fieldSizeInBytes = 0x40;

static const size_t RecvProp_SIZE = 0x60;

// engine.dll CDownloadManager::QueueInternal(this, baseURL, urlPath, gamePath, bool bAsHTTP, bool bIsBZ2).
// CL_QueueDownload inlines Queue, so this is the hookable part.
static const char SIG_CDownloadManager_QueueInternal[] =
	"48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 41 54 41 55 41 56 41 57 48 81 EC 50 02 00 00 FF 41 54 33 DB";

// CDownloadManager vtable slots; the default SetupURLPath copies gamePath to urlPath and
// OnDownloadError is empty.
static const int VT_CDownloadManager_SetupURLPath    = 4; // (RequestContext_t*, const char* urlPath)
static const int VT_CDownloadManager_OnDownloadError = 9; // (RequestContext_t*)

// CDownloadManager::UpdateProgressBar, slot 1.
static const char SIG_CDownloadManager_UpdateProgressBar[] =
	"40 53 48 81 EC 40 02 00 00 4C 8B 41 28 48 8B D9 4D 85 C0 0F 84 ? ? ? ? 41 80 78 03 00";
static const ptrdiff_t OFF_CDownloadManager_m_activeRequest = 0x28;

// RequestContext_t, same layout as Linux.
static const ptrdiff_t OFF_RequestContext_bIsBZ2        = 0x002;
static const ptrdiff_t OFF_RequestContext_bAsHTTP       = 0x003;
static const ptrdiff_t OFF_RequestContext_urlPath       = 0x114;
static const ptrdiff_t OFF_RequestContext_gamePath      = 0x314;
static const ptrdiff_t OFF_RequestContext_nBytesTotal   = 0x618;
static const ptrdiff_t OFF_RequestContext_nBytesCurrent = 0x61C;

// CEngineVGui::UpdateCustomProgressBar(this, float progress, const wchar_t* desc). No separate
// thunk here: the GameUI null check is its first compare.
static const char SIG_CEngineVGui_UpdateCustomProgressBar[] =
	"48 81 EC 38 04 00 00 48 83 3D ? ? ? ? 00 49 8B C0 0F 29 B4 24 20 04 00 00 0F 28 F1";

// DownloadCache::PersistToDisk(this, RequestContext_t*).
static const char SIG_DownloadCache_PersistToDisk[] =
	"48 89 74 24 20 41 56 48 81 EC 60 02 00 00 48 83 39 00 48 8B F2 4C 8B F1 0F 84";

// DecompressBZipToDisk(const char* out, const char* src, char* data, int bytes).
static const char SIG_DecompressBZipToDisk[] =
	"48 89 5C 24 20 56 57 41 57 B8 50 03 01 00 E8 ? ? ? ? 48 2B E0 4C 8B FA 49 8B D8";

// BZ2_bzread(BZFILE*, void*, int), with BZ2_bzRead inlined.
static const char SIG_BZ2_bzread[] =
	"40 53 41 57 48 83 EC 28 83 B9 E8 13 00 00 04 45 8B F8 48 8B D9 75";

// CClientState::ProcessServerInfo(this, SVC_ServerInfo*). `this` is the handler subobject (+0x10).
static const char SIG_CClientState_ProcessServerInfo[] =
	"48 89 5C 24 08 57 48 83 EC 20 48 8B FA 48 8B D9 E8 ? ? ? ? 48 8B D7 48 8B CB E8 ? ? ? ? 84 C0 75 ? 48 8B 43 F0 48 8D 4B F0";

static const ptrdiff_t OFF_SVC_ServerInfo_m_nMapMD5   = 0x38;
static const ptrdiff_t OFF_SVC_ServerInfo_m_szMapName = 0x60;

// MD5_MapFile(MD5Value_t* out, const char* mapfile).
static const char SIG_MD5_MapFile[] =
	"48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 20 55 41 56 41 57 48 8D AC 24 70 F8 FF FF 48 81 EC 90 08 00 00 48 8B DA 4C 8B F9";

// IsValidFileForTransfer(const char*).
static const char SIG_IsValidFileForTransfer[] =
	"48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 55 48 8D 6C 24 D0 48 81 EC 30 01 00 00 48 8B F1 48 85 C9 0F 84";

// client.dll CHLClient via CreateInterface("VClient017").
static const int VT_CHLClient_LevelInitPreEntity = 5; // (const char* mapname)

// Options tab: GameUI.dll's vgui_controls, vgui2.dll's IPanel/IInput and vguimatsurface.dll's
// CMatSystemSurface ("VGUI_Surface030"). MSVC emits one destructor slot and orders overloads
// differently, so these differ from Linux; tests/gameui_probe checks them.
static const int VT_Panel_GetVPanel          = 0;
static const int VT_Panel_Paint              = 130;
static const int VT_Panel_PerformLayout      = 134;
static const int VT_Panel_OnMousePressed     = 106; // (MouseCode)
static const int VT_Label_SetText            = 214; // (const char*); 213 is the wchar_t one
static const int VT_Label_GetText            = 216; // (char*, int)
static const ptrdiff_t OFF_Sheet_Pages       = 0x250;
static const ptrdiff_t OFF_Sheet_PageCount   = 0x260;
static const ptrdiff_t OFF_Sheet_Tabs        = 0x270;
static const ptrdiff_t OFF_Sheet_TabCount    = 0x280;
static const int VT_IPanel_GetAbsPos         = 10;
static const int VT_IPanel_SetVisible        = 14;
static const int VT_IPanel_IsVisible         = 15;
static const int VT_IPanel_GetChildCount     = 17;
static const int VT_IPanel_GetChild          = 18;
static const int VT_IInput_GetCursorPos      = 8;
static const int VT_Surface_DrawSetColor     = 11; // (int r, g, b, a)
static const int VT_Surface_DrawFilledRect   = 12;
static const int VT_Surface_DrawOutlinedRect = 14;
static const int VT_Surface_DrawSetTextFont  = 17;
static const int VT_Surface_DrawSetTextColor = 19; // (int r, g, b, a)
static const int VT_Surface_DrawSetTextPos   = 20;
static const int VT_Surface_DrawPrintText    = 22; // (const wchar_t*, int len, int type)
static const int VT_Surface_CreateFont       = 66;
static const int VT_Surface_SetFontGlyphSet  = 67; // (HFont, name, tall, weight, blur, scanlines, flags, rangeMin, rangeMax)
static const char RTTI_OptionsSubMultiplayer[] = ".?AVCOptionsSubMultiplayer@@";
static const char RTTI_PropertySheet[]         = ".?AVPropertySheet@vgui@@";

static const char RTTI_CSPlayer[] = ".?AVC_CSPlayer@@";
static const int VT_CSPlayer_Interpolate       = 97;  // (float currentTime)
static const int VT_CSPlayer_ShouldInterpolate = 151;
static const int VT_CSPlayer_GetObserverTarget = 243;
static const int VT_CSPlayer_CalcInEyeCamView  = 294; // (Vector& eye, QAngle& angles, float& fov)
// GetClientInterpAmount: max(cl_interp, cl_interp_ratio / cl_updaterate). No vtable entry.
static const char SIG_GetClientInterpAmount[] =
	"40 53 48 83 EC 40 8B 05 ? ? ? ? A8 01 75 29 48 8B 0D ? ? ? ? 48 8D 15 ? ? ? ? 83 C8 01 89 05 ? ? ? ? 48 8B 01 FF 50 68 "
	"48 8B D8 48 89 05 ? ? ? ? EB 07 48 8B 1D ? ? ? ? 48 85 DB 0F 84";

// Host_AccumulateTime: min(host_frametime, 0.1) then max(.., 0.001); the maxsd is 15 bytes in.
static const char SIG_HostAccumulateTime_MinFrametime[] =
	"F2 0F 5D 0D ? ? ? ? 66 0F 5A C1 0F 5A D0 F2 0F 5F 15 ? ? ? ? 66 0F 5A CA";
