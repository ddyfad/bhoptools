# bhoptools

Server side of client-predicted checkpoint saves and teleports for shavit in CS:S. The client half is [bhoptools](https://github.com/ddyfad/bhoptools), for Windows and Linux: it predicts a save or teleport on the command you press it, and this plugin does the same thing on the same usercmd, so nothing waits a round trip and prediction doesn't correct it afterwards.

Saves and teleports go through `impulse 150` and `impulse 151`. A usercmd carries its impulse, so the client and the server both know exactly which command the teleport happens on. On a server running this plugin the client sends `sm_save` and `sm_tele` as those impulses, so existing binds work. Noclip, restart and setstart work the same way (see below).

## What it does

- Saves with `Shavit_SaveCheckpoint` after the command's move and teleports with `Shavit_TeleportToCheckpoint`, so shavit's own checks and forwards all run.
- Teleports go to shavit's current checkpoint, so prev, next and the checkpoint menu work. The client is told whenever the current checkpoint changes.
- Tells the client when the menu on screen is its own checkpoint menu, which checkpoint it shows and which command the server had run when it drew it. The client then predicts menu keys 3 and 4 as prev/next and redraws the menu itself, instead of showing the old checkpoint until shavit's redraw arrives a round trip later. A hash of the start of the menu's text lets the client check it's attaching this to the right menu, and says whether keys 1 and 2 can be predicted as save and teleport too (see below).
- Confirms every save with the exact saved state, and tells the client when a save or teleport is refused or shavit deletes the checkpoint.
- Sends the client shavit's "Checkpoint (N) saved." line and the checkpoint count, so it can print the line the moment you save.
- Runs shavit-misc's noclip commands on the command the client toggled noclip on, and tells it whether they did anything.
- Runs shavit's restart (`sm_r`) and setstart (`sm_sp`) on the command the client pressed them on, and tells it in advance where a restart goes.
- Tells the client whether [rngfix](https://github.com/jason-e/rngfix) is running and what its cvars are set to, again whenever that changes (map configs, loading or unloading rngfix), so the client predicts exactly the fixes the server applies. Those cvars aren't replicated, and a client never predicts rngfix on a server that doesn't report it.
- Tells the client the runspeed shavit-misc gives its style and which landfix method it gets, so the client moves like the server on the ground and around landings.
- Tells the client whether [momsurffix2](https://github.com/GAMMACASE/MomSurfFix) is running and what its cvars are, so the client moves with the same `TryPlayerMove` (ramp fixes, and getting unstuck from a floor a teleport left it in).
- Tells the client whether [shavit-syncstyle](https://github.com/Nimmy2222/shavit-syncstyle)'s autosync applies to the player, where it runs among the plugins and when it last saw a teleport, so the client moves with the angles and sidemove it forces.
- Ends the client's stale touch links right after each of its commands, so trigger `OnEndTouch` outputs don't depend on how its packets were grouped into server frames.
- If [tpcomp](https://github.com/ddyfad/tpcomp/tree/lagfix) is loaded, turns off its angle compensation for teleports the client predicted, and tells it which clients predict `trigger_teleport`s.

Players without the client library can use the same binds and get a normal checkpoint load.

## Requirements

- shavit-checkpoints.
- shavit-misc, for noclip.
- shavit-zones, for restarts.
- A 32-bit server: the view snap is cleared through `pl.fixangle`, read at a fixed offset from `pl.deadflag`.

## Cvars

- `sm_bhoptools_quiet` (1): don't print shavit's "teleported to checkpoint" line for impulse teleports.
- `sm_bhoptools_refused_snap` (1): snap an old (protocol 1) client's view back after a refused teleport. Newer clients turn back themselves.
- `sm_bhoptools_debug` (0): print what the server does with each impulse to the player's console.
- `sm_bhoptools_log` (0): log every save, teleport and protocol message to `logs/bhoptools.log`.
- `sm_bhoptools_duck_teleport_fix` (1): stop the engine snapping a player into the floor after a trigger teleport on the command they duck or unduck.
- `sm_bhoptools_sync_origin` (1): after each command, copy the abs origin to the networked `m_vecOrigin` when a plugin moved only the abs origin (Haze-style landfix lifts). 0 off, 1 predicting clients, 2 everyone.

## rngfix reporting

rngfix registers no library and its cvars stay behind when it unloads, so the plugin counts rngfix as loaded while a plugin named `RNGFix` (or `rngfix.smx`) is running. It checks at startup, on map configs, when a plugin unloads and once a second, and hooks the rngfix cvars as soon as they exist.

The hello response has capability flag 4 (`CAPS_RNGFIX`). Clients that set hello flag 4 then get, after `bhoptools_caps` and again on every change:

```
bhoptools_rngcvars <session> <loaded> <downhill> <uphill> <edge> <triggerjump> <telehop> <stairs> <useoldslopefixlogic>
```

`loaded` is 0 or 1; the rest are the cvar values (`uphill` is -1, 0 or 1), all 0 when rngfix isn't loaded.

## momsurffix2 reporting

momsurffix2 detours `CGameMovement::TryPlayerMove` and replaces it: more bumps, retraces with a grown hull when the player is stuck on a ramp or in a floor, and a push out along the plane found. Its cvars aren't replicated. The plugin counts it as loaded while a plugin named `Momentum surf fix '2` (or `momsurffix2.smx`) is running, checked like rngfix's.

The hello response has capability flag 128 (`CAPS_MSF`). Clients that set hello flag 128 then get, after `bhoptools_caps` and again on every change:

```
bhoptools_msf <session> <loaded> <bumpcount> <retrace length> <noclip workaround> <os>
```

`<retrace length>` (`momsurffix_ramp_initial_retrace_length`) is the float's bits in hex; `<bumpcount>` and `<noclip workaround>` are `momsurffix_ramp_bumpcount` and `momsurffix_enable_noclip_workaround`. `<os>` is `OSType` from the plugin's gamedata (1 Windows, 2 Linux): on Windows it clips velocity with its own copy of the old `ClipVelocity`, on Linux with the engine's. All 0 when momsurffix2 isn't running.

## Checkpoint menu

When shavit builds a player's own checkpoint menu, the plugin sends:

```
bhoptools_menu <session> <current> <total> <last command run> <length> <hash> <keys> <items>
```

`<hash>` is FNV-1a (32-bit, printed as a signed int) of the first `<length>` bytes of the text SourceMod's radio style sends in the ShowMenu message(s): the title and a newline, then items 1 to 4 as `->1. <text>\n` (`1. <text>\n` when disabled). The rest of the menu is only known once it's drawn (the angle/velocity settings, the translated exit item), and nothing past item 4 is used. The client only treats the menu on screen as the checkpoint menu when its text starts with those bytes, so another plugin's menu arriving in the same frame isn't mistaken for it. Without the last four arguments (or when the start can't be hashed) older behaviour applies: the newest menu is taken to be the checkpoint menu and only keys 3 and 4 are predicted.

`<keys>`: 1 when key 1 can go out as the save impulse (item 1 is shavit's save and `Shavit_SaveCheckpoint` would save as the menu does: segmented, or below `shavit_checkpoints_maxcp`), 2 when key 2 can go out as the teleport impulse (item 2 is shavit's teleport, and either the client set hello flag 512 or the player's "use angles" and "use velocity" settings are both on, since an older client's teleport always restores both). The client only sends key 2 as an impulse while the teleport item on screen can be picked. Menu saves print no chat line, so neither does the client for them.

`<items>` has one letter per item from item 1: `s` save, `t` teleport, `p` previous, `n` next, `d` delete current, `r` reset, `o` use others, `a` use angles, `v` use velocity, `-` and `+` timescale, `z` pause, `x` anything else. CS:S closes a menu the moment a key picks an item. For the items shavit redraws the menu after (`a`, `v`, `d`, `-`, `+`, `z`), the client sends the key to the server as usual but shows the menu again straight away, with the angle or velocity box already ticked, until shavit's redraw arrives.

## Checkpoint settings

The hello response has capability flag 1024 (`CAPS_CPSET`). Clients that set hello flag 512 get, after `bhoptools_caps` and whenever it changes (checked every command):

```
bhoptools_cpset <session> <flags>
```

`<flags>`: 1 restore angles, 2 restore velocity: the player's "use angles" and "use velocity" checkpoint settings from the `shavit_checkpoints` cookie, or both on segmented styles. For these clients impulse teleports restore only what the last `bhoptools_cpset` said: without velocity the player keeps its own (still passed to `TeleportEntity`, so the base velocity is zeroed as the client does), without angles the view stays where the command has it and nothing is shifted afterwards. Older clients keep getting both.

## Autobhop

shavit-core clears `IN_JUMP` from `m_nOldButtons` before the move when autobhop is on for the player (`sm_auto`, and the style's `autobhop` or an autobhop zone), the player walks, isn't in water and holds jump. rngfix reads the old buttons after that, so the client's rngfix prediction has to do the same. The hello response has capability flag 8 (`CAPS_AUTOBHOP`); clients that set hello flag 8 get, after `bhoptools_caps` and whenever it changes (checked every command):

```
bhoptools_autobhop <session> <0/1>
```

`sm_auto` is read from shavit's `shavit_autobhop` cookie, like the checkpoint settings from `shavit_checkpoints`.

## Style keys

shavit-core's `OnPlayerRunCmd` zeroes `vel[0]`/`vel[1]` and clears the buttons of keys the style blocks, before the move, so a client that predicts the command as it sent it moves with keys the server throws away. In shavit's order:

- `block_pleft`/`block_pright`: while the timer runs (and isn't paused), outside the start zone, `+left` or `+right` zeroes both. Without shavit-zones every command is zeroed while the timer runs.
- `bCanUseAllKeys` is set on a ladder with `shavit_core_simplerladders` and cleared on the ground. Nothing below applies while it's set, on a ladder, in noclip or in a freestyle zone, nor on the ground unless the style has `force_groundkeys`.
- `block_w`, `block_a`, `block_s`, `block_d`: the key, or a move in its direction without it.
- `a_or_d_only`: the first of A or D pressed after the timer starts is the only side allowed (`iKeyCombo`).
- `force_hsw 1`: W alone, A or D alone, and S with A or D are blocked. `force_hsw 2` (surf HSW) keeps whichever of W+A/S+D or W+D/S+A comes first outside the start zone and blocks everything else.

`block_pstrafe` isn't reported. The hello response has capability flag 4096 (`CAPS_KEYS`); clients that set hello flag 2048 get, after `bhoptools_caps` and whenever it changes (checked after every command):

```
bhoptools_keys <session> <flags> <combo> <command>
```

`<flags>`: 1, 2, 4, 8 `block_w`, `block_a`, `block_s`, `block_d`; 16 `a_or_d_only`; 32 `force_hsw` above 0; 64 `force_hsw 2`; 128 `force_groundkeys`; 256 `block_pleft`; 512 `block_pright`; 1024 timer running and not paused; 2048 shavit-zones not loaded; 4096 in a start zone of the timer's track; 8192 in a freestyle zone; 16384 `bCanUseAllKeys`; 32768 `shavit_core_simplerladders`; 65536 paused with `shavit_core_pause_movement` 0 or 3, where shavit returns before any of this. `<combo>` is `iKeyCombo` (-1 none; `a_or_d_only`: 0 A, 1 D; surf HSW: 0 W+A/S+D, 1 W+D/S+A). They're the state after `<command>`, read from `Shavit_SaveSnapshot` once its zone touches and timer starts are done. The client works out the ground, ladder and noclip itself and carries the ladder state and combo over the commands it predicted since.

## Autosync

shavit-syncstyle's `OnPlayerRunCmd`, on a style whose specialstring contains its `autostrafer` cvar (`autosync`) with the player's `sm_synctoggle` on (reset to on at a change to such a style), swaps in the angles of the last command it ran on, so the server moves one command behind the view. It then sets `vel[1]` to 400 (and `IN_MOVERIGHT`) when the yaw went down by more than 0.01, -400 (`IN_MOVELEFT`) when it went up. The sidemove is skipped:

- on the ground, except on the first ground tick with jump held;
- until it has run on 9 commands since the last `CBaseEntity::Teleport` of the player (its own DHook: trigger teleports, checkpoints, restarts, and rngfix's `TeleportEntity` fixes all count);
- when a hull trace 64 units down (`MASK_PLAYERSOLID`, the player's own bounds) hits something with a normal z within ±0.7.

In noclip, on a ladder, with `MOVETYPE_NONE` or at least waist deep in water it returns before any of that and keeps its state.

The hello response has capability flag 8192 (`CAPS_AUTOSYNC`, only when the `Teleport` offset is in `sdktools.games`); clients that set hello flag 4096 get, after `bhoptools_caps` and whenever it changes (checked after every command):

```
bhoptools_autosync <session> <command> <on> <order> <zero> <movesonly>
```

`<on>`: syncstyle is running, the style has autosync and the player hasn't turned it off. The toggle is mirrored from `sm_synctoggle` (bhoptools registers it too). `<order>` is where syncstyle's `OnPlayerRunCmd` runs, from the plugin load order: 0 before shavit-core's, 1 between shavit-core's and bhoptools', 2 after bhoptools'. It decides what it sees of the key blocking, noclip, impulse teleports and restarts. `<zero>` is the command syncstyle's teleport count is 0 on, from bhoptools' own hook on `Teleport`: the next command for a teleport outside a command or during one, the same command for bhoptools' impulse teleports and restarts when syncstyle runs after bhoptools (0 for none yet). `<movesonly>` is 1 for syncstyle 1.2.1 and later, which only counts teleports that move the player. Changes to `<order>` and `<zero>` only go out while `<on>` is 1. They apply from the command after `<command>`. The client works out the ground, water, movetype, angles and slope trace itself, and adds the teleports it predicts (triggers, checkpoints, restarts, rngfix fixes) until the server's word on them arrives.

## Noclip

The client sends shavit-misc's noclip commands as impulses: `impulse 154` for `sm_nc`, `sm_noclip`, `sm_noclipme`, `sm_prac` and `sm_practice` (without arguments; `sm_noclip <target>` is SourceMod's admin noclip and goes to the server as typed), `impulse 155` for `+noclip` and `impulse 156` for `-noclip`. On that command, before the move, the plugin runs `sm_nc`, `+noclip` or `-noclip` with `FakeClientCommand`, so shavit-misc's own checks (`shavit_misc_noclipme` and its overrides, alive, the stop-timer warning menu, one noclip per tick), timer stop and `SetEntityMoveType` all happen as usual, and the client moves with the new movetype on that same command. Then:

```
bhoptools_ack <session> <command> o <1 if the movetype changed, 0 if not>
```

A client that predicted a change shavit refused undoes it on replays that still run that command. Chat triggers (`!noclip`, `/nc`) are `say` on the server, so the client can't send those as impulses and they still take a round trip.

The hello response has capability flag 16 (`CAPS_NOCLIP`) when shavit-misc is loaded (`sm_nc` and `shavit_misc_noclipme` exist). Clients that set hello flag 16 get, after `bhoptools_caps` and whenever it changes (checked every command):

```
bhoptools_noclip <session> <flags>
```

1: `sm_nc` would enter noclip (access, and no stop-timer warning, or paused with `shavit_core_pause_movement`), 2: `sm_nc` would leave noclip (access), 4: `+noclip` would enter noclip (the `noclipme` override, no stop-timer warning). `-noclip` always leaves. The client predicts from these, so a press that opens shavit's "stop your timer?" menu isn't shown as noclip for a round trip.

## Restart

The client sends `sm_r`, `sm_restart` and `sm_start` without arguments as `impulse 157`, and `sm_sp`, `sm_ss`, `sm_setstart` and `sm_startpoint` without arguments as `impulse 158`. Everything else (`sm_b`, `sm_m`, `sm_r` with arguments, chat triggers) goes to the server as typed.

On an `impulse 157` command, before the move, the plugin runs `sm_r` with `FakeClientCommand`, so shavit-core's `Command_StartTimer` (the restart cvar, the start zone, the main track for players who set `sm_ihate!main`), shavit-misc's stop-timer warning menu, the forwards and shavit-zones' teleport all happen on that command. It reads back what happened: whether `Shavit_OnRestart` fired, whether shavit-zones' teleport zeroed the velocity and set the view (the fixangle it leaves, which the plugin then clears, as the client turns its own view), and whether `StartTimer` ran (`Shavit_OnStart`). When the view was set, the command moves with the new angles, as on the client. A dead player's restart (shavit-misc respawns instead) runs with `FakeClientCommandEx` after the command. Then:

```
bhoptools_ack <session> <command> r <done> <flags> <x> <y> <z> <pitch> <yaw> <gravity> <laggedmovement>
```

`<done>` is 1 when the player restarted and was teleported, and the rest is where to and what came with it, floats as bits: flag 1 teleported, 2 the view was set to pitch/yaw, 4 the timer started (gravity and laggedmovement are the style's, from `StartTimer`), 8 stamina is 0 and 16 movetype is walk (both shavit-misc's `Shavit_OnRestart`). The client replays that command with this, turns its view to match, and undoes a restart it predicted that didn't happen.

shavit-zones keeps each player's setstart to itself and loads it from its database, so the plugin can't work out a destination in advance. It remembers where each impulse restart that moved or turned the player went, per track, and works out the rest from the setstart commands it sees: a setstart on the ground becomes the destination 1 unit up, with its view and no timer start; one in the air only sets the view of a start zone destination, is ignored by a custom spawn and makes a setstart destination unknown (the start zone centre it goes back to hasn't been seen). The setstart commands are listened for as commands and as `say` chat triggers (`!sp`, `/ss`). `sm_deletestart` and its aliases open a menu whose pick isn't seen, so they forget every destination, and nothing is learned until the player has no menu open. Map changes, zone count changes and `shavit_zones_extra_spawn_height` changes forget them too, as does reconnecting. A track is unknown until the player's first impulse restart there, which isn't predicted.

The hello response has capability flag 2048 (`CAPS_RESTART`) when shavit-zones and the newer shavit-core natives are there. Clients that set hello flag 1024 get, after `bhoptools_caps`, whenever it changes (checked every command) and after every setstart impulse:

```
bhoptools_restart <session> <command> <flags> <x> <y> <z> <pitch> <yaw> <gravity> <laggedmovement>
```

It says what an `sm_r` does from the command after `<command>` on, with the same flags and fields as the ack. Flag 1 is only set when a press restarts and teleports: the restart cvars are on, the track has a start zone (or is a KZ map), the player is alive, no stop-timer warning menu would open, no delete-setstart menu is open, and the destination is known. The gravity and laggedmovement for flag 4 are the style's `gravity`, and the player's timescale times the style's `timescale` and `speed`, multiplied in `UpdateLaggedMovement`'s order. The other flags say what a setstart would do to it, so the client can predict a restart pressed within a round trip of an `sm_sp`: 32 one on the ground makes it the destination, 64 one on the ground doesn't change it (it's for the timer's track, which isn't the restart's), 128 one in the air sets only the view, 256 one in the air doesn't change it. Without the bit for its kind the client stops predicting restarts until the next `bhoptools_restart`.

On an `impulse 158` command the plugin runs `sm_setstart` after the move (shavit-zones saves the position then, as the client does), works out the new destination, and sends `bhoptools_restart` for that command even when nothing changed. That's the ack: the client's own setstart is replaced by it.

## Runspeed and landfix

shavit-misc post-hooks `CCSPlayer::GetPlayerMaxSpeed` through DHooks when `shavit_misc_staticprestrafe` is on, and returns the style's `runspeed` for every live player. The only virtual call is `CGameMovement::ProcessMovement`'s `mv->m_flMaxSpeed = player->GetPlayerMaxSpeed()`, so every ground move accelerates with it; the client's stock prediction uses the weapon's 250.

SuperLandfix runs one of two methods:

- Haze: in `OnPlayerRunCmd`, before the move, when `m_hGroundEntity` isn't the previous command's and isn't none, the player is lifted to 1.5 units above the ground, measured with a 2 unit hull trace down. Ground with `m_currentSound` (`func_movelinear`, `func_water_analog`) and ground moving vertically are left alone.
- Olivia/NoTimeLoss (the default): a pre hook on `CheckJumpButton`. On the ground, out of water and with neither jump nor duck in the old buttons, a player within 0.49 units of the ground is raised to 0.49 above it, and one between 1.5 and 2 units lowered to 1.5.

The hello response has capability flag 32 (`CAPS_MOVE`). Clients that set hello flag 32 get, after `bhoptools_caps` and whenever any of these changes (checked after every command):

```
bhoptools_move <session> <runspeed> <landfix> <easybhop> <command>
```

`<runspeed>` is the float's bits in hex, 0 when nothing overrides `GetPlayerMaxSpeed` (shavit-misc not running or `shavit_misc_staticprestrafe 0`). `<landfix>`: 0 off, 1 Haze, 2 Olivia. 1 and 2 come from SuperLandfix's `SuperLandfix_GetMethod(client)` native, so each player's setting, type and jump toggles are followed; 0 when SuperLandfix isn't loaded. `<easybhop>` is 1 when the style has easybhop, 2 inside an easybhop zone, 0 neither (shavit-core zeroes the stamina then). `<command>` is the command these apply after, so the client re-predicts from there.

After `bhoptools_caps` they also get the entities Haze leaves alone, in parts:

```
bhoptools_lfskip <session> <total> <offset> <entity>...
```

## Hold after teleport (betterseg)

[betterseg](https://github.com/dowoge/better-seg)'s `!seg_freeze` holds a player at the checkpoint after a teleport: every command it teleports them back with no velocity and `MOVETYPE_NONE`, until a command with forward or side movement releases them with the checkpoint's velocity. A client that doesn't know keeps predicting the movement and is pulled back a round trip later.

The hello response has capability flag 262144 (`CAPS_HOLD`) when betterseg is loaded with the `BetterSeg_GetFreeze` and `BetterSeg_SetPredictedHold` natives. Clients that set hello flag 1048576 get, after `bhoptools_caps` and whenever it changes (checked after every command):

```
bhoptools_hold <session> <armed> <command>
```

`<armed>`: 1 when a teleport now would be held: the player has freeze on, the timer is started (not stopped, more than five ticks old) and not paused. The client then holds after a checkpoint teleport it predicts: from the teleport command itself it holds the player at the checkpoint with no velocity and `MOVETYPE_NONE` (betterseg sets `MOVETYPE_NONE` right after the teleport for these clients, so the teleport command doesn't move either), and on the first command with forward or side movement (read before key blocking), or once `<armed>` goes to 0, it restores the checkpoint's velocity and movetype for that command. For these clients the plugin also calls `BetterSeg_SetPredictedHold(client, true)`, which stops betterseg ramping their speed after the release (that only hid the round trip this removes).

## Touch links

The engine ends a player's stale touch links (`PhysicsCheckForEntityUntouch`, which fires the triggers' `EndTouch`) once per server frame, after all the commands that arrived in that frame, and shavit-core again around each move. Which command's `OnEndTouch` a booster's output follows then depends on how the network grouped packets, which the client can't predict. For clients that set hello flag 64 the plugin calls `PhysicsCheckForEntityUntouch` in `OnPlayerRunCmdPost` of every command (after its touch pass, PostThink and an impulse save), so the outputs always follow the command that left the trigger. The call comes from shavit's gamedata (`shavit.games`, `PhysicsCheckForEntityUntouch`); the hello response has capability flag 64 (`CAPS_UNTOUCH`) when it was found.

## Entity diff

Stripper:Source configs, `.lmp` lump files and `OnMapInit` edits change the entities the server spawns, but the client reads the BSP's own entity lump. At map start the plugin compares the BSP's lump with SourceMod's `EntityLump`, which has all of those edits, and keeps the entities the client's trigger sim reads that were removed, changed or added. It publishes them in the `InfoPanel` string table, so one copy reaches every client in its signon, before it spawns: `bhoptools_mapdiff` is `<entries> <crc32> 1 <lump crc>` and `bhoptools_mapdiff_0` onwards hold the diff in 16000-byte pieces. The hello response has capability flag 16384 (`CAPS_MAPDIFF`) when that worked: the lump was readable and uncompressed, and the diff fits in 128000 bytes.
