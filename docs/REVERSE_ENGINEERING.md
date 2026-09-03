# Reverse Engineering Guide for Crimson Desert Co-op

This document explains how to find the memory offsets and function signatures needed for the co-op mod. Many offsets are already verified and integrated - see the **Current Status** section below for what's done and what still needs work.

## Current Status (Steam build 25050808 / legacy v1.01 fallback)

| Category | Status | Details |
|----------|--------|---------|
| Player entity / WorldSystem chain | **Verified** | 3 WorldSystem AOBs uniquely match; current component chain verified live on build 25050808 |
| Position (read) | **Verified** | `ClientTransformSyncActorComponent+0x63C`, float32 XYZ; confirmed with before/after movement snapshots |
| Health | **Verified** | Player data +0x58 -> StatEntry, int64 value*1000; live read matched 300/300 HP |
| Stamina / Spirit | **Needs refresh** | May 2026 offsets remain as legacy candidates, but their expected type IDs did not match build 25050808 |
| Rotation (read) | **Verified** | Quaternion at `ClientTransformSyncActorComponent+0x62C`; live norm and facing changes verified |
| Companion discovery | **Verified; writes disabled** | ActorManager persistent registry at +0x128 (capacity +0x134) and the Mercenary filter were confirmed live. Direct pose writes race AI; clearing the AI slot caused a delayed crash |
| Damage tracking | **Disabled / needs refresh** | Legacy AOB retained, but the unsafe inline hook is disabled and current ABI is unverified |
| Enemy HP / state | **Disabled / needs refresh** | Current ActorManager enumeration and entity mapping are unresolved |
| Inventory / items | **Legacy research** | Not used by the current co-op path and not revalidated on build 25050808 |
| Reputation | **Legacy research** | Historical gain/no-decrease RVAs are not used by co-op or revalidated on build 25050808 |
| Resistance attributes | **Legacy research** | Historical injection offset is not used by co-op or revalidated on build 25050808 |
| Camera zoom/FOV | **Legacy research** | Direct hook remains disabled pending current-build register validation |
| Dragon timer | **Needs current-build test** | Historical r13+0x160 candidate is opt-in and was not exercised |
| Mount HP (horse) | **Needs current-build test** | Dynamic capture exists but was not exercised during build 25050808 validation |
| 40+ AOB signatures | **Mixed validity** | Core AOBs match build 25050808; optional signatures were not all retested |
| Animation state | **Likely wrong** | 0x120 / 0x124 estimated, but CDAnimCancel shows animation runs via .paac action charts, not simple actor fields |
| Combat flags (per-action) | **Estimated** | 0x130 / 0x131 - CDAnimCancel found evaluator flag at `[rbx+0x6A]` but on evaluator struct, not actor base |
| Quest manager | **Missing** | Not found |
| Cutscene manager | **Missing** | Not found |
| Full camera struct | **Missing** | Only zoom at +0xD8; rest is PAZ XML-controlled |
| Dragon HP | **Candidate known** | `+0xD8` in dragon-mount struct (strong float candidate from field map); needs in-game read-back |
| World object manager | **Missing** | MapLookup/MapInsert sigs exist but manager unknown |
| Teleport / fast-travel (capture) | **Hook landed** (0.2.1) | `SafetyHookMid` at `+0xAB5594`, opt-in via `sync_fast_travel`. Host broadcasts `TELEPORT_TRIGGER`; native apply path still log-only; CD Companion physics-delta fallback is documented but not wired |
| Mount pointer / stamina | **Implemented / documented** | `MOUNT_PTR_CAPTURE` is wired for mount entity capture. `MOUNT_STAMINA_ACCESS` remains documented as a direct AB00 hook, but current sync reads validated StatEntry data instead |

## Prerequisites

- **x64dbg** - Free debugger for finding function signatures
- **Cheat Engine** - Memory scanner for finding data structures
- **ReClass.NET** - Visual structure reconstruction tool
- **Ghidra** or **IDA Pro** - Disassemblers for static analysis
- A copy of Crimson Desert installed locally

