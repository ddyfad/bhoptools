## bhoptools

Ports [Momentum Mod's](https://momentum-mod.org/) `m_rawinput 2` behaviour to CS:S: mouse interpolation that lines up with the tickrate without needing a specific framerate. Runs on Windows and Linux.

Also included:
- autohop prediction (BunnyhopAPE)
- starts CS:S with `-insecure`, with the Steam library path detected
- viewpunch remover (e.g. from fall damage)
- map download progress while loading, and missing or differing maps fetched from [fastdl.me](https://fastdl.me)
- Prediction: client-side prediction of trigger teleports, pushes, boosters and outputs (including simple map VScript), rngfix, and the speed on shavit's HUD, so they don't lag behind by your ping
- on servers running the [bhoptools server plugin](https://github.com/ddyfad/bhoptools), also predicted:
  - checkpoint saves and teleports, from binds or shavit's checkpoint menu keys, with teleports following the menu's angle and velocity settings
  - noclip, restarts (`sm_r`) and setting the start position (`sm_sp`)
  - the server's shavit runspeed, easybhop, landfix, style key blocking, shavit-syncstyle autosync, momsurffix2 and mpbhops_but_working doors
  - on SourceJump servers, your own shots on damage-activated buttons (shoot-to-boost: the targetname they give you and take back), from the game's own bullet trace
- Linux: chat characters the chat font has no glyph for are drawn from a fallback font instead of as boxes (`chat_glyphs 0/1`)
- Linux: map textures under a one-character folder like `materials/z/` load instead of going missing (the game drops that folder from the name on Linux), so the singleletterfix pack isn't needed for them

### Features

Mouse and input
* Mouse interpolation: each frame's raw mouse motion is spread over its ticks, like Momentum's `m_rawinput 2`. It sets `m_rawinput` to 1 (off: 0) after each level load and on every change, and starts on unless `m_rawinput` was 0.
* Viewpunch remover: no punch from fall damage or rough landings, a client-side [SuppressViewpunch](https://github.com/xen-000/SuppressViewpunch). On by default.
* Autohop prediction (BunnyhopAPE): Windows only, off by default.
* High polling rate fix: Windows only, on by default; `bhoptools_pollfix 0` turns it off, and `bhoptools_nolegacy 0` turns off just the part that registers the mouse without legacy messages while in game. A 4-8 kHz mouse puts a message in the game's input queue every 125 us and the engine retrieves them one by one; this reads the raw input in one batch per frame and takes the cursor-move messages once per frame instead, the way Godot handles it. `bhoptools_pumpstats` prints what the input loop costs.

Prediction
* Triggers: trigger teleports, pushes, boosters and their outputs, including map VScript that only touches the activator's velocity, origin and gravity. Anything a script does outside that is left to the server.
* Doors: when func_door and func_door_rotating finish opening, start closing and so on, so their outputs fire on time. Your own +use opens "use opens" doors on the command you press it, found the way the server's FindUseEntity does. Doors opened by other players come from the server plugin's state sync, and so does the fix when a use was seen differently.
* rngfix (stair, incline and telehop fixes) and momsurffix2 (ramp clipping), mpbhops_but_working doors.
* With the bhoptools server plugin, also predicted:
  * Checkpoints: `sm_save`, `sm_tele`, `sm_prevcp`, `sm_nextcp`, and shavit's checkpoint menu keys. Teleports follow the menu's angle and velocity settings, and the commands are sent as typed while spectating.
  * Noclip: `sm_nc`, `sm_noclip`, `sm_noclipme`, `sm_prac`, `sm_practice`, `+noclip` and `-noclip`.
  * Restart and start position: `sm_r`, `sm_restart`, `sm_start`, and `sm_sp`, `sm_ss`, `sm_setstart`, `sm_startpoint`.
  * shavit's style runspeed, easybhop, style key blocking, and shavit-syncstyle's autosync.
  * Landfix and autobhop as the server reports them.
* On SourceJump servers (the SourceJump plugin answers the same protocol), its own landfix and momsurffix: the lift after a bhop or jumpbug, boosters touched from the unlifted position, and momsurffix2's bevel and displacement seam fixes. Only a server that says it runs them gets this; other servers are predicted as before.
* Chat triggers for the predicted commands above run on the client, through `say` and `say_team`.
* `bhoptools_predict` is the one switch for all of it. Without the plugin only trigger prediction runs; rngfix and momsurffix2 settings come from the plugin.

HUD
* `hud_speed`: the speed in shavit's center HUD comes from local prediction instead of the server.
* `hud_keys`: shavit's key overlay (and bgs-jumpstats' showkeys) is drawn from your newest local command instead of a ping late.
* bgs-jumpstats (jhud, trainer, prestrafe trainer, offsets, speedometer, showyaws, showkeys, ssj) is worked out and drawn from your own commands, on servers whose jumpstats sends the settings for it. Gain follows the server: the old per-tick average, bhop-get-stats' mmod gain (speed gained over the most each tick could give), both, or linear (the mmod gain on the old gain's scale), as picked in the jumpstats menu. The slow ticks HUD shows the ticks per strafe turning under 25, 50 or 75% of the best. The compact strafe and prestrafe trainers are redrawn every tick, smoothed over 0.15, 0.1 or 0.06 s by trainer speed, or every frame with "jumpstats trainers every frame" on (`jsframe`), drawn between the last two ticks. The prestrafe trainer is the strafe trainer for the ground, averaged over the trainer speed from 93% of the best top prestrafe: the top speed you are on pace for if every tick gained the share of its most that this one did, with the bar left of centre when turning too slowly and right when too fast, worked out from the server's sv_accelerate, sv_friction and sv_stopspeed. The client says hello with flags 262144, 2097152, 4194304, 8388608, 16777216, 33554432, 134217728, 268435456 and 536870912; jumpstats builds with the pace prestrafe trainer only hand their HUD to clients with the first five, and draw the gain modes and compact trainers themselves for clients without the sixth, linear gain for clients without the seventh, the slow ticks HUD for clients without the eighth and ShowYaws (at its own position since it stopped sharing the speedometer's) for clients without the last.
* betterseg's `!seg_freeze` hold after a checkpoint teleport is predicted (needs the bhoptools plugin and a betterseg with `BetterSeg_GetFreeze`), so you no longer move for a round trip before being pulled back.
* HUD slots: every HUD message gets a slot of its own by what it is sent with (channel and position), instead of sharing the game's six HudMsg channels, where elements from different plugins overwrote each other and blinked. Done on the client, so it works on any server.
* Toggles print a short message on screen (`prediction 1`, `Viewpunch: 0`, `BunnyhopAPE: 1`).

Spectating
* First-person spectating stays smooth. The watched player is kept interpolated even with no weapon out (replay bots, players on bhop servers), which otherwise needs cycling spectator modes to fix, and they alone are drawn at least 100 ms back (the default `cl_interp`) whatever yours is, since a low lerp is what makes spectating choppy. Always on, from zastools.

Downloads
* The map download bar shows byte counts, and a second bar for writing and decompressing the `.bz2`.
* When your map is missing or differs from the server's, the server's copy is fetched from fastdl.me. Map names with extra dots (`bhop_x.v2.bsp`) are allowed through.

Options tab
* `bhoptools` replaces the Multiplayer tab in Options: rawinput2 (mouse interpolation), prediction and clientside hud fixes (predicted speed, the local key overlay and bgs-jumpstats' HUD together). Everything is saved in `settings.cfg` (`rawinput`, `predict`, `viewpunch`, `fastdl`, `speedo`, `hudkeys`, `jsframe`, `chatglyphs`, `pollfix`); the fastdl.me map fix is only switched there, the chat glyph fallback with `chat_glyphs`.

Hotkeys and console commands
* `F5`: BunnyhopAPE autohop prediction (Windows only, not saved).
* `F8`: prediction, saved.
* `bhoptools_predict 0/1`, `hud_speed 0/1`, `hud_keys 0/1`, `bhoptools_viewpunch 0/1` (the viewpunch remover, on by default), on Windows `bhoptools_pollfix 0/1` (the high polling rate fix) and `bhoptools_pumpstats` (prints and resets the input loop counters), and on Linux `chat_glyphs 0/1`. With no argument they print the current state. All saved.
* `bhoptools_log` starts or stops writing debug info to `cstrike/bhoptools_log.txt`: what's predicted and what isn't, every correction from the server and its likely cause, and every `[bhoptools]` console message while it's on. Send that file with a bug report.
* Hidden (left out of `find` and autocomplete): the server plugin's protocol commands (`bhoptools_caps`, `bhoptools_ack`, `bhoptools_chat`, `bhoptools_menu`, `bhoptools_ask`, `bhoptools_rngcvars`, `bhoptools_autobhop`, `bhoptools_noclip`, `bhoptools_move`, `bhoptools_lfskip`, `bhoptools_msf`, `bhoptools_cpset`, `bhoptools_keys`, `bhoptools_autosync`, `bhoptools_ents`, `bhoptools_player`, `bhoptools_sync`, `bhoptools_restart`, `bhoptools_frames`), the shavit command names listed above, and `say`/`say_team` when the game doesn't have its own. The shavit names and protocol commands can also be run by the server.

Windows and Linux
* Windows: an injector starts CS:S with `-console -novid -insecure`. `F5` and `F8` work.
* Linux: `bhoptools` prints launch options for `LD_PRELOAD`, and the preload only acts when the game runs with `-insecure`. Only `F8` exists.
* Linux only: chat glyph fallback (`chat_glyphs`), including combining marks and Tibetan, and the one-character texture folder fix.
* Autohop prediction is not available on Linux.

### Usage
* Download the latest release from the [Releases](../../releases) page.
* Windows: run `bhoptools.exe`. It starts the game with `-insecure` for you.
* Linux: put `bhoptools.so` and `bhoptools` somewhere without spaces or `:` in the path, run `./bhoptools` and paste the launch options it prints.
* Updates: on start it checks for a newer release and offers it, with the notes of every release since yours. A release is only installed when its `SHA256SUMS` is signed by the bhoptools release key and lists the file downloaded (the first signed release reaches older versions without that check).
  * Windows: Yes replaces the exe and starts the new one, No asks again next time, Cancel stops checking.
  * Linux: the popup comes up with the game window (Update, Ignore, Ignore forever). Update downloads the new `.so` next to the old one and restarts the game into it. The files are swapped on the next launch from Steam.
  * To check again after turning it off, delete `no_update_check` from the settings folder below.
* Everything is switched from Options → `Fixes` or the hotkeys and commands above. Settings are kept in `%APPDATA%\bhoptools\settings.cfg` or `~/.config/bhoptools/settings.cfg`. With zastools loaded the tab is left to it.
* Only join servers the server browser shows as insecure.

### Troubleshooting
* A signature that no longer matches after a game update is logged as `sig <name>: NOT FOUND`, and its feature is switched off instead of crashing the game.
* Messages in the game console start with `[bhoptools]`. On Linux they also go to stderr.

### Disclaimer
* Servers with `shavit_core_hijack_teleport_angles 1` make trigger teleports rubberband you. The timer defaults it to 0.

### Building
- `git clone --recurse-submodules https://github.com/ddyfad/bhoptools.git`
- Windows: build `bhoptools.sln` with Visual Studio 2022+ (Release x64).
- Linux: `make -C linux`, then `make -C tests test` for the offline prediction tests.
- `python3 tests/gameui_probe/probe.py` checks the Options tab's vtable slots and offsets against the installed game's Linux and Windows binaries (needs `llvm-objdump`).
- `shared/` is the prediction code, `windows/` and `linux/` load it into the game.

### Credits
- [Haze1337](https://github.com/Haze1337/RawInput2): RawInput2, the original `m_rawinput 2` port.
- [alkatrazbhop](https://github.com/alkatrazbhop/BunnyhopAPE), [yupi2](https://github.com/yupi2/BunnyhopAPE) and [rtldg](https://github.com/rtldg/BunnyhopAPE): BunnyhopAPE.
- [rtldg](https://github.com/rtldg/RawInput2BunnyhopAPE): RawInput2BunnyhopAPE, which put the two together and added the download progress, fastdl.me map fixing and the viewpunch remover.
- [bhopbhopbhop](https://github.com/bhopbhopbhop/RawInput2BunnyhopAPE-lagfix): lagfix, the first client-side trigger prediction and predicted HUD speed.
- [dowoge](https://github.com/dowoge/RawInput2BunnyhopAPE/tree/linux): the Linux port.
- [zas](https://github.com/zasbu/zastools): zastools, where the Linux chat glyph fallback comes from.

### Third-party code
- [Monocypher](https://monocypher.org/) 4.0.3 (`shared/vendor/monocypher`), BSD-2-Clause or CC0-1.0 (`LICENCE.md` there): ed25519 and SHA-512 for checking signed updates.
- [Brad Conte's crypto-algorithms](https://github.com/B-Con/crypto-algorithms) SHA-256 (`shared/vendor/sha256`), public domain.
- [Microsoft Detours](https://github.com/microsoft/Detours) (`windows/Detours`, submodule), MIT.
