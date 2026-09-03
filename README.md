# Crimson Desert Co-op Mod

A co-op multiplayer mod for [Crimson Desert](https://store.steampowered.com/app/3321460/Crimson_Desert/) that allows two players to play through the game together. The host player's game world is shared with a second player who joins via Steam P2P networking.

> **Status: 0.3.1 Pre-Alpha - Safe Player-Sync Baseline**
>
> Steam connection polling, handshake retry, position interpolation, and health state handling form the current testable baseline. Steam build `25050808` has verified WorldSystem, local-player, TransformSync position/rotation, health reads, and active companion enumeration. Direct companion pose writes race AI, and removing the AI component caused a delayed crash, so remote pose application remains disabled until a safe engine update hook is found. The end-to-end two-player path is not ready for release. Enemy/combat sync and direct animation writes remain disabled because their previous hook ABI/entity mapping was unsafe. DX12 overlay rendering is experimental and off by default; the Present hook remains tick-only.

## What's New in 0.3.1

- Fixed the Steam session deadlock: transports are polled while hosting and connecting, and clients send/retry the handshake only after the asynchronous P2P connection is ready.
- Both peers now locate a validated local companion candidate, with retry when the companion appears after loading; mutation remains disabled pending a safe engine update hook.
- Position-only packets no longer overwrite remote HP/animation state, reconnects clear stale state, and no companion writes occur before the first valid packet.
- Added committed-page checks for game-memory reads/writes and fail-closed unsupported-build detection.
- Disabled unsafe legacy inline hooks and unverified enemy synchronization.
- Added a Windows build workflow and packet validation tests.

## What's New in 0.2.3

- **May 2026 stat-layout refresh** - Public bbfox CT v29 / OpenCheatTables data has stamina and spirit current values at `stats_component+0x518` and `stats_component+0x5A8` on current builds. The legacy v1.01 offsets are still kept as fallbacks, and `MountSync` now validates `StatEntry::TYPE` before choosing the current or legacy mount stamina entry.
- **CD Companion offset leads added** - Fresh public source from [CD Companion](https://github.com/leandrodiogenes/cd-companion) exposes a separate physics-delta teleport path, static XYZ globals, in-game map-marker capture, and a camera-heading hook. These are now documented as `CD_COMPANION_*` signatures for future opt-in teleport/camera work; they do not replace the native fast-travel apply function, which is still unidentified.
- **Mount stat read hardening** - `MountSync` now checks committed readable memory before reading HP/stamina StatEntry ranges, reducing the chance that a stale mount pointer crashes the game after dismount or patch drift.
- **ChildActor vtable resolved at startup** - `HookManager::resolve_child_actor_vtbl()` walks three fallback EquipHide patterns (`CHILD_ACTOR_VTBL_P1/P2/P3`) using RIP-relative LEA resolution and populates the previously-orphaned `RuntimeOffsets::child_actor_vtbl`. New `is_child_actor(entity)` helper compares an entity's vtable pointer against the resolved address — single read + compare. `EnemySync` now uses it as a third filter layer (after the explicit player + hijacked-companion checks) so other companion entities, summons, and related child actors stop being mis-classified as enemies.
- **WorldSystem probe now logs RTTI class names + vfunc fingerprints** - With `dump_world_system_probe = true`, each sibling line in `cdcoop_world_probe.log` now includes the MSVC-decorated RTTI class name (e.g. `.?AVQuestManager@pa@@`) read from the vtable's `RTTICompleteObjectLocator`, plus the first 4 vtable function RVAs. That's the behavioural fingerprint plus the human-readable class name a community submitter needs to identify which sibling is the quest / cutscene / world-object manager — in most cases they'll be readable directly from the decorated name without needing to cross-reference RVAs at all. Significantly more useful than the bare vtable RVA we logged in 0.2.2. Every dereference is bounded by `VirtualQuery` so a bad vtable just logs `(unknown)` rather than touching garbage memory.
- **Hook correctness fixes (from 0.2.2 [#21](https://github.com/blizz3010/CrimsonDesertCoop/pull/21))** - `dragon_hp_probe` was treating `rcx` as the marker but the AOB hits a mid-function `mov [r13+0x160]` write — converted to `SafetyHookMid` and reads `ctx.r13` directly. `player_position_detour` was broadcasting garbage `y`/`z` from detour args at a mid-function site — now reads position from the verified pointer chain (same path used for rotation). Adds `is_valid_ptr` guards in the overlay and a `Mount::HP_SANITY_MAX_RAW` constant.
- **Documented audit findings** - `docs/REVERSE_ENGINEERING.md` now lists every signature and offset that is documented in `signatures::` / `offsets::` but not yet wired into a hook or sync path, with a short note on possible co-op use. Helps contributors see at a glance what scaffolding exists for the next features.

## What's New in 0.2.2

- **Mount HP / stamina sync (opt-in)** - With `sync_mount_state = true`, `MountSync` installs a `SafetyHookMid` on the Orcax `MOUNT_PTR_CAPTURE` AOB to capture the local mount entity, polls HP/stamina from its stats component at 5Hz, and broadcasts a new `MOUNT_STATE` packet. Both peers read each other's mount state and the session panel displays a dedicated "Peer's Mount" HP + stamina bar when the remote player is mounted. Edge-triggered: mount and dismount events broadcast immediately without waiting for the next tick. Read-only on the remote side — no writes to the local mount entity, no attempt to spawn a mount for the companion. See the [Current Limitations](#current-limitations) table for the visual-sync caveat.

## What's New in 0.2.1

- **Fast-travel mid-hook (opt-in)** - With `sync_fast_travel = true`, `HookManager` now installs a `SafetyHookMid` at `CrimsonDesert.exe+0xAB5594` (the map-waypoint apply site documented in [`docs/RESEARCH_2026-04-18.md`](docs/RESEARCH_2026-04-18.md) #7). On the host, the detour reads `[r15+0x1C..0x28]` (waypoint X/Y/Z) and `[r15+0x00]` (waypoint type id), then broadcasts a new `TELEPORT_TRIGGER` packet. The client-side apply is intentionally log-only for this release because the apply function (the one that consumes the staged target and triggers the area transition) hasn't been identified — the existing 30Hz position broadcast already pulls the companion entity along once the host arrives.
- **First mid-function hook in the project** - `HookManager::create_mid_hook()` and the `safetyhook::Context`-based detour signature are now available for any future register-state captures (quest manager dispatch, cutscene trigger, etc.).
- **Two new community AOBs** in `signatures::` - `MOUNT_PTR_CAPTURE` and `MOUNT_STAMINA_ACCESS` (from Orcax-1399 player-status-modifier scanner). This landed the research foundation later used by 0.2.2 mount HP / stamina sync.
- **Build cleanup** - Fixed C2027 undefined `SteamNetConnectionStatusChangedCallback_t` regression that broke Windows builds with the Steamworks SDK ([#18](https://github.com/blizz3010/CrimsonDesertCoop/pull/18)). Fixed C4456 shadowed local in the position detour.

## What's New in 0.2.0

- **Animation evaluator hook (experimental)** - A new hook based on [CDAnimCancel](https://github.com/faisalkindi/CDAnimCancel)'s research captures the real animation evaluator struct. When captured, `AnimationSync` writes to `evaluator+0x00 / +0x04` (state / blend) instead of the deprecated `actor+0x120 / +0x124` fields that the community has shown to be inert.
- **Dragon HP auto-scan** - On first dragon mount the mod walks `marker+0x08..+0x200` looking for a plausible float HP value (100 <= v <= 1e7). The chosen offset is cached in `RuntimeOffsets::dragon_hp_offset` and surfaced in the debug overlay. Read-only, no writes.
- **WorldSystem sibling probe** - With `dump_world_system_probe = true`, the mod walks `WorldSystem+0x30..+0x100` and logs every sibling pointer's vtable RVA to `cdcoop_world_probe.log`. This is telemetry to help the community identify the quest / cutscene / world-object managers. Best-guess candidates are cached in `RuntimeOffsets` for on-demand use.
- **Hook-install telemetry** - `HookManager::status()` now exposes `installed / failed` counts and the list of hook names that succeeded vs. failed. The debug overlay surfaces this so users on a new game patch can see at a glance which signature broke.
- **Primary/fallback signature try-helper** - `try_hook_pair()` cleaned up the repetitive primary/fallback install blocks and standardised the logging prefix.
- **Config gating** - Experimental behavior is opt-in via two new flags: `enable_experimental_hooks` (default `false`) and `dump_world_system_probe` (default `false`). Default install keeps today's stable behavior.
- **MSVC /W4 + /permissive- on cdcoop_core** - Tightened compiler warnings to catch common issues in project sources without affecting third-party deps.
- **Offset cleanup** - The estimated actor animation offsets (`0x120 / 0x124 / 0x130 / 0x131`, etc.) are kept but clearly marked `DEPRECATED` in `game_structures.h`. The new canonical path is `offsets::AnimationEvaluator::*`. Stamina/spirit offset relationship (`entry + CURRENT_VALUE`) is now documented inline.

## Intended Flow

The following is the target two-player flow. It is not release-ready until the
current companion write path and two-machine Steam session are verified.

1. **Host starts a session** (press F7 in-game) - their game becomes the shared world
2. **Client connects** via Steam friend invite or Steam ID
3. **Player 2 appears** as a hijacked companion entity (Oongka/Yann/Naira) controlled by the remote player's inputs
4. **Game state syncs** through the host (enemies, damage, positions)

### Architecture

```
+-----------------+         Steam P2P          +-----------------+
|  HOST (Player 1)|<-------------------------->| CLIENT (Player 2)|
|                 |   Position, Animation,     |                  |
|  Authoritative  |   Combat, Enemy State      |  Receives world  |
|  game world     |                            |  state from host |
|                 |                            |                  |
|  Companion slot |                            |  Sends inputs &  |
|  = Player 2     |                            |  position to host|
+-----------------+                            +-----------------+
```

### Key Design Decisions

- **Companion Hijacking**: We take over an existing companion slot so the game handles rendering, collision, and animations for Player 2 natively
- **Host-Authoritative**: The host's game state is the source of truth for enemies, world interactions, and damage validation
- **Steam P2P**: Uses Steamworks `ISteamNetworkingSockets` - NAT traversal, encryption, reliable/unreliable channels, no server needed
- **ASI Injection**: Loaded via standard ASI loader. Uses [safetyhook](https://github.com/cursey/safetyhook) for mid-function hooking

## Current Limitations

These features are **still not fully functional in-game** and need further research or live-process testing:

| Feature | Status | What's Blocking It |
|---------|--------|--------------------|
| Player position / health | Local reads verified on Steam build `25050808`; network path implemented | Two-peer field testing and world-origin rebasing still need verification |
| Companion / remote player | Discovery verified; pose writes disabled | ActorManager's persistent registry and Mercenary filter locate the nearest companion. Direct TransformSync writes race AI; clearing the AI component caused a delayed crash. A safe AI/update hook is still required. Remote health is not written into save-backed companion stats |
| Animation sync (cross-model) | Direct actor writes disabled | Per-model animation IDs still need PAZ extraction and the evaluator must be mapped per remote companion |
| Enemy / combat sync | Disabled | Previous ActorManager enumeration and pointer-derived IDs were not valid across processes |
| DX12 overlay | Experimental, off by default | The game's DirectX command queue and cross-queue fence lifecycle are not yet captured safely |
| Fast-travel sync | Capture-only mid-hook (opt-in via `sync_fast_travel`) | Hook reads the host's waypoint and broadcasts `TELEPORT_TRIGGER`; receive-side is log-only because the native apply function isn't identified. CD Companion's physics-delta path is documented as a possible opt-in fallback, but not wired |
| Quest sync | Receive-side stub + candidate pointer logging | Quest manager identity is now probed - set `dump_world_system_probe=true`, reproduce progress on host, and send `cdcoop_world_probe.log` to the issue tracker |
| Cutscene sync | Receive-side stub + candidate pointer logging | Same probe as above. Trigger function still unknown |
| World interaction sync | Receive-side stub + candidate pointer logging | Same probe as above |
| Dragon mount HP | Dynamically resolved at first mount (experimental) | Auto-scan picks a plausible float. Value surfaces in the debug overlay - verify against in-game HP bar and report back if wrong |
| Mount HP / stamina sync | State broadcast + overlay (opt-in via `sync_mount_state`) | `MOUNT_PTR_CAPTURE` mid-hook captures the local mount entity, `MountSync` polls HP/stamina at 5Hz and broadcasts `MOUNT_STATE`. Stamina uses the May 2026 StatEntry layout first and falls back to the legacy layout by type-id validation. Visual sync (the companion entity still hovers at mount height on the remote side because no mount entity is spawned there) is a follow-up |
| Per-action combat flags | Evaluator `+0x6A` flag captured (experimental) | Works only when evaluator hook is enabled and a combat action is active. Actor-base `0x130 / 0x131` stay deprecated |
| Full camera struct | Zoom/FOV only | Camera mods use PAZ XML, not runtime memory. Only `+0xD8` is mapped |

## Building

### Requirements

- **Windows 10/11** (the game is Windows-only)
- **Visual Studio 2022** with C++ desktop development workload
- **CMake 3.20+**
- **Steamworks SDK** (download from [Steamworks](https://partner.steamgames.com/))

### Build Steps

```bash
# Clone the repository
git clone https://github.com/blizz3010/CrimsonDesertCoop.git
cd CrimsonDesertCoop

# Configure with CMake
cmake -B build -G "Visual Studio 17 2022" -A x64 \
    -DSTEAMWORKS_SDK_PATH="C:/path/to/steamworks_sdk"

# Build
cmake --build build --config Release
```

The output `CrimsonDesertCoop.asi` will be in `build/bin/Release/`.

### Building without Steam (for testing)

```bash
cmake -B build -G "Visual Studio 17 2022" -A x64 -DCDCOOP_USE_STEAM=OFF
cmake --build build --config Release
```

## Installation

### Recommended: CDUMM

This is a native-code mod, so the runtime payload remains an ASI plugin. Current
[CDUMM](https://www.nexusmods.com/crimsondesert/mods/207) releases can import ASI
plugins and mixed ZIP packages alongside JSON/PAZ data mods.

1. Install **CDUMM** and complete its game-directory setup
2. Drop the release ZIP into CDUMM and enable CrimsonDesertCoop in its ASI section
3. Apply the mod and launch the game through CDUMM

### Manual Installation

1. Download [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases) (`dinput8.dll` version)
2. Copy `dinput8.dll` to your Crimson Desert game directory (next to `CrimsonDesert.exe`)
3. Copy `CrimsonDesertCoop.asi` to the same directory
4. (Optional) Copy `cdcoop_config.json` to customize settings
5. Launch the game normally through Steam

### Compatibility

- Recommended with [CDUMM](https://www.nexusmods.com/crimsondesert/mods/207); legacy [JSON Mod Manager](https://www.nexusmods.com/crimsondesert/mods/113) also supports ASI plugins
- Compatible with most PAZ-based mods (camera, visuals, equipment)
- **May conflict** with other ASI mods that hook the same game functions (damage, position writes). If using alongside the [player-status-modifier](https://www.nexusmods.com/crimsondesert/mods/52), disable overlapping features in the config

## Usage

| Key | Action |
|-----|--------|
| **F7** | Host, or join `join_steam_id` when configured |
| **F8** | Toggle the opt-in experimental overlay UI |

### Hosting

1. Leave `join_steam_id` empty and press **F7** to start hosting
2. Share your Steam ID with your friend, or use the Steam overlay to invite them
3. Your friend sets `join_steam_id` to your Steam ID and presses F7, or joins through Steam while both games are running

### Configuration

Edit `cdcoop_config.json` in the game directory. The file is auto-created with defaults if missing.

```json
{
    "player_name": "Player",
    "port": 0,
    "join_steam_id": "",
    "use_steam_networking": true,

    "enemy_hp_multiplier": 1.5,
    "enemy_dmg_multiplier": 1.0,
    "tether_distance": 150.0,

    "sync_cutscenes": false,
    "sync_quest_progress": false,
    "sync_fast_travel": false,
    "sync_mount_state": false,
    "skip_animation_remap": true,

    "player2_model_id": -1,
    "player2_use_companion_slot": true,

    "debug_overlay": false,
    "enable_experimental_overlay": false,
    "log_packets": false,
    "log_level": 2,

    "enable_experimental_hooks": false,
    "diagnose_companion_position_write": false,
    "dump_world_system_probe": false,

    "toggle_overlay_key": 119,
    "open_session_key": 118
}
```

| Option | Default | Description |
|--------|---------|-------------|
| `player_name` | `"Player"` | Display name in co-op |
| `port` | `0` | Steam P2P virtual port. This is not a UDP port and must be below 1000 |
| `join_steam_id` | `""` | If set, F7 joins this Steam ID instead of hosting |
| `enemy_hp_multiplier` | `1.5` | Scale enemy HP for 2 players |
| `enemy_dmg_multiplier` | `1.0` | Scale enemy damage |
| `tether_distance` | `150.0` | Max distance between players (meters) |
| `sync_cutscenes` | `false` | Cutscene sync (receive-side still stubbed) |
| `sync_quest_progress` | `false` | Quest sync (receive-side still stubbed) |
| `sync_fast_travel` | `false` | Install the map-waypoint mid-hook. Host broadcasts captured waypoints; receive-side is log-only for now |
| `sync_mount_state` | `false` | Install mount pointer capture. Both peers broadcast mount HP/stamina so the overlay can show the other player's mount status |
| `skip_animation_remap` | `true` | Use passthrough mode for animation sync |
| `player2_model_id` | `-1` | -1 = same as host character model |
| `player2_use_companion_slot` | `true` | Hijack companion vs spawn new entity |
| `debug_overlay` | `false` | Show debug info + hook status in overlay |
| `enable_experimental_overlay` | `false` | Opt into the unverified DX12 ImGui renderer. Present remains hooked for networking when disabled |
| `log_packets` | `false` | Log network packets to cdcoop.log |
| `log_level` | `2` | 0=trace, 1=debug, 2=info, 3=warn, 4=error |
| `enable_experimental_hooks` | `false` | Install CDAnimCancel animation-evaluator hook and dragon HP probe. Disable if mod becomes unstable after a game patch |
| `diagnose_companion_position_write` | `false` | Install a diagnostic mid-hook at the authoritative position store and log whether it runs for the selected companion. The detour does not change saved registers or game-state data |
| `dump_world_system_probe` | `false` | Walk WorldSystem sibling pointers once after resolve and log their vtable RVAs to `cdcoop_world_probe.log`. Safe, read-only telemetry |
| `toggle_overlay_key` | `119` | F8 keycode |
| `open_session_key` | `118` | F7 keycode |

## Project Structure

```
CrimsonDesertCoop/
+-- include/cdcoop/
|   +-- core/           # Hooks, memory scanning, game structures, config
|   +-- network/        # Session management, packets, Steam P2P transport
|   +-- sync/           # Player, enemy, world, animation synchronization
|   +-- player/         # Companion hijacking, player entity management
|   +-- ui/             # ImGui overlay
+-- src/
|   +-- asi/            # DLL entry point (loaded by ASI loader)
|   +-- core/           # Hook installation, memory scanning, config I/O
|   +-- network/        # Networking implementation
|   +-- sync/           # Sync system implementations
|   +-- player/         # Player & companion management
|   +-- ui/             # Overlay rendering (DX12)
+-- tools/
|   +-- offset_scanner.py   # Helper for finding game memory offsets
+-- docs/
|   +-- REVERSE_ENGINEERING.md  # Guide for discovering game offsets
+-- CMakeLists.txt
```

## Contributing

Core systems are functional but several areas need community help. If you have experience with Cheat Engine, x64dbg, IDA/Ghidra, or ReClass.NET, see [`docs/REVERSE_ENGINEERING.md`](docs/REVERSE_ENGINEERING.md) for verified offsets and what still needs work.

### What's Needed (Missing Offsets)

These are the remaining offsets/systems needed. **If you have access to any of these, please contribute!**

| # | Priority | System | What We Need | Status |
|---|----------|--------|--------------|--------|
| 1 | **HIGH** | Animation IDs per model | Extract animation paths from .paac files (721 nodes in `sword_upper.paac` per CDGuardCancel research). The original [CDAnimCancel](https://github.com/faisalkindi/CDAnimCancel) repo went 404, but the same author published the successor [**CDGuardCancel**](https://github.com/faisalkindi/CDGuardCancel) with `tools/extract_paac.py` + `patch_transitions.py` and full .paac format documentation (3-layer guard block system, 622KB bytecode section). Needed for true cross-model remap | Evaluator hook exists (experimental) but the ID namespace is still same-model-only |
| 2 | **MEDIUM** | Quest Manager | Confirm which WorldSystem sibling slot from `cdcoop_world_probe.log` is the quest manager, then identify its "set stage" entry function | Probe scaffolding landed; waiting on community log submissions |
| 3 | **MEDIUM** | Cutscene Manager | Same probe approach; find the cutscene trigger function | Probe scaffolding landed; waiting on logs |
| 4 | **MEDIUM** | Camera State | Map camera struct beyond zoom (`+0xD8`) - position, rotation, target. CD Companion adds a camera-heading hook at `r15+0x4A4`, which helps rotation but not full camera position/target | Partial (zoom + heading lead) |
| 5 | **MEDIUM** | Dragon HP verification | Read `[dragon_mount+0xD8]` in-game and confirm it tracks the HP bar. Strong candidate from the dragon-mount field map at `CrimsonDesert.exe+0x339D8CB` (see [`docs/RESEARCH_2026-04-18.md`](docs/RESEARCH_2026-04-18.md)). Integrated as `Mount::DRAGON_HP_PREFERRED_OFFSET` | **Candidate known**, needs field read-back |
| 6 | **LOW** | World Objects | Confirm WorldSystem sibling for doors / chests / interactive world objects | Probe candidate stored but not yet mapped to a dispatch function |
| 7 | **LOW** | Teleport apply function | The native capture mid-hook at `+0xAB5594` is **landed** (0.2.1, gated on `sync_fast_travel`) and broadcasts `TELEPORT_TRIGGER`. CD Companion now provides a separate physics-delta teleport path (`CD_COMPANION_PHYS_DELTA`) that can be tested as an opt-in receiver apply fallback. The native area-transition function that consumes `[r14+0xD8] / [r14+0xE0]` is still unknown | Native capture landed; physics fallback lead documented |
| 8 | **LOW** | Combat Flag verification | The evaluator `+0x6A` flag from CDAnimCancel now writes when experimental hooks are enabled - confirm it actually gates co-op combat state the way we want | New hook needs field testing |
| 9 | **LOW** | Mount visual sync | **0.2.2 landed mount state sync** via the `MOUNT_PTR_CAPTURE` mid-hook + `MOUNT_STATE` packet + `MountSync`. What's still missing is visual parity: the companion entity appears to hover at mount height on the remote side because no mount entity is spawned for it. A full fix needs the mount-spawn function (unknown). An intermediate fix could piggyback on the companion's own native mount when it spawns during single-player |
| 10 | **LOW** | `MOUNT_STAMINA_ACCESS` hook | The AOB is in `signatures::` and Orcax's AB00 notes still make it useful for direct write/intercept experiments. It is no longer a blocker for overlay/network mount stamina because `MountSync` reads validated StatEntry data with current and legacy layout fallback | Signature known, not needed for current sync path |

#### Where to Look

- **Animation**: The animation system uses **.paac action chart files**, not simple actor struct fields. The original CDAnimCancel repo went 404; the successor [CDGuardCancel](https://github.com/faisalkindi/CDGuardCancel) (same author) re-publishes the `extract_paac.py` parser + `patch_transitions.py` + full .paac format documentation, and confirms the evaluator function at `CrimsonDesert.exe+2712090` (AOB: `0F 28 CE 48 89 4C 24 20 48 8B CB E8`). [CrimsonForge](https://www.nexusmods.com/crimsondesert/mods/446) can extract .paa animation files from PAZ
- **Quest/Cutscene**: WorldSystem (+0x30 = ActorManager) likely has sibling pointers to other managers. Scan +0x38, +0x40, +0x48 etc. [NattKh Save Editor](https://github.com/NattKh/CRIMSON-DESERT-SAVE-EDITOR) has 633 quests / 5,450 missions for validation. CDAnimCancel found InputBlock RTTI at `0x144AFCC70` handles "menu/cutscene blocking" - possible lead
- **Teleport**: [CD Companion](https://github.com/leandrodiogenes/cd-companion) uses a physics-delta hook rather than the native fast-travel transition. Its public source gives the current static XYZ, map marker, physics delta, and camera heading patterns. Porting that path should be a separate opt-in apply mode because it bypasses the game's native area-transition flow.
- **Dragon HP**: Confirmed float type. 2026-04-18 research pass located the full mount struct field map at the dragon-timer injection point (`+0x339D8CB`) and identified `+0xD8` as the strongest HP candidate (only standalone float in the stat cluster, written with xmm8). Needs in-game read-back — see [`docs/RESEARCH_2026-04-18.md`](docs/RESEARCH_2026-04-18.md) #5
- **Camera**: [UltimateCameraMod](https://github.com/FitzDegenhub/UltimateCameraMod) has 150+ camera states in `playercamerapreset.xml`. Try ReClass on the camera struct pointer (captured via r12 in zoom hook)

### Resources (Bot-Protected / Auth-Gated)

Some of these pages require manual human access (403 for automated tools). **If you can access any gated source, the offset data inside would be valuable:**

| Resource | What It Likely Contains | Priority |
|----------|------------------------|----------|
| [FearLess CE Thread (pages 14-16)](https://fearlessrevolution.com/viewtopic.php?t=38679) | Dragon HP discussion, mount pointers, new scripts | **HIGH** |
| [Nexus Mods Cheat Table v1.0.6](https://www.nexusmods.com/crimsondesert/mods/64) | Pointer chains (partially extracted from [GitHub mirror](https://github.com/bbfox0703/Mydev-Cheat-Engine-Tables)) | MEDIUM |
| [Nexus Mods Modding Guide v4.0](https://www.nexusmods.com/crimsondesert/mods/366) | BlackSpace engine internals (updated April 2026) | MEDIUM |
| [CDCamera source/patches](https://www.nexusmods.com/crimsondesert/mods/65) | Camera field mappings (distance, height, FOV, steadycam) | MEDIUM |
| [OpenCheatTables Thread](https://opencheattables.com/viewtopic.php?t=1836) | bbfox CT discussion and refreshed stat offsets; currently mirrored into docs where accessible | LOW |
| [CD Companion source](https://github.com/leandrodiogenes/cd-companion) | Physics-delta teleport, static XYZ globals, map marker capture, camera heading hook | LOW |

### Offset Status

| Category | Status | Key Details |
|----------|--------|-------------|
| **Player Entity** | Verified on build `25050808` | WorldSystem -> ActorManager -> ChildActor -> component table -> StatusComponent -> player data |
| **Position** | Verified read on build `25050808` | TransformSync `+0x63C`, float32 XYZ |
| **Health** | Verified read on build `25050808` | StatEntry via player data `+0x58`, int64 value*1000 |
| **Stamina/Spirit** | Needs refresh | May 2026 candidate offsets do not have the expected type IDs on build `25050808` |
| **Rotation** | Verified read on build `25050808` | Quaternion at TransformSync `+0x62C` |
| **Companion System** | Discovery verified on build `25050808`; writes disabled | ActorManager registry (`array +0x128`, `capacity +0x134`); ChildActor entries with a Mercenary component at table `+0x118` are companions. Direct pose writes race AI, while clearing AI slot `+0x58` caused a delayed crash. A safe update hook is required; companion health writes are also disabled |
| **Damage Tracking** | Disabled / current-build unverified | Legacy AOBs are retained as research only |
| **Enemy HP/State** | Disabled / current-build unverified | Actor enumeration and cross-process IDs are unresolved |
| **Inventory / items / ATK/DEF** | Legacy research | Not revalidated on build `25050808` and not used by the current co-op path |
| **Camera Zoom/FOV** | Legacy research | Direct hook is disabled pending a register-correct current-build validation |
| **Progression systems** | Legacy research | Supply, contribution, trust, reputation, resistance and durability data are not used by co-op |
| **Combat State / Dragon / Mount** | Experimental or unverified | Opt-in signatures require current-build in-game validation before release use |
| **Fast-Travel Capture** | Implemented (opt-in) | `SafetyHookMid` at `+0xAB5594` reads `[r15+0x1C..0x28]`; host broadcasts `TELEPORT_TRIGGER` |
| **Mount HP / Stamina Sync** | Implemented but unverified on current build | Opt-in hook and packet path exist; current mount/stat offsets still need live validation |
| **DX12 Present** | Verified on build `25050808` | Tick-only hook installed and ran without enabling experimental rendering |
| **Steam P2P** | Compile-tested | ISteamNetworkingSockets builds and packet tests pass; two-machine runtime test remains |
| **40+ AOB Signatures** | Mixed validity | Core WorldSystem, PlayerBase, Stats, Position and ChildActor AOBs match build `25050808`; optional signatures were not all retested |

### Related Projects & Offset Sources

- [CrimsonDesert-player-status-modifier](https://github.com/Orcax-1399/CrimsonDesert-player-status-modifier) - Stats, position, damage, durability signatures (ASI mod, safetyhook)
- [CDGuardCancel](https://github.com/faisalkindi/CDGuardCancel) - **Animation system RE (successor to CDAnimCancel)**: `extract_paac.py` + `patch_transitions.py` + full .paac action chart format. Evaluator function entry confirmed at `+0x2712090`. The 0.2.x README still cites CDAnimCancel because that's where our integrated findings came from before the original repo went 404 (April 2026)
- [JustSkip](https://github.com/wealdly/JustSkip) - Combat state flag AOB and RIP-relative resolver
- [CrimsonDesertTools](https://github.com/tkhquang/CrimsonDesertTools) - WorldSystem, actor structure, equipment visibility
- [DetourModKit](https://github.com/tkhquang/DetourModKit) - AOB scanning framework used by CD mods
- [bbfox0703 Cheat Tables](https://github.com/bbfox0703/Mydev-Cheat-Engine-Tables) - 220+ entry open-source CT with reputation, friendship, durability, and the May 2026 stamina/spirit layout refresh
- [CD Companion](https://github.com/leandrodiogenes/cd-companion) - Real-time map overlay with source for static XYZ, map marker, camera heading, and physics-delta teleport hooks
- [crimson-desert-unpacker](https://github.com/lazorr410/crimson-desert-unpacker) - PAZ archive extraction tool
- [pycrimson](https://github.com/LukeFZ/pycrimson) - Python PAZ/save decrypt library
- [UltimateCameraMod](https://github.com/FitzDegenhub/UltimateCameraMod) - 150+ camera states in PAZ XML
- [SWISS Knife Save Editor](https://www.nexusmods.com/crimsondesert/mods/20) - 633 quests, 5,450 missions, 2,262 items
- [CrimsonDesertModdingResearch](https://github.com/marvelmaster/CrimsonDesertModdingResearch) - Address value table, XML configs
- [CrimsonForge](https://github.com/hzeemr/crimsonforge) - Browse 1.4M game files, full PAZ round-trip
- [Elden Ring Seamless Co-op](https://github.com/LukeYui/EldenRingSeamlessCoopRelease) - Architectural inspiration

## Technical Details

### Game Info

- **Engine**: BlackSpace Engine (Pearl Abyss proprietary, evolved from BDO engine)
- **DRM**: Denuvo Anti-Tamper (no kernel-level anti-cheat)
- **Graphics API**: DirectX 12
- **Architecture**: x64 Windows
- **Game Version**: May 2026 public offset refresh, with v1.01.03 legacy fallback notes

### Networking Protocol

All packets use a binary format with a `CD` magic header:

```
[2 bytes magic "CD"] [1 byte type] [2 bytes payload size] [4 bytes sequence] [4 bytes timestamp] [payload...]
```

Position updates are sent unreliable at 30 Hz. Combat actions and world state changes are sent reliable. A heartbeat packet maintains the connection with a 10-second timeout.

## License

This project is for educational and personal use. Use at your own risk. Not affiliated with Pearl Abyss.