## Important Notes

- Crimson Desert uses **Denuvo Anti-Tamper** (DRM), NOT kernel-level anti-cheat
- ASI injection and memory hooking are proven to work (see [CrimsonDesert-player-status-modifier](https://github.com/Orcax-1399/CrimsonDesert-player-status-modifier))
- The game uses the **BlackSpace Engine** (proprietary, no public documentation)
- Game files are in **PAZ archives** (ChaCha20 encrypted, LZ4 compressed) - see [crimson-desert-unpacker](https://github.com/lazorr410/crimson-desert-unpacker)

## Step 1: Find the Player Entity

### Method A: Cheat Engine (Recommended for beginners)

1. Open Crimson Desert and load into the game world
2. Attach Cheat Engine to the process
3. Search for your **Health** value (float, exact value)
4. Take damage, search for new value -> repeat until 1-2 results remain
5. The address you find is `Player + HEALTH_OFFSET`
6. Use "Find what accesses this address" to discover the **player base pointer**
7. In the dissect data structures view, map out the player struct

### Method B: Signature from existing mod

The `CrimsonDesert-player-status-modifier` project already hooks player stat writes. Study their signatures and hook points to find the player pointer - it's typically the first argument (`rcx` on x64 Windows) to the hooked function.

### Verified Player Structure Layout (Steam build 25050808)

```
WorldSystem
+0x30   ptr      ClientActorManager
  +0x50 ptr      ClientChildOnlyInGameActor
    +0x68 ptr    Component table
      +0x20 ptr  ClientStatusActorComponent
        +0x18 ptr  Player data
          +0x58 ptr  Stats component
      +0x1A0 ptr ClientTransformSyncActorComponent
        +0x62C float[4] Rotation quaternion
        +0x63C float[3] Position XYZ

Stats Component (+0x58):
+0x08  int64    Health current (value * 1000) (VERIFIED)
+0x18  int64    Health max (VERIFIED)
+0x518 int64    Stamina current (May 2026 candidate; not valid by type ID on build 25050808)
+0x528 int64    Stamina max (May 2026 candidate)
+0x5A8 int64    Spirit current (May 2026 candidate; not valid by type ID on build 25050808)
+0x5B8 int64    Spirit max (May 2026 candidate)

Legacy v1.01 layout kept as fallback:
+0x488 int64    Stamina current
+0x498 int64    Stamina max
+0x518 int64    Spirit current
+0x528 int64    Spirit max

Animation (ESTIMATED - likely incorrect approach):
+0x120 uint32   Animation state ID (estimated, but CDAnimCancel research shows
                 animation runs via .paac action charts, not simple actor fields)
+0x124 float    Blend weight (estimated)
+0x130 bool     IsAttacking (estimated, unverified by entire community)
+0x131 bool     IsDodging (estimated)
+0x134 uint32   WeaponId (estimated)
+0x140 float    MovementSpeed (estimated)
```

## Step 2: Find the Companion System

Companions (Oongka, Yann, Naira) are key to the co-op approach - we hijack one for Player 2.

The legacy player-data `+0xD0..+0x108` body-slot path is not valid on Steam
build 25050808. The current active actor vector is:

```
WorldSystem +0x30 -> ClientActorManager
  +0x128 ptr     Persistent actor pointer registry
  +0x134 uint32  Registry capacity (4000 observed)
```

Companions are `ClientChildOnlyInGameActor` entries whose component table
(`actor+0x68`) contains a `ClientMercenaryActorComponent` at `+0x118`. Their
Status and TransformSync components use the same paths as the current player.
The implementation scans the bounded, potentially sparse registry, selects the
nearest valid mercenary, and validates the registry entry and all cached
component links before every write. ActorManager `+0x08` contains a
`ClientActorContainer`, but its `+0x50/+0x54/+0x58` vector was observed as a
transient queue: its count returned to zero while the actors remained loaded.
It must not be used as the persistent actor list.

The companion AI component is stored at component-table `+0x58`. A direct
TransformSync write with that pointer intact was reverted by AI within 100 ms;
even continuous 60 Hz writes only held the target in 60% of samples. Temporarily
clearing the AI slot made a single pose write remain stable for a one-second
test, but the game subsequently crashed. Nulling the component is unsafe and
must not be used. `CompanionHijack` keeps pose and health writes disabled until
a safe AI/update hook is identified; health writes could also persist a peer's
max HP into the local save.

Character slot offsets (from bbfox0703 CT, v1.01.03):
| Character | Slot Offset |
|-----------|-------------|
| Kliff     | 0x68        |
| Oongka    | 0xE0        |
| Damiane   | 0x168       |

## Step 3: Find the Game Instance / World Singleton

**Already verified.** The WorldSystem singleton is found via RIP-relative signature scanning:

```
WorldSystem (sig scan) -> ActorManager (+0x30) -> ChildActor (+0x50)
    -> component table (+0x68) -> StatusComponent (+0x20)
    -> player data (+0x18)
```

Three fallback patterns exist in `game_structures.h` (WORLD_SYSTEM_P1/P2/P3).

Static base pointers (backup, may break on patch):
- `CrimsonDesert.exe+05CC7618` (bbfox0703, v1.01.03)
- `CrimsonDesert.exe+05CFE230` (ClientActorManager)

## Step 4: Find Function Signatures

For each hook, we need an **IDA-style signature** (byte pattern with wildcards).

See `include/cdcoop/core/game_structures.h` namespace `signatures` for all 40+ patterns with primary/fallback variants.

### Currently Installed Hooks

| Hook | Signature | Purpose |
|------|-----------|---------|
| PositionAccess | Disabled | AOB is a mid-function register site; position broadcasting now polls the verified pointer chain |
| DamageSlot | Disabled | Previous inline detour used a function ABI at a mid-function register site |
| StatWrite | Disabled | Not required for the player-state polling path; previous inline detour was unsafe |
| CameraZoomFOV | Disabled | Must be rebuilt as a register-correct mid hook before use |
| WorldSystem | `WORLD_SYSTEM_P1` / `P2` / `P3` | WorldSystem singleton resolution |
| AnimationEvaluator (opt-in) | `ANIM_EVALUATOR` | rcx = evaluator this-pointer (function entry), gated on `enable_experimental_hooks` |
| DragonHpProbe (opt-in, mid-hook) | `DRAGON_TIMER` | r13 = mount marker at the timer write site; dynamic HP scan, gated on `enable_experimental_hooks`. Converted from inline-hook to mid-hook because the AOB hits a mid-function `mov [r13+0x160]` write, not a function entry — the previous detour treated rcx as the marker which scanned unrelated memory |
| TeleportWaypoint (opt-in, mid-hook) | `TELEPORT_WAYPOINT` | r15 = source waypoint struct; gated on `sync_fast_travel` |

### Hooks Defined But Not Yet Installed (Need Signatures)

| Hook | Blocker |
|------|---------|
| `player_animation_hook` | Animation state offsets (0x120/0x124) are estimated, no animation write AOB found. Superseded in practice by the experimental evaluator hook (`enable_experimental_hooks`) |
| `companion_spawn_hook` | No companion spawn function signature found. CompanionHijack retries lazily on `activate()` so this isn't a functional gap |

### Documented Signatures Available But Not Yet Wired (0.2.3 audit)

These AOB constants live in `signatures::` and resolve correctly, but no
detour or sync system currently calls them. They are kept as scaffolding
because they may unlock specific co-op features when needed:

| Signature | What it gives you | Possible co-op use |
|-----------|-------------------|--------------------|
| `MAP_LOOKUP_P1` / `MAP_INSERT_P1` | Generic map dictionary primitives (from EquipHide) | Could be the foundation for a generic key-value sync (e.g. world flags, quest state) once the manager identity is known |
| `PART_INOUT_P1` / `PART_INOUT_P2` | Companion / equipment visibility transitions | Auto-trigger `CompanionHijack::activate()` when a companion becomes visible, removing the manual `spawn_remote_player()` step |
| `HP_CAPTURE_STEP1` / `HP_CAPTURE_STEP2` | Alternative path to player + horse HP base capture (from bbfox CT) | Cross-verification of our existing player HP read; could also resolve mount HP without depending on the Orcax `MOUNT_PTR_CAPTURE` |
| `CD_COMPANION_*` | Static XYZ globals, map marker capture, physics-delta teleport, camera heading (from CD Companion) | Candidate opt-in receiver-side teleport fallback and camera heading source; not wired because it is a separate physics injection path, not the native fast-travel transition |
| `DURABILITY_WRITE_PRIMARY` / `_FALLBACK` | Gear durability writes (from Orcax) | Co-op gear durability sync if both players want shared damage/repair |
| `REPUTATION_SET_MIN` / `REPUTATION_NO_DEC` | Reputation gain setters (from bbfox CT) | Single-player progression — not co-op-relevant |
| `CONTRIBUTION_GAIN` / `_MAP`, `TRUST_GIFT` / `_SHOP`, `INVENTORY_SLOT_READ` | Single-player progression hooks | Not co-op-relevant |
| `INF_ARROW`, `ITEM_REMOVE_NO_DEC`, `FAST_ENEMY_KILL`, `FAST_FRIENDSHIP` | Single-player cheats | Not co-op-relevant |
| `BASE_SUPPLY`, `SELECTED_ITEM`, `ARCHERY_CONTEST`, `CONTEST_SCORE`, `ITEM_HIGHLIGHT` | Misc single-player UX | Not co-op-relevant |

### Documented Offsets Available But Not Yet Used

Same logic — these `offsets::*` constants are defined but no .cpp file
references them. Mostly single-player progression that doesn't need
syncing across peers:

- `ItemEntry::*` (5 fields) — inventory item layout
- `BaseSupply::*` (6 fields) — base supply economy
- `Contribution::*`, `Trust::VALUE`, `Reputation::*`, `ResistanceAttrs::*` — progression
- `Companion::MODEL_ID`, `Companion::IS_ACTIVE` — only `AI_CONTROLLER` and `ANIM_STATE` are read by `CompanionHijack`
- `AnimationEvaluator::COMBAT_FLAG` — declared but the evaluator hook only reads `STATE` / `BLEND_WEIGHT`
- `Enemy::AGGRO_TARGET` — only `STATE` is synced via `EnemySync`

## Step 5: Extract Signatures

Once you've found a function in x64dbg:

1. Copy the first ~20 bytes of the function
2. Replace varying bytes (addresses, offsets) with `?`
3. Test the signature in Cheat Engine's AOB scan to verify uniqueness

Example:
```
Actual bytes:  48 89 5C 24 08 57 48 83 EC 20 48 8B D9 E8 AB CD EF 01
Signature:     48 89 5C 24 ? 57 48 83 EC 20 48 8B D9 E8 ? ? ? ?
```

## Step 6: Update the Code

Edit `include/cdcoop/core/game_structures.h`:
- Fill in any remaining `constexpr uint32_t` offset values
- Add new signatures to the `signatures` namespace

Edit `src/core/hooks.cpp`:
- Add `create_hook()` calls with verified signatures

## Useful Tools

### Offset Scanner (included)
Run `tools/offset_scanner.py` with Cheat Engine's Python API to automate offset discovery.

### PAZ Unpacker
The [crimson-desert-unpacker](https://github.com/lazorr410/crimson-desert-unpacker) can extract game configuration XMLs from PAZ archives. These may contain entity structure definitions, animation IDs, and other useful data.

### PAZ Encryption Spec (from community RE)
- PAMT index: filename, offset, comp_size, orig_size, flags
- Compression flags (bits 16-19): 0=none, 2=LZ4, 3=proprietary, 4=zlib
- ChaCha20 with Jenkins hashlittle key derivation (init `0xC5EDE`)
- 32-byte key: 8 chunks of `(seed ^ 0x60616263) ^ delta[i]`

## Known Offsets and Signatures (April/May 2026)

The following offsets and signatures have been sourced from the active modding community. All are integrated in `include/cdcoop/core/game_structures.h`.

### Stat Entry Structure (Health / Stamina / Spirit)
From FearLess CE community and CrimsonDesert-player-status-modifier:
- Health, stamina, and spirit are **8-byte values** (int64, displayed value * 1000)
- All three share the same write opcode
- Stat type at +0x00 (0=Health, 17=Stamina, 18=Spirit)
- Current value at +0x08, Max value at +0x18
- Stats component is at actor base + 0x58, entries are 16 bytes each

### Actor Structure (from EquipHide RE work)
- VTable at +0x00
- Component link / AI controller at +0x48
- Body slots (child actors) at +0xD0 through +0x108 (8 slots, 8 bytes each)
- Body -> VisCtrl chain: +0x68 -> +0x40 -> +0xE8
- Actor type detection: +0x48 -> +0x08 -> +0x88 -> +0x01 (type byte)

### WorldSystem Chain
- WorldSystem is a singleton found via RIP-relative pointer in signature scan
- ActorManager at WorldSystem + 0x30
- ClientChildOnlyInGameActor at ActorManager + 0x50
- ClientUserActor wrapper at ActorManager + 0x58
- Current player data is reached through ChildActor +0x68 -> component table
  +0x20 -> ClientStatusActorComponent +0x18

### Position Data (Verified Read Path, build 25050808)
```
component table -> +0x1A0 -> ClientTransformSyncActorComponent
    rotation quaternion: +0x62C
    position XYZ:        +0x63C
```
- Position is float32 XYZ and changed consistently with controlled player movement.
- The quaternion at +0x62C remained normalized and changed with facing direction.
- Remote companion pose writes are disabled. Direct writes race AI, and clearing the AI component to prevent that race caused a delayed game crash.
- The legacy `actor -> +0x40 -> +0x08 -> +0x248 -> +0x90` path does not resolve on build 25050808.
- Position write instruction at `CrimsonDesert.exe+36ADB8C`: `41 0F 11 45 00` (movups [r13+00], xmm0)

**Hook-time direct access** (via PositionHeightAccess sig):
- r13 = float* pointing directly at the position vector

### Static Player Base Pointer (from bbfox0703 CT, v1.01.03 legacy)
```
Player = CrimsonDesert.exe+5CC7618
```
Chain: `[base+0x18] -> +0xA0 -> +0xD0 -> {character_slot} -> +0x20 -> +0x18 -> +0x58 -> {stat}`

### Stamina/Spirit Offset Note
The May 2026 bbfox CT v29 / OpenCheatTables layout moved the active stamina and spirit current values to `stats_component+0x518` and `stats_component+0x5A8`. That means their entry starts are `health_entry+0x510` and `health_entry+0x5A0`.

The Orcax player-status-modifier source still defines the older `kStaminaEntryOffsetFromHealth = 0x480` and `kSpiritEntryOffsetFromHealth = 0x510`, which correspond to `stats_component+0x488` and `stats_component+0x518` after adding the `+0x08` current-value field. Runtime readers now try the current layout first, validate `StatEntry::TYPE`, and fall back to the legacy layout for older game builds.

### Reputation System (from bbfox0703 CT, v1.01.03)
Fully integrated in `game_structures.h`:
- Gain setter at `CrimsonDesert.exe+1B4C98E`
- No-decrease at `CrimsonDesert.exe+1B4C971`
- Current at `[rax+0x08]`, minimum at `[rax+0x04]`, delta at `[rax+0x0C]`

### Resistance Attributes (from bbfox0703 CT v1.0.6)
Fully integrated:
- Injection at `CrimsonDesert.exe+12D1DFC`
- Stride 0x20 per entry, scale 50M per level (max level 15)
- ATK +0x000, DEF +0x020, Cold +0x340, Fire +0x360, Lightning +0x3A0

### Camera Zoom/FOV (from Send's CE table, v1.00.03)
```asm
movss [r12+0xD8], xmm0    ; F3 41 0F 11 84 24 D8 00 00 00
```
- `r12` = camera struct base pointer, offset `0xD8` = zoom/FOV float
- Full camera struct beyond +0xD8 is unmapped - camera mods use PAZ XML (`playercamerapreset.xml`)

### Key Signatures
See `include/cdcoop/core/game_structures.h` namespace `signatures` for the full list of 40+ patterns.

## New Leads from Community Research (April 2026)

### CDAnimCancel / CDGuardCancel (Animation System RE)
**Original**: https://github.com/faisalkindi/CDAnimCancel — now 404. The
findings below were captured before the original repo went down.

**Successor (April 2026, same author)**: https://github.com/faisalkindi/CDGuardCancel
- Re-publishes `tools/extract_paac.py` and `patch_transitions.py`
- Documents the 3-layer guard block system (branchset, timeline, guard sub-blocks)
- 622KB bytecode section controls attack sub-state transitions
- Confirms the same evaluator function entry below

Captured findings (still valid against current builds):
- **Critical finding**: The animation system uses **.paac action chart files** (PA Action Chart binary), NOT simple actor struct fields. Memory scanning for animation state offsets was "unsuccessful -- state hidden behind unknown pointers"
- **Evaluator function**: `CrimsonDesert.exe+2712090` - gate that returns 0/1 for animation transitions. AOB: `0F 28 CE 48 89 4C 24 20 48 8B CB E8`
- **Guard activation**: Entry at `+2712330`, AOB: `48 8B C4 48 89 58 10 48 89 68 18 48 89 70 20 57 41 54 41 55 41 56 41 57 48 83 EC 60`
- **Candidate array struct**: `[rbx+0x40]` = array ptr, `[rbx+0x48]` = count, `[rbx+0x68]` = current state, `[rbx+0x6A]` = active flag (0x01)
- **Each transition candidate**: 0xD0 (208) bytes
- **Includes `extract_paac.py`**: 1,368-line binary format decoder for .paac files (428 animation .paa paths in sword_upper.paac alone)
- **Implication**: Our estimated actor+0x120/0x124 animation offsets are likely the wrong approach. Real animation state lives in deserialized .paac runtime objects
- **Themida note**: CRC protection reverts executable code patches silently. Only system DLL hooks and SafetyHook work

### bbfox0703 Cheat Table (Open Source, 220+ entries)
**GitHub**: https://github.com/bbfox0703/Mydev-Cheat-Engine-Tables/blob/main/Crimson%20Desert/CrimsonDesert.CT
- Most detailed open-source CT available. Reputation, friendship, durability, resistance attributes all extracted and integrated.

### UltimateCameraMod (150+ camera states in XML)
**GitHub**: https://github.com/FitzDegenhub/UltimateCameraMod
- Full PAZ decrypt/repack pipeline with 150+ camera state definitions
- Could help map the runtime camera struct beyond +0xD8

### pycrimson Python Library (PAZ extraction)
**GitHub**: https://github.com/LukeFZ/pycrimson
- PAZ/PAMT extraction, DDS decompression, save decrypt/re-encrypt, reflection-based deserializer
- Could programmatically extract animation data from PAZ archives

### Teleport / fast-travel (bbfox0703 CT, 2026-04-18 research pass)
See `docs/RESEARCH_2026-04-18.md` for full derivation. Summary:

- **Waypoint-apply hook** at `CrimsonDesert.exe+0xAB5594`
  - AOB: `F2 41 0F 11 86 D8 00 00 00 ?? ?? ?? ?? 41 89 86 E0 00 00 00`
  - `r14` = destination entity, `r15` = source waypoint
  - Entity layout: `+0xD8` = X,Y double; `+0xE0` = Z float
  - Source layout: `+0x1C` = X,Y double; `+0x24` = Z float
- **worldOffset global** — entity positions are stored in entity-local
  space; world pos = local + worldOffset. Resolved via RIP-relative AOB:
  `0F ?? ?? ?? ?? ?? ?? 0F 11 ?? 90 00 00 00 E8 ?? ?? ?? ?? F3`
  (pos=3, len=7). Use site at `+0x278C6C8`.
- **Entity velocity write** at `+0x2791A16` — `movups [entity+0x1B0], xmm1`
  packed as `{X, Z_height, Y, W}`.

Integrated as `namespace Teleport` in `include/cdcoop/core/game_structures.h`.

**0.2.1 update**: capture mid-hook is now installed (opt-in via `sync_fast_travel`). Host detour at `+0xAB5594` reads `[r15+0x00]` (waypoint type), `[r15+0x1C..0x28]` (X/Y/Z), and broadcasts `TELEPORT_TRIGGER`. Receive-side is intentionally log-only — the apply / area-transition function isn't identified yet. The 30Hz position broadcast pulls the companion entity along once the host arrives.

### CD Companion map / physics teleport leads (leandrodiogenes, May 2026)

CD Companion is a public real-time map overlay that reads Crimson Desert
position data and teleports through a physics-delta hook instead of calling
the game's native fast-travel apply function. These are integrated as
`signatures::CD_COMPANION_*` scaffolding:

- **Entity base capture**: `48 83 EC 50 48 8B F9 48 8B 91 30 11 00 00`, hook offset `+7`; original instruction reads `[rcx+0x1130]` and `rcx` is the entity base.
- **Static XYZ globals**: `C5 FB 11 05 ?? ?? ?? ?? 8B 44 24 28 89 05 ?? ?? ?? ??`; first RIP target is X/Y, second is Z.
- **Map destination capture**: `C5 FB 10 07 C5 FB 11 02 8B 47 08 89 42 08`, hook offset `+4`, captures in-game map marker X/Y/Z.
- **Physics delta hook**: `0F 28 C6 F3 45 0F 5C C8`, confirmed by `41 0F 58 45 00 41 0F 11 45 00` eight bytes later. CD Companion queues `target - current` into this path to move the player.
- **Camera heading hook**: `C4 C1 7A 11 97 A4 04 00 00 C5 78 2F CE`; writes signed camera degrees to `r15+0x4A4`.

This is useful, but it is not the same as the native area-transition apply
function. Treat it as a future opt-in receiver-side fallback, not as a drop-in
replacement for `WorldSync::on_remote_teleport()`.

### Mount pointer / stamina (Orcax-1399 player-status-modifier scanner, 2026-04-18 research pass)
- **Mount pointer capture**: `48 8B C7 49 8B 7D 08 80 BF 94 00 00 00 00 0F 85 ?? ?? ?? ?? 48 8B 47 68 48 8B 48 20 48 83 C1 30 E8 ?? ?? ?? ?? 66 83 B8 E4 00 00 00 00` (offset 20). Function entry that walks the mount pointer chain ending at `+0x30`. Useful as an alternative to the static-base + chain resolution we currently use.
- **Mount stamina ("AB00") access**: `0F B7 D7 49 8B CE E8 ?? ?? ?? ?? 48 8B F0 48 85 DB 74 ?? 33 C0 66 89 44 24 20 38 46 53` (offset 11). Hook site that reads/writes the mount stamina stat.

Both were added as `signatures::MOUNT_PTR_CAPTURE` and `signatures::MOUNT_STAMINA_ACCESS` in 0.2.1. `MOUNT_PTR_CAPTURE` is now wired by `MountSync`; `MOUNT_STAMINA_ACCESS` remains available for a future direct stamina hook, but the current overlay/network path polls StatEntry data and validates current-vs-legacy layouts by type id.

### Other Orcax-1399 AOBs (not yet integrated, listed for completeness)
The Orcax scanner.cpp also publishes AOBs we don't currently need but are documented here in case a future mod wants them: dragon village-summon jump, dragon flying-restrict write, dragon roof-restrict test, affinity gain prepare / current write, abyss-gear durability delta, spirit signed-delta, item gain write. See [Orcax-1399/CrimsonDesert-player-status-modifier scanner.cpp](https://github.com/Orcax-1399/CrimsonDesert-player-status-modifier/blob/master/src/scanner.cpp).

### Dragon HP candidate (bbfox0703 CT, 2026-04-18 research pass)
Reading the surrounding code at the dragon-timer injection site
(`+0x339D8CB`) produced a contiguous field map for the dragon-mount
struct at `r13`. `+0xD8` is the only standalone 4-byte float between
the `+0xC0` coord block and the `+0xE0..+0xEC` stat quad, and is
written with `xmm8` — same convention as horse HP init. Added as
`Mount::DRAGON_HP_PREFERRED_OFFSET = 0xD8`; the existing dynamic scan
still runs as fallback. Needs in-game read-back to confirm it matches
the visible HP bar.

### Save Editor Data (NattKh/CRIMSON-DESERT-SAVE-EDITOR)
- 2,262 item templates, 633 quests, 5,450 missions, 5,500+ knowledge entries
- Mount/vehicle respawn timers are editable - suggests a vehicle manager struct exists

### BDO Reverse Engineering (Engine Lineage)
**URL**: https://secret.club/2019/01/24/reverse-engineering-bdo-2.html
- BDO (same engine family) documented actor proxy class hierarchies
- Check if Crimson Desert binary has RTTI symbols

## Community Resources

- [CrimsonDesert-player-status-modifier](https://github.com/Orcax-1399/CrimsonDesert-player-status-modifier) - Player stats, position, damage hooks
- [CDAnimCancel](https://github.com/faisalkindi/CDAnimCancel) - Animation system RE, .paac format parser, evaluator AOBs
- [CrimsonDesertTools](https://github.com/tkhquang/CrimsonDesertTools) - WorldSystem, actor structure, equipment visibility (v0.5.1, April 8 2026)
- [DetourModKit](https://github.com/tkhquang/DetourModKit) - AOB scanning framework
- [CrimsonDesertModdingResearch](https://github.com/marvelmaster/CrimsonDesertModdingResearch) - Address value table
- [JustSkip](https://github.com/wealdly/JustSkip) - Combat state flag, cutscene skip hooks
- [CD Companion](https://github.com/leandrodiogenes/cd-companion) - Static XYZ, map marker, camera heading, physics-delta teleport hooks
- [Nexus Mods - Crimson Desert](https://www.nexusmods.com/crimsondesert) - 256+ mods
- [OpenCheatTables Crimson Desert thread](https://opencheattables.com/viewtopic.php?t=1836) - bbfox CT discussion and public stat-layout updates
- [FearLess CE Thread](https://fearlessrevolution.com/viewtopic.php?t=38679) - Active offset research (16+ pages)

## Game Version Tracking

Offsets WILL change with game patches. Maintain a version table:

| Game Version | Position Sig | Stats Sig | WorldSystem Sig | Notes |
|-------------|-------------|-----------|-----------------|-------|
| 1.00.02     | Verified    | Verified  | Verified        | Launch version |
| 1.00.03     | Verified    | Verified  | Verified        | March 25 patch |
| 1.01.03     | Verified    | Verified  | Verified        | March hotfix, legacy stat spacing |
| May 2026 public tables | Unchanged in public sources | Verified via bbfox CT v29 | Unchanged in public sources | Stamina/spirit entry deltas moved to +0x510 / +0x5A0 from health entry |
| Steam build 25050808 | Read verified at TransformSync +0x63C; remote write disabled | Health read verified; companion health writes disabled; stamina/spirit need refresh | Verified | Live-tested locally. Player and companion discovery chains are verified. Direct pose writes race AI; clearing the AI component caused a delayed crash. |

Use the signature scanner to automatically find updated offsets after patches rather than hardcoding addresses.
