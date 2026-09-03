#include <cdcoop/core/hooks.h>
#include <cdcoop/core/memory.h>
#include <cdcoop/core/game_structures.h>
#include <cdcoop/core/config.h>
#include <cdcoop/network/session.h>
#include <cdcoop/network/packet.h>
#include <cdcoop/sync/player_sync.h>
#include <cdcoop/sync/enemy_sync.h>
#include <cdcoop/sync/world_sync.h>
#include <cdcoop/player/companion_hijack.h>
#include <cdcoop/player/player_manager.h>
#include <cdcoop/ui/overlay.h>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <Windows.h>

#include <atomic>
#include <bit>
#include <cmath>
#include <cstring>

namespace cdcoop {

namespace {
static_assert(std::atomic<uint64_t>::is_always_lock_free);
static_assert(std::atomic<uintptr_t>::is_always_lock_free);
static_assert(std::atomic<uint32_t>::is_always_lock_free);

std::atomic<uint64_t> companion_position_probe_hits{0};
std::atomic<uint64_t> companion_position_probe_matches{0};
std::atomic<uint64_t> companion_position_probe_coordinate_matches{0};
std::atomic<uint64_t> companion_position_override_writes{0};
std::atomic<uintptr_t> companion_position_probe_target{0};
std::atomic<uintptr_t> companion_position_probe_last_r13{0};
std::atomic<uintptr_t> companion_position_probe_last_rbx{0};
std::atomic<uintptr_t> companion_position_probe_candidate_r13{0};
std::atomic<uintptr_t> companion_position_probe_candidate_rbx{0};
std::atomic<uint32_t> companion_position_probe_candidate_streak{0};
std::atomic<uintptr_t> companion_position_probe_locked_r13{0};
std::atomic<uintptr_t> companion_position_probe_locked_rbx{0};
std::atomic<uint64_t> companion_position_probe_target_epoch{0};
std::atomic<uint64_t> companion_position_probe_locked_epoch{0};
std::atomic<uint32_t> companion_position_probe_validation_lease{0};
std::atomic<uint32_t> companion_position_probe_reference_sequence{0};
std::atomic<uint32_t> companion_position_probe_reference_x{0};
std::atomic<uint32_t> companion_position_probe_reference_y{0};
std::atomic<uint32_t> companion_position_probe_reference_z{0};
std::atomic<uint32_t> companion_position_probe_reference_ready{0};
std::atomic<uint64_t> companion_position_probe_reference_epoch{0};
std::atomic<uint64_t> companion_position_probe_reference_timestamp{0};
std::atomic<uint32_t> companion_position_probe_vertical_offset{0};
std::atomic<uint32_t> companion_position_override_sequence{0};
std::atomic<uint32_t> companion_position_override_x{0};
std::atomic<uint32_t> companion_position_override_y{0};
std::atomic<uint32_t> companion_position_override_z{0};
std::atomic<uint32_t> companion_position_override_active{0};
std::atomic<uint64_t> companion_position_override_epoch{0};
std::atomic<uint32_t> companion_position_test_started{0};
std::atomic<uint64_t> companion_position_test_expires_at{0};
std::atomic<uint64_t> companion_position_test_epoch{0};
std::atomic<uint32_t> companion_position_test_x{0};
std::atomic<uint32_t> companion_position_test_y{0};
std::atomic<uint32_t> companion_position_test_z{0};
std::atomic<uint32_t> companion_position_override_allowed{0};
std::atomic<uint32_t> companion_position_test_allowed{0};
std::atomic_flag companion_position_state_gate = ATOMIC_FLAG_INIT;
std::atomic<uint32_t> companion_position_state_writer_pending{0};

struct AtomicPosition {
    std::atomic<uint32_t>& sequence;
    std::atomic<uint32_t>& x;
    std::atomic<uint32_t>& y;
    std::atomic<uint32_t>& z;
};

bool publish_position(AtomicPosition position, float x, float y, float z) noexcept {
    uint32_t sequence = position.sequence.load(std::memory_order_acquire);
    bool acquired = false;
    for (int attempt = 0; attempt < 3; ++attempt) {
        if ((sequence & 1U) != 0) return false;
        if (position.sequence.compare_exchange_weak(
                sequence, sequence + 1, std::memory_order_acq_rel)) {
            acquired = true;
            break;
        }
    }
    if (!acquired) return false;
    position.x.store(std::bit_cast<uint32_t>(x), std::memory_order_relaxed);
    position.y.store(std::bit_cast<uint32_t>(y), std::memory_order_relaxed);
    position.z.store(std::bit_cast<uint32_t>(z), std::memory_order_relaxed);
    position.sequence.store(sequence + 2, std::memory_order_release);
    return true;
}

bool load_position(AtomicPosition position, float& x, float& y, float& z) noexcept {
    for (int attempt = 0; attempt < 3; ++attempt) {
        const uint32_t before = position.sequence.load(std::memory_order_acquire);
        if ((before & 1U) != 0) continue;
        const uint32_t x_bits = position.x.load(std::memory_order_relaxed);
        const uint32_t y_bits = position.y.load(std::memory_order_relaxed);
        const uint32_t z_bits = position.z.load(std::memory_order_relaxed);
        if (position.sequence.load(std::memory_order_acquire) == before) {
            x = std::bit_cast<float>(x_bits);
            y = std::bit_cast<float>(y_bits);
            z = std::bit_cast<float>(z_bits);
            return true;
        }
    }
    return false;
}

AtomicPosition companion_reference() noexcept {
    return {companion_position_probe_reference_sequence,
            companion_position_probe_reference_x,
            companion_position_probe_reference_y,
            companion_position_probe_reference_z};
}

AtomicPosition companion_override() noexcept {
    return {companion_position_override_sequence,
            companion_position_override_x,
            companion_position_override_y,
            companion_position_override_z};
}

void lock_companion_position_state() noexcept {
    companion_position_state_writer_pending.fetch_add(1, std::memory_order_acq_rel);
    while (companion_position_state_gate.test_and_set(std::memory_order_acquire)) {
        YieldProcessor();
    }
    companion_position_state_writer_pending.fetch_sub(1, std::memory_order_release);
}

void unlock_companion_position_state() noexcept {
    companion_position_state_gate.clear(std::memory_order_release);
}

struct CompanionPositionStateGuard {
    bool acquired = !companion_position_state_gate.test_and_set(
        std::memory_order_acquire);

    ~CompanionPositionStateGuard() {
        if (acquired) unlock_companion_position_state();
    }
};

void revoke_companion_position_mapping_unlocked() noexcept {
    companion_position_probe_candidate_r13.store(0, std::memory_order_relaxed);
    companion_position_probe_candidate_rbx.store(0, std::memory_order_relaxed);
    companion_position_probe_candidate_streak.store(0, std::memory_order_relaxed);
    companion_position_probe_locked_epoch.store(0, std::memory_order_release);
    companion_position_probe_locked_r13.store(0, std::memory_order_relaxed);
    companion_position_probe_locked_rbx.store(0, std::memory_order_relaxed);
    companion_position_probe_validation_lease.store(0, std::memory_order_relaxed);
    companion_position_test_started.store(0, std::memory_order_relaxed);
    companion_position_test_expires_at.store(0, std::memory_order_relaxed);
    companion_position_test_epoch.store(0, std::memory_order_relaxed);
}

void clear_companion_position_override_unlocked() noexcept {
    companion_position_override_active.store(0, std::memory_order_release);
    companion_position_override_epoch.store(0, std::memory_order_relaxed);
}

uint64_t replace_companion_position_target_unlocked(uintptr_t target) noexcept {
    if (target == 0 &&
        companion_position_probe_target.load(std::memory_order_acquire) == 0) {
        companion_position_probe_reference_ready.store(0, std::memory_order_release);
        companion_position_probe_reference_epoch.store(0, std::memory_order_relaxed);
        companion_position_probe_reference_timestamp.store(0, std::memory_order_relaxed);
        revoke_companion_position_mapping_unlocked();
        clear_companion_position_override_unlocked();
        return companion_position_probe_target_epoch.load(std::memory_order_acquire);
    }

    companion_position_probe_target.store(0, std::memory_order_release);
    const uint64_t target_epoch =
        companion_position_probe_target_epoch.fetch_add(
            1, std::memory_order_acq_rel) + 1;
    companion_position_probe_reference_ready.store(0, std::memory_order_release);
    companion_position_probe_reference_epoch.store(0, std::memory_order_relaxed);
    companion_position_probe_reference_timestamp.store(0, std::memory_order_relaxed);
    revoke_companion_position_mapping_unlocked();
    clear_companion_position_override_unlocked();
    companion_position_probe_target.store(target, std::memory_order_release);
    return target_epoch;
}

bool valid_position(float x, float y, float z) noexcept {
    constexpr float kMaxCoordinate = 10'000'000.0f;
    return std::isfinite(x) && std::isfinite(y) && std::isfinite(z) &&
           std::abs(x) <= kMaxCoordinate && std::abs(y) <= kMaxCoordinate &&
           std::abs(z) <= kMaxCoordinate;
}

// Dedicated logger for world-system probe telemetry. Created lazily on
// first call to scan_world_system_siblings() so we don't spawn the file
// when the probe is disabled.
std::shared_ptr<spdlog::logger> worldprobe_logger() {
    static std::shared_ptr<spdlog::logger> logger = []() {
        try {
            auto l = spdlog::basic_logger_mt(
                "worldprobe", self_module_dir() + "cdcoop_world_probe.log", true);
            l->set_pattern("%Y-%m-%d %H:%M:%S | %v");
            l->set_level(spdlog::level::info);
            return l;
        } catch (const std::exception& e) {
            spdlog::warn("Failed to create worldprobe logger: {}", e.what());
            return std::shared_ptr<spdlog::logger>{};
        }
    }();
    return logger;
}
} // namespace

HookManager& HookManager::instance() {
    static HookManager inst;
    return inst;
}

bool HookManager::initialize() {
    if (initialized_) return true;

    // Find the main game module (CrimsonDesert.exe or similar)
    HMODULE game_module = GetModuleHandleA(nullptr);
    if (!game_module) {
        spdlog::error("Failed to get game module handle");
        return false;
    }

    game_base_ = reinterpret_cast<uintptr_t>(game_module);

    // Get module size from PE header
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(game_base_);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(game_base_ + dos->e_lfanew);
    game_size_ = nt->OptionalHeader.SizeOfImage;

    spdlog::info("Game module: base=0x{:X}, size=0x{:X}", game_base_, game_size_);

    // =====================================================================
    // HOOK INSTALLATION
    // =====================================================================
    // Signatures sourced from:
    //   - CrimsonDesert-player-status-modifier (Orcax-1399)
    //   - CrimsonDesertTools/EquipHide (tkhquang)
    // Game version: v1.00.02 / v1.01.03 (March 2026)
    // =====================================================================

    status_ = {};
    core_signature_found_ = false;

    // Try a pair of (primary, fallback) signatures for the same logical hook,
    // and record the outcome in HookStatus.
    auto try_hook_pair = [&](const char* primary, const char* fallback,
                              const char* name, auto detour,
                              SafetyHookInline& slot, bool required) -> bool {
        if (create_hook(primary, name, detour, slot)) {
            spdlog::info("Installed {} hook (primary sig)", name);
            status_.installed++;
            status_.installed_names.emplace_back(std::string(name) + " (primary)");
            return true;
        }
        if (fallback && create_hook(fallback, name, detour, slot)) {
            spdlog::info("Installed {} hook (fallback sig)", name);
            status_.installed++;
            status_.installed_names.emplace_back(std::string(name) + " (fallback)");
            return true;
        }
        if (required) {
            spdlog::error("Failed to install {} hook", name);
        } else {
            spdlog::warn("Failed to install {} hook (optional)", name);
        }
        status_.failed++;
        status_.failed_names.emplace_back(name);
        return false;
    };

    // --- WorldSystem singleton resolution (from EquipHide) ---
    resolve_world_system();

    // --- Player base pointer resolution (from bbfox0703 CT, v1.01.03) ---
    if (!get_runtime_offsets().player_resolved) {
        resolve_player_base();
    }

    // A matching core signature with a null runtime pointer is normal on the
    // main menu. Only fail closed when none of the version-specific anchors
    // match this executable at all.
    if (!core_signature_found_) {
        status_.unsupported_build = true;
        spdlog::error("No core signatures match this game build. Game-memory hooks "
                      "and co-op sessions are disabled until offsets are updated.");
        initialized_ = true;
        return true;
    }

    // --- ChildActor vtable resolution (from EquipHide, 3 fallback sigs) ---
    // Populates rt.child_actor_vtbl. Used by is_child_actor() to filter
    // child entities (companions, summons) from regular enemies during
    // ActorManager body-slot iteration. Best-effort — failure is non-fatal.
    resolve_child_actor_vtbl();

    // These AOBs identify mid-function register sites, not callable function
    // entries. The old inline detours used the C ABI and could corrupt register
    // state. Position polling already uses the verified pointer chain; the
    // remaining features stay disabled until they are rebuilt as mid hooks and
    // field-tested against a supported game build.
    spdlog::warn("Unsafe legacy Position/Damage/Stat/Camera hooks are disabled");

    // --- Game tick: DX12 Present drives the update loop ---
    spdlog::info("Game tick: using DX12 Present hook as frame tick (no dedicated sig needed)");

    // --- Experimental hooks (opt-in via config) ---
    // Kept behind a flag because CDAnimCancel's evaluator AOB and the dragon
    // HP dynamic scan are both third-party research that has not been
    // verified against this specific build.
    auto& cfg = get_config();
    companion_position_override_allowed.store(
        cfg.enable_companion_position_override ? 1U : 0U,
        std::memory_order_relaxed);
    companion_position_test_allowed.store(
        cfg.test_companion_position_write ? 1U : 0U,
        std::memory_order_relaxed);
    if (cfg.diagnose_companion_position_write ||
        cfg.enable_companion_position_override || cfg.test_companion_position_write) {
        auto position_sig = sig_scan(signatures::POSITION_PRIMARY,
                                     "CompanionPositionProbe");
        uintptr_t target = position_sig
            ? position_sig.address + signatures::POSITION_PRIMARY_OFFSET
            : 0;
        if (!position_sig) {
            position_sig = sig_scan(signatures::POSITION_FALLBACK,
                                    "CompanionPositionProbeFallback");
            if (position_sig) {
                target = position_sig.address + signatures::POSITION_FALLBACK_OFFSET;
            }
        }

        constexpr uint8_t kExpectedPositionWrite[] = {
            0x0F, 0x28, 0xC6, 0xF3, 0x45, 0x0F, 0x5C, 0xC8,
            0x41, 0x0F, 0x58, 0x45, 0x00,
            0x41, 0x0F, 0x11, 0x45, 0x00,
        };
        const bool target_in_module = target >= game_base_ + 13 &&
                                      target <= game_base_ + game_size_ - 5;
        const bool bytes_match = target_in_module &&
            is_readable(target - 13, sizeof(kExpectedPositionWrite)) &&
            std::memcmp(reinterpret_cast<const void*>(target - 13),
                        kExpectedPositionWrite, sizeof(kExpectedPositionWrite)) == 0;
        if (position_sig && bytes_match &&
            create_mid_hook(target, hooks::companion_position_probe_detour,
                            hooks::companion_position_probe_hook)) {
            spdlog::info("Installed CompanionPositionProbe at 0x{:X} "
                         "(remote_override={}, one_shot_test={})",
                         target, cfg.enable_companion_position_override,
                         cfg.test_companion_position_write);
            status_.installed++;
            status_.installed_names.emplace_back("CompanionPositionProbe (correlated mid)");
        } else {
            spdlog::warn("Failed to install CompanionPositionProbe (signature/bytes mismatch)");
            status_.failed++;
            status_.failed_names.emplace_back("CompanionPositionProbe");
        }
    }

    if (cfg.enable_experimental_hooks) {
        spdlog::info("Experimental hooks enabled (config.enable_experimental_hooks = true)");

        try_hook_pair(signatures::ANIM_EVALUATOR, nullptr,
                      "AnimationEvaluator",
                      hooks::animation_evaluator_detour, hooks::animation_evaluator_hook, false);

        // Dragon HP probe — mid-hook (r13 = mount marker at the timer write site)
        auto dragon_sig = sig_scan(signatures::DRAGON_TIMER, "DragonHpProbe");
        if (dragon_sig && create_mid_hook(dragon_sig.address, hooks::dragon_hp_probe_detour,
                                          hooks::dragon_hp_probe_hook)) {
            spdlog::info("Installed DragonHpProbe mid-hook");
            status_.installed++;
            status_.installed_names.emplace_back("DragonHpProbe (mid)");
        } else {
            spdlog::warn("Failed to install DragonHpProbe hook (optional)");
            status_.failed++;
            status_.failed_names.emplace_back("DragonHpProbe");
        }
    } else {
        spdlog::debug("Experimental hooks disabled (stable mode)");
    }

    // --- Map waypoint / fast-travel mid-hook (opt-in) ---
    // Captures the host's fast-travel target so we can notify the peer.
    // Mid-hook so the detour can read r15 (source waypoint struct) directly.
    if (cfg.sync_fast_travel) {
        auto sig = sig_scan(signatures::TELEPORT_WAYPOINT, "TeleportWaypoint");
        if (sig && create_mid_hook(sig.address, hooks::teleport_waypoint_detour,
                                    hooks::teleport_waypoint_hook)) {
            spdlog::info("Installed TeleportWaypoint mid-hook");
            status_.installed++;
            status_.installed_names.emplace_back("TeleportWaypoint (mid)");
        } else {
            spdlog::warn("Failed to install TeleportWaypoint hook");
            status_.failed++;
            status_.failed_names.emplace_back("TeleportWaypoint");
        }
    }

    // --- Mount pointer capture mid-hook (opt-in) ---
    // Hook offset +20 inside MOUNT_PTR_CAPTURE places us right before
    // `mov rax, [rdi+0x68]` — rdi is the mount entity this-pointer.
    // MountSync polls HP/stamina off this capture once per tick.
    if (cfg.sync_mount_state) {
        auto sig = sig_scan(signatures::MOUNT_PTR_CAPTURE, "MountPtrCapture");
        if (sig) {
            uintptr_t target = sig.address + signatures::MOUNT_PTR_CAPTURE_OFFSET;
            if (create_mid_hook(target, hooks::mount_ptr_capture_detour,
                                hooks::mount_ptr_capture_hook)) {
                spdlog::info("Installed MountPtrCapture mid-hook");
                status_.installed++;
                status_.installed_names.emplace_back("MountPtrCapture (mid)");
            } else {
                spdlog::warn("Failed to install MountPtrCapture hook");
                status_.failed++;
                status_.failed_names.emplace_back("MountPtrCapture");
            }
        } else {
            spdlog::warn("MountPtrCapture AOB not found");
            status_.failed++;
            status_.failed_names.emplace_back("MountPtrCapture");
        }
    }

    // --- WorldSystem sibling probe (opt-in) ---
    if (cfg.dump_world_system_probe && get_runtime_offsets().world_system_resolved) {
        scan_world_system_siblings();
    }

    spdlog::info("Hook installation complete: {} installed, {} failed",
                 status_.installed, status_.failed);
    if (status_.failed > 0) {
        spdlog::warn("Some hooks failed - game version may have changed.");
        spdlog::warn("Try updating signatures. See docs/REVERSE_ENGINEERING.md");
        for (const auto& name : status_.failed_names) {
            spdlog::warn("  failed: {}", name);
        }
    }

    initialized_ = true;
    return true;
}

void HookManager::shutdown() {
    if (!initialized_) return;

    hooks::set_companion_position_probe_target(0);
    // SafetyHook automatically restores original code when hooks are destroyed
    hooks::companion_position_probe_hook = {};
    hooks::player_animation_hook = {};
    hooks::damage_calc_hook = {};
    hooks::companion_spawn_hook = {};
    hooks::world_state_hook = {};
    hooks::camera_hook = {};
    hooks::animation_evaluator_hook = {};
    hooks::dragon_hp_probe_hook = {};
    hooks::teleport_waypoint_hook = {};
    hooks::mount_ptr_capture_hook = {};
    companion_position_override_allowed.store(0, std::memory_order_release);
    companion_position_test_allowed.store(0, std::memory_order_release);

    initialized_ = false;
    spdlog::info("All hooks removed");
}

void HookManager::scan_world_system_siblings() {
    auto& rt = get_runtime_offsets();
    if (!rt.world_system_resolved || rt.world_probe_ran) return;

    auto logger = worldprobe_logger();
    if (!logger) {
        spdlog::warn("WorldSystem probe skipped (logger unavailable)");
        return;
    }

    logger->info("==== WorldSystem probe begin ====");
    logger->info("ws_ptr = 0x{:X} (RVA 0x{:X})",
                 rt.world_system_ptr, rt.world_system_ptr - game_base_);

    constexpr uint32_t kStart = 0x30;
    constexpr uint32_t kEnd   = 0x100;
    constexpr uint32_t kStep  = 0x08; // pointer-aligned

    // Helper: check whether an address range is within committed memory.
    auto addr_committed = [&](uintptr_t addr, size_t bytes = 1) -> bool {
        MEMORY_BASIC_INFORMATION local_mbi{};
        if (VirtualQuery(reinterpret_cast<void*>(addr), &local_mbi, sizeof(local_mbi)) == 0)
            return false;
        if (!(local_mbi.State & MEM_COMMIT)) return false;
        uintptr_t region_end = reinterpret_cast<uintptr_t>(local_mbi.BaseAddress) + local_mbi.RegionSize;
        return addr + bytes <= region_end;
    };

    // Helper: read a vtable function pointer at index i, return its RVA
    // within the game module if it lies inside, else 0.
    auto vfunc_rva = [&](uintptr_t vtable, int i) -> uintptr_t {
        uintptr_t slot_addr = vtable + static_cast<uintptr_t>(i * 8);
        if (!addr_committed(slot_addr, sizeof(uintptr_t))) return 0;
        uintptr_t fn = *reinterpret_cast<uintptr_t*>(slot_addr);
        if (fn < game_base_ || fn >= game_base_ + game_size_) return 0;
        return fn - game_base_;
    };

    // Helper: extract the MSVC-decorated RTTI class name from a vtable.
    // MSVC x64 layout:
    //   [vtable - 8]  -> RTTICompleteObjectLocator*
    //   COL + 0x0C    : pTypeDescriptor (32-bit image-base-relative offset)
    //   TypeDesc + 0x10 : decorated name, null-terminated C string (e.g.
    //                     ".?AVSomeClass@pa@@")
    // Every dereference is bounded by addr_committed + module-range checks
    // so a bad vtable just returns an empty string. The decorated name is
    // kept verbatim — even un-demangled it contains the plain class name
    // between the ".?AV" / ".?AU" prefix and the "@@" suffix, which is
    // exactly what a community submitter needs to cross-reference against
    // an IDA / Ghidra RTTI dump.
    auto rtti_class_name = [&](uintptr_t vtable, char* out, size_t out_size) {
        out[0] = '\0';
        if (out_size < 2) return;

        if (vtable < game_base_ || vtable >= game_base_ + game_size_) return;
        uintptr_t col_slot = vtable - sizeof(uintptr_t);
        if (!addr_committed(col_slot, sizeof(uintptr_t))) return;
        uintptr_t col_ptr = *reinterpret_cast<uintptr_t*>(col_slot);
        if (col_ptr < game_base_ || col_ptr + 0x10 > game_base_ + game_size_) return;

        uint32_t type_desc_rva = *reinterpret_cast<uint32_t*>(col_ptr + 0x0C);
        if (type_desc_rva == 0) return;
        uintptr_t type_desc = game_base_ + type_desc_rva;
        if (type_desc < game_base_ || type_desc + 0x20 > game_base_ + game_size_) return;

        const char* name = reinterpret_cast<const char*>(type_desc + 0x10);
        if (!addr_committed(reinterpret_cast<uintptr_t>(name), 1)) return;

        // Copy at most out_size-1 bytes, stopping at NUL or at the end of
        // committed memory. Bounded loop — never reads past the region.
        size_t i = 0;
        while (i < out_size - 1) {
            if (!addr_committed(reinterpret_cast<uintptr_t>(name + i), 1)) break;
            char c = name[i];
            if (c == '\0') break;
            // Keep printable ASCII only; if we hit a garbage byte the
            // vtable probably wasn't a real one, so give up cleanly.
            if (c < 0x20 || c > 0x7E) { out[0] = '\0'; return; }
            out[i] = c;
            i++;
        }
        out[i] = '\0';
    };

    MEMORY_BASIC_INFORMATION mbi{};
    for (uint32_t off = kStart; off < kEnd; off += kStep) {
        uintptr_t slot_addr = rt.world_system_ptr + off;
        // Bounded VirtualQuery sanity check.
        if (VirtualQuery(reinterpret_cast<void*>(slot_addr), &mbi, sizeof(mbi)) == 0)
            continue;
        if (!(mbi.State & MEM_COMMIT)) continue;

        uintptr_t sibling = *reinterpret_cast<uintptr_t*>(slot_addr);
        if (!is_valid_ptr(sibling)) continue;

        if (VirtualQuery(reinterpret_cast<void*>(sibling), &mbi, sizeof(mbi)) == 0)
            continue;
        if (!(mbi.State & MEM_COMMIT)) continue;

        uintptr_t vtable = *reinterpret_cast<uintptr_t*>(sibling);
        uintptr_t vtable_rva = (vtable >= game_base_ && vtable < game_base_ + game_size_)
                               ? vtable - game_base_ : 0;

        // First 4 vfunc RVAs are a stable behavioural fingerprint — they
        // typically include destructor + a couple of class-specific
        // virtuals. Logging them makes the probe output identifiable
        // against any RTTI dump or x64dbg symbol cross-reference.
        uintptr_t f0 = 0, f1 = 0, f2 = 0, f3 = 0;
        char class_name[128] = {};
        if (vtable_rva != 0) {
            f0 = vfunc_rva(vtable, 0);
            f1 = vfunc_rva(vtable, 1);
            f2 = vfunc_rva(vtable, 2);
            f3 = vfunc_rva(vtable, 3);
            // MSVC decorated RTTI name (e.g. ".?AVQuestManager@pa@@").
            // Most useful piece of probe telemetry: a community submitter
            // can read this directly instead of cross-referencing RVAs.
            rtti_class_name(vtable, class_name, sizeof(class_name));
        }

        logger->info("ws+0x{:02X}: sibling=0x{:X} vtable=0x{:X} (RVA 0x{:X}) "
                     "class='{}' vfuncs=[0x{:X}, 0x{:X}, 0x{:X}, 0x{:X}]",
                     off, sibling, vtable, vtable_rva,
                     class_name[0] ? class_name : "(unknown)",
                     f0, f1, f2, f3);

        // Best-effort heuristic caching. These are stored as "candidates"
        // only; the names are community guesses and may be wrong.
        if (rt.quest_manager_candidate == 0)               rt.quest_manager_candidate = sibling;
        else if (rt.cutscene_manager_candidate == 0)       rt.cutscene_manager_candidate = sibling;
        else if (rt.world_object_manager_candidate == 0)   rt.world_object_manager_candidate = sibling;
    }

    logger->info("==== WorldSystem probe end ====");
    logger->flush();
    rt.world_probe_ran = true;
}

SigScanResult HookManager::sig_scan(const std::string& pattern, const std::string& name) {
    auto addr = MemoryScanner::scan_module(pattern);
    if (addr) {
        spdlog::info("Found '{}' at 0x{:X} (RVA: 0x{:X})", name, addr, addr - game_base_);
    } else {
        spdlog::error("Failed to find '{}'", name);
    }
    return {addr, addr != 0};
}

bool HookManager::resolve_world_system() {
    auto& rt = get_runtime_offsets();

    // Try WorldSystem signature patterns (from CrimsonDesertTools EquipHide)
    // These use RIP-relative addressing to find the WorldSystem singleton

    auto try_rip_resolve = [&](const char* sig, const char* name,
                                int rip_offset, [[maybe_unused]] int rip_end) -> bool {
        auto result = sig_scan(sig, name);
        if (!result) return false;
        core_signature_found_ = true;

        // Follow the RIP-relative address
        uintptr_t rip_addr = result.address + rip_offset;
        rt.world_system_ptr = MemoryScanner::follow_rel32(rip_addr, 0);

        // is_readable, not just is_valid_ptr: if a patched build makes this
        // signature match at the wrong place, the computed RIP target can point
        // at unmapped memory, and the dereference below would fault. Fail closed
        // instead of crashing.
        if (is_readable(rt.world_system_ptr, sizeof(uintptr_t))) {
            // Dereference to get actual WorldSystem pointer
            uintptr_t ws = read_mem<uintptr_t>(rt.world_system_ptr, 0);
            if (is_valid_ptr(ws)) {
                rt.world_system_ptr = ws;
                rt.world_system_resolved = true;
                spdlog::info("WorldSystem resolved at 0x{:X}", rt.world_system_ptr);
                return resolve_player_actor();
            }
        }
        spdlog::warn("WorldSystem pointer invalid from {}", name);
        return false;
    };

    if (try_rip_resolve(signatures::WORLD_SYSTEM_P1, "WorldSystem_P1",
                        signatures::WORLD_SYSTEM_P1_RIP_OFFSET,
                        signatures::WORLD_SYSTEM_P1_RIP_END))
        return true;

    if (try_rip_resolve(signatures::WORLD_SYSTEM_P2, "WorldSystem_P2",
                        signatures::WORLD_SYSTEM_P2_RIP_OFFSET,
                        signatures::WORLD_SYSTEM_P2_RIP_END))
        return true;

    if (try_rip_resolve(signatures::WORLD_SYSTEM_P3, "WorldSystem_P3",
                        signatures::WORLD_SYSTEM_P3_RIP_OFFSET,
                        signatures::WORLD_SYSTEM_P3_RIP_END))
        return true;

    spdlog::error("Failed to resolve WorldSystem - all signature patterns failed");
    return false;
}

bool HookManager::resolve_player_actor() {
    auto& rt = get_runtime_offsets();
    if (!rt.world_system_resolved) return false;

    // Follow the current client actor/component chain. ClientUserActor is a
    // wrapper in this build; the status component owns the player data used by
    // stat reads, while TransformSync owns the live pose.
    uintptr_t actor_mgr = resolve_ptr_chain(rt.world_system_ptr, {
        offsets::World::ACTOR_MANAGER
    });

    if (!is_valid_ptr(actor_mgr)) {
        spdlog::warn("ActorManager not found (world_system=0x{:X})", rt.world_system_ptr);
        return false;
    }

    rt.actor_manager_ptr = actor_mgr;

    uintptr_t child_actor = resolve_ptr_chain(actor_mgr, {
        offsets::World::CHILD_ACTOR
    });
    uintptr_t component_table = resolve_ptr_chain(child_actor, {
        offsets::Player::COMPONENT_TABLE
    });
    uintptr_t status_component = resolve_ptr_chain(component_table, {
        offsets::Player::STATUS_COMPONENT
    });
    uintptr_t player = resolve_ptr_chain(status_component, {
        offsets::Player::STATUS_PLAYER_DATA
    });
    uintptr_t transform = resolve_ptr_chain(component_table, {
        offsets::Player::TRANSFORM_COMPONENT
    });

    if (!is_valid_ptr(player) || !is_valid_ptr(transform)) {
        spdlog::warn("Player components not found (actor_mgr=0x{:X})", actor_mgr);
        return false;
    }

    rt.player_actor_ptr = player;
    rt.player_component_table = component_table;
    rt.player_transform_component = transform;
    rt.player_resolved = true;
    spdlog::info("Player resolved at 0x{:X}, transform at 0x{:X}", player, transform);

    // Resolve the stats component (pointer at actor + 0x58)
    uintptr_t stat_base = resolve_ptr_chain(player, {offsets::Player::STAT_COMPONENT});
    if (is_valid_ptr(stat_base)) {
        rt.player_stats_component = stat_base;
        spdlog::info("Player stats component at 0x{:X}", stat_base);
    } else {
        spdlog::warn("Player stats component not found");
    }

    return true;
}

bool HookManager::resolve_player_base() {
    auto& rt = get_runtime_offsets();

    // Method 1: Signature-based RIP-relative resolution (from bbfox0703 CT)
    auto result = sig_scan(signatures::PLAYER_BASE_DISCOVERY, "PlayerBaseDiscovery");
    if (result) {
        core_signature_found_ = true;
        uintptr_t rip_addr = result.address + signatures::PLAYER_BASE_DISCOVERY_RIP_OFFSET;
        uintptr_t player_base_storage = MemoryScanner::follow_rel32(rip_addr, 0);

        if (is_readable(player_base_storage, sizeof(uintptr_t))) {
            uintptr_t player_base = read_mem<uintptr_t>(player_base_storage, 0);
            if (is_valid_ptr(player_base)) {
                // Follow the chain to find the player actor:
                // base -> +0x18 -> +0xA0 -> +0xD0 -> +0x68 (Kliff)
                uintptr_t actor = resolve_ptr_chain(player_base, {
                    offsets::PlayerBase::CHAIN_0,
                    offsets::PlayerBase::CHAIN_1,
                    offsets::PlayerBase::CHAIN_2,
                    offsets::PlayerBase::SLOT_KLIFF
                });

                if (is_valid_ptr(actor)) {
                    rt.player_actor_ptr = actor;
                    rt.player_resolved = true;
                    // Also resolve stats component
                    uintptr_t sb = resolve_ptr_chain(actor, {offsets::Player::STAT_COMPONENT});
                    if (is_valid_ptr(sb)) rt.player_stats_component = sb;
                    spdlog::info("Player resolved via PlayerBaseDiscovery sig at 0x{:X}", actor);
                    return true;
                }
            }
        }
    }

    // Method 2: Hardcoded static base (last resort, v1.01.03).
    // Unlike Method 1, this RVA is a blind guess against the old (March 2026)
    // image layout — no signature backs it. After a game patch the address can
    // fall outside the relaid-out (or smaller) module image, so we must confirm
    // it is inside the module AND currently mapped before reading it. A bare
    // dereference here is the crash reported when a patch moves every offset:
    // the signatures fail, we fall through to this read, and it faults on
    // unmapped memory. is_valid_ptr can't catch it (the address is numerically
    // in range) — only a bounds + committed-memory check can.
    const uintptr_t static_rva = offsets::PlayerBase::STATIC_RVA;
    const uintptr_t static_base = game_base_ + static_rva;
    if (static_rva <= game_size_ && sizeof(uintptr_t) <= game_size_ - static_rva &&
        is_readable(static_base, sizeof(uintptr_t))) {
        uintptr_t player_base = read_mem<uintptr_t>(static_base, 0);
        if (is_valid_ptr(player_base)) {
            uintptr_t actor = resolve_ptr_chain(player_base, {
                offsets::PlayerBase::CHAIN_0,
                offsets::PlayerBase::CHAIN_1,
                offsets::PlayerBase::CHAIN_2,
                offsets::PlayerBase::SLOT_KLIFF
            });

            if (is_valid_ptr(actor)) {
                rt.player_actor_ptr = actor;
                rt.player_resolved = true;
                // Also resolve stats component
                uintptr_t sb = resolve_ptr_chain(actor, {offsets::Player::STAT_COMPONENT});
                if (is_valid_ptr(sb)) rt.player_stats_component = sb;
                spdlog::info("Player resolved via static base (0x{:X}) at 0x{:X}",
                             static_rva, actor);
                return true;
            }
        }
    } else {
        spdlog::warn("Static player base RVA 0x{:X} is outside the current module "
                     "image (size 0x{:X}) or not mapped — game version likely "
                     "changed; skipping hardcoded fallback to avoid a crash",
                     static_rva, game_size_);
    }

    spdlog::warn("Failed to resolve player via PlayerBase methods");
    return false;
}

bool HookManager::resolve_child_actor_vtbl() {
    auto& rt = get_runtime_offsets();
    if (rt.child_actor_vtbl != 0) return true;

    // Three sibling sigs from EquipHide that all anchor a `lea rax, [rip+VTBL]`
    // followed by `mov [rsi], rax`. The RIP_OFFSET in each constant is the
    // byte position of the disp32 inside the matched bytes. follow_rel32
    // returns the absolute address the LEA loads — that's the vtable.
    auto try_pattern = [&](const char* sig, const char* name, int rip_offset) -> bool {
        auto result = sig_scan(sig, name);
        if (!result) return false;
        uintptr_t vtbl = MemoryScanner::follow_rel32(result.address + rip_offset, 0);
        if (!is_valid_ptr(vtbl)) {
            spdlog::warn("ChildActor vtable address invalid from {}", name);
            return false;
        }
        // Vtables live in the game module's .rdata. Reject anything outside.
        if (vtbl < game_base_ || vtbl >= game_base_ + game_size_) {
            spdlog::warn("ChildActor vtable from {} out of game module range (0x{:X})",
                         name, vtbl);
            return false;
        }
        rt.child_actor_vtbl = vtbl;
        spdlog::info("ChildActor vtable resolved at 0x{:X} (RVA 0x{:X}) via {}",
                     vtbl, vtbl - game_base_, name);
        return true;
    };

    if (try_pattern(signatures::CHILD_ACTOR_VTBL_P1, "ChildActor_VTBL_P1",
                    signatures::CHILD_ACTOR_VTBL_P1_RIP_OFFSET)) return true;
    if (try_pattern(signatures::CHILD_ACTOR_VTBL_P2, "ChildActor_VTBL_P2",
                    signatures::CHILD_ACTOR_VTBL_P2_RIP_OFFSET)) return true;
    if (try_pattern(signatures::CHILD_ACTOR_VTBL_P3, "ChildActor_VTBL_P3",
                    signatures::CHILD_ACTOR_VTBL_P3_RIP_OFFSET)) return true;

    spdlog::warn("Failed to resolve ChildActor vtable - all patterns failed");
    return false;
}

// =========================================================================
// Hook detour implementations
// =========================================================================

namespace hooks {

void companion_position_probe_detour(SafetyHookContext& ctx) {
    companion_position_probe_hits.fetch_add(1, std::memory_order_relaxed);
    companion_position_probe_last_r13.store(ctx.r13, std::memory_order_relaxed);
    companion_position_probe_last_rbx.store(ctx.rbx, std::memory_order_relaxed);

    if (companion_position_state_writer_pending.load(std::memory_order_acquire) != 0) {
        return;
    }
    CompanionPositionStateGuard state_guard;
    if (!state_guard.acquired ||
        companion_position_state_writer_pending.load(std::memory_order_acquire) != 0) {
        return;
    }

    const uint64_t target_epoch = companion_position_probe_target_epoch.load(
        std::memory_order_acquire);
    const uintptr_t target = companion_position_probe_target.load(
        std::memory_order_acquire);
    if (target != 0 && ctx.r13 == target) {
        companion_position_probe_matches.fetch_add(1, std::memory_order_relaxed);
    }

    if (target == 0 || ctx.r13 < 0x10000 ||
        companion_position_probe_reference_ready.load(std::memory_order_acquire) == 0 ||
        companion_position_probe_reference_epoch.load(std::memory_order_acquire) !=
            target_epoch) {
        return;
    }
    const uint64_t now = GetTickCount64();
    const uint64_t reference_timestamp =
        companion_position_probe_reference_timestamp.load(std::memory_order_acquire);
    constexpr uint64_t kReferenceMaxAgeMs = 500;
    if (reference_timestamp > now || now - reference_timestamp > kReferenceMaxAgeMs) {
        return;
    }

    float reference_x = 0.0f;
    float reference_y = 0.0f;
    float reference_z = 0.0f;
    if (!load_position(companion_reference(), reference_x, reference_y, reference_z)) {
        return;
    }

    // The hook writes a physics-layer float4 at r13. Live correlation shows
    // its X/Z track TransformSync exactly, while Y varies by a few centimetres
    // due to collision/grounding. Require repeated close samples before locking.
    const auto* current = reinterpret_cast<const float*>(ctx.r13);
    const float current_x = current[0];
    const float current_y = current[1];
    const float current_z = current[2];
    constexpr float kHorizontalTolerance = 0.5f;
    constexpr float kVerticalTolerance = 1.0f;
    float expected_x = reference_x;
    float expected_y = reference_y;
    float expected_z = reference_z;
    const bool locked_identity =
        companion_position_probe_locked_r13.load(std::memory_order_acquire) == ctx.r13 &&
        companion_position_probe_locked_rbx.load(std::memory_order_acquire) == ctx.rbx &&
        companion_position_probe_locked_epoch.load(std::memory_order_acquire) == target_epoch;
    if (locked_identity) {
        const uint64_t test_expires_at = companion_position_test_expires_at.load(
            std::memory_order_acquire);
        if (test_expires_at > now &&
            companion_position_test_epoch.load(std::memory_order_acquire) == target_epoch) {
            expected_x = std::bit_cast<float>(
                companion_position_test_x.load(std::memory_order_relaxed));
            expected_y = std::bit_cast<float>(
                companion_position_test_y.load(std::memory_order_relaxed));
            expected_z = std::bit_cast<float>(
                companion_position_test_z.load(std::memory_order_relaxed));
        } else if (companion_position_override_allowed.load(std::memory_order_acquire) != 0 &&
                   companion_position_override_active.load(std::memory_order_acquire) != 0 &&
                   companion_position_override_epoch.load(std::memory_order_acquire) ==
                       target_epoch) {
            float desired_x = 0.0f;
            float desired_y = 0.0f;
            float desired_z = 0.0f;
            if (load_position(companion_override(), desired_x, desired_y, desired_z)) {
                const float vertical_offset = std::bit_cast<float>(
                    companion_position_probe_vertical_offset.load(
                        std::memory_order_relaxed));
                expected_x = desired_x;
                expected_y = desired_y + vertical_offset;
                expected_z = desired_z;
            }
        }
    }
    const bool coordinates_match =
        valid_position(current_x, current_y, current_z) &&
        valid_position(expected_x, expected_y, expected_z) &&
        std::abs(current_x - expected_x) <= kHorizontalTolerance &&
        std::abs(current_y - expected_y) <= kVerticalTolerance &&
        std::abs(current_z - expected_z) <= kHorizontalTolerance;

    if (coordinates_match) {
        companion_position_probe_coordinate_matches.fetch_add(
            1, std::memory_order_relaxed);
        uint32_t streak = 1;
        if (companion_position_probe_candidate_r13.load(std::memory_order_relaxed) == ctx.r13 &&
            companion_position_probe_candidate_rbx.load(std::memory_order_relaxed) == ctx.rbx) {
            streak = companion_position_probe_candidate_streak.fetch_add(
                1, std::memory_order_relaxed) + 1;
        } else {
            companion_position_probe_candidate_r13.store(
                ctx.r13, std::memory_order_relaxed);
            companion_position_probe_candidate_rbx.store(
                ctx.rbx, std::memory_order_relaxed);
            companion_position_probe_candidate_streak.store(
                streak, std::memory_order_relaxed);
        }

        constexpr uint32_t kValidationLease = 8;
        if (companion_position_probe_locked_r13.load(std::memory_order_acquire) == ctx.r13 &&
            companion_position_probe_locked_rbx.load(std::memory_order_acquire) == ctx.rbx &&
            companion_position_probe_locked_epoch.load(std::memory_order_acquire) ==
                target_epoch) {
            companion_position_probe_validation_lease.store(
                kValidationLease, std::memory_order_release);
        }

        constexpr uint32_t kRequiredMatches = 30;
        uintptr_t expected = 0;
        if (streak >= kRequiredMatches && target != 0 &&
            target_epoch == companion_position_probe_target_epoch.load(
                                std::memory_order_acquire) &&
            target == companion_position_probe_target.load(std::memory_order_acquire) &&
            companion_position_probe_locked_r13.compare_exchange_strong(
                expected, ctx.r13, std::memory_order_acq_rel)) {
            companion_position_probe_locked_rbx.store(
                ctx.rbx, std::memory_order_relaxed);
            companion_position_probe_vertical_offset.store(
                std::bit_cast<uint32_t>(current_y - reference_y),
                std::memory_order_relaxed);
            companion_position_probe_validation_lease.store(
                kValidationLease, std::memory_order_relaxed);
            companion_position_probe_locked_epoch.store(
                target_epoch, std::memory_order_release);

            uint32_t test_expected = 0;
            if (target_epoch == companion_position_probe_target_epoch.load(
                                    std::memory_order_acquire) &&
                companion_position_test_allowed.load(std::memory_order_acquire) != 0 &&
                companion_position_test_started.compare_exchange_strong(
                    test_expected, 1, std::memory_order_acq_rel)) {
                const float test_x = ctx.xmm0.f32[0] + 2.0f;
                const float test_y = ctx.xmm0.f32[1];
                const float test_z = ctx.xmm0.f32[2];
                if (valid_position(test_x, test_y, test_z)) {
                    companion_position_test_x.store(
                        std::bit_cast<uint32_t>(test_x), std::memory_order_relaxed);
                    companion_position_test_y.store(
                        std::bit_cast<uint32_t>(test_y), std::memory_order_relaxed);
                    companion_position_test_z.store(
                        std::bit_cast<uint32_t>(test_z), std::memory_order_relaxed);
                    companion_position_test_epoch.store(
                        target_epoch, std::memory_order_relaxed);
                    companion_position_test_expires_at.store(
                        now + 3000, std::memory_order_release);
                } else {
                    companion_position_test_started.store(0, std::memory_order_relaxed);
                }
            }
        }
    } else {
        companion_position_probe_candidate_r13.store(0, std::memory_order_relaxed);
        companion_position_probe_candidate_rbx.store(0, std::memory_order_relaxed);
        companion_position_probe_candidate_streak.store(0, std::memory_order_relaxed);

        if (companion_position_probe_locked_r13.load(std::memory_order_acquire) == ctx.r13 &&
            companion_position_probe_locked_rbx.load(std::memory_order_acquire) == ctx.rbx) {
            uint32_t lease = companion_position_probe_validation_lease.load(
                std::memory_order_acquire);
            while (lease != 0 &&
                   !companion_position_probe_validation_lease.compare_exchange_weak(
                       lease, lease - 1, std::memory_order_acq_rel)) {
            }
            if (lease == 1) {
                companion_position_probe_locked_epoch.store(0, std::memory_order_release);
                companion_position_probe_locked_r13.store(0, std::memory_order_relaxed);
                companion_position_probe_locked_rbx.store(0, std::memory_order_relaxed);
                companion_position_test_expires_at.store(0, std::memory_order_relaxed);
            }
        }
    }

    const bool authorized =
        companion_position_probe_locked_r13.load(std::memory_order_acquire) == ctx.r13 &&
        companion_position_probe_locked_rbx.load(std::memory_order_acquire) == ctx.rbx &&
        companion_position_probe_locked_epoch.load(std::memory_order_acquire) == target_epoch &&
        companion_position_probe_validation_lease.load(std::memory_order_acquire) != 0 &&
        target_epoch == companion_position_probe_target_epoch.load(std::memory_order_acquire) &&
        target == companion_position_probe_target.load(std::memory_order_acquire) &&
        companion_position_probe_reference_ready.load(std::memory_order_acquire) != 0 &&
        companion_position_probe_reference_epoch.load(std::memory_order_acquire) ==
            target_epoch &&
        reference_timestamp <= now && now - reference_timestamp <= kReferenceMaxAgeMs;
    if (!authorized) {
        return;
    }

    const uint64_t test_expires_at = companion_position_test_expires_at.load(
        std::memory_order_acquire);
    if (test_expires_at != 0 && now < test_expires_at &&
        companion_position_test_epoch.load(std::memory_order_acquire) == target_epoch &&
        target_epoch == companion_position_probe_target_epoch.load(
                            std::memory_order_acquire)) {
        ctx.xmm0.f32[0] = std::bit_cast<float>(
            companion_position_test_x.load(std::memory_order_relaxed));
        ctx.xmm0.f32[1] = std::bit_cast<float>(
            companion_position_test_y.load(std::memory_order_relaxed));
        ctx.xmm0.f32[2] = std::bit_cast<float>(
            companion_position_test_z.load(std::memory_order_relaxed));
        companion_position_override_writes.fetch_add(
            1, std::memory_order_relaxed);
        return;
    }

    if (companion_position_override_allowed.load(std::memory_order_acquire) == 0 ||
        companion_position_override_active.load(std::memory_order_acquire) == 0 ||
        companion_position_override_epoch.load(std::memory_order_acquire) != target_epoch) {
        return;
    }

    float desired_x = 0.0f;
    float desired_y = 0.0f;
    float desired_z = 0.0f;
    if (!load_position(companion_override(), desired_x, desired_y, desired_z)) {
        return;
    }
    if (!valid_position(desired_x, desired_y, desired_z) ||
        target_epoch != companion_position_probe_target_epoch.load(
                            std::memory_order_acquire)) {
        return;
    }
    const float vertical_offset = std::bit_cast<float>(
        companion_position_probe_vertical_offset.load(std::memory_order_acquire));
    const float final_y = desired_y + vertical_offset;
    if (!valid_position(desired_x, final_y, desired_z)) return;
    ctx.xmm0.f32[0] = desired_x;
    ctx.xmm0.f32[1] = final_y;
    ctx.xmm0.f32[2] = desired_z;
    companion_position_override_writes.fetch_add(1, std::memory_order_relaxed);
}

uint64_t set_companion_position_probe_target(uintptr_t target) noexcept {
    lock_companion_position_state();
    const uint64_t target_epoch = replace_companion_position_target_unlocked(target);
    unlock_companion_position_state();
    return target_epoch;
}

bool revoke_companion_position_probe_target(
    uintptr_t expected_target, uint64_t expected_epoch) noexcept {
    lock_companion_position_state();
    const bool matches =
        companion_position_probe_target.load(std::memory_order_acquire) ==
            expected_target &&
        companion_position_probe_target_epoch.load(std::memory_order_acquire) ==
            expected_epoch;
    if (matches) {
        replace_companion_position_target_unlocked(0);
    }
    unlock_companion_position_state();
    return matches;
}

bool update_companion_position_probe_reference(
    uintptr_t expected_target, uint64_t expected_epoch,
    float x, float y, float z) noexcept {
    lock_companion_position_state();
    const uintptr_t target = companion_position_probe_target.load(
        std::memory_order_acquire);
    const uint64_t target_epoch = companion_position_probe_target_epoch.load(
        std::memory_order_acquire);
    if (target == 0 || target != expected_target || target_epoch != expected_epoch) {
        unlock_companion_position_state();
        return false;
    }
    if (!valid_position(x, y, z)) {
        companion_position_probe_reference_ready.store(0, std::memory_order_release);
        companion_position_probe_reference_epoch.store(0, std::memory_order_relaxed);
        revoke_companion_position_mapping_unlocked();
        clear_companion_position_override_unlocked();
        unlock_companion_position_state();
        return false;
    }
    bool published = false;
    if (publish_position(companion_reference(), x, y, z)) {
        companion_position_probe_reference_epoch.store(
            target_epoch, std::memory_order_relaxed);
        companion_position_probe_reference_timestamp.store(
            GetTickCount64(), std::memory_order_relaxed);
        companion_position_probe_reference_ready.store(1, std::memory_order_release);
        published = true;
    }
    unlock_companion_position_state();
    return published;
}

void set_companion_position_override(
    uintptr_t expected_target, uint64_t expected_epoch,
    float x, float y, float z) noexcept {
    lock_companion_position_state();
    const uintptr_t target = companion_position_probe_target.load(
        std::memory_order_acquire);
    const uint64_t target_epoch = companion_position_probe_target_epoch.load(
        std::memory_order_acquire);
    if (target == 0 || target != expected_target || target_epoch != expected_epoch) {
        unlock_companion_position_state();
        return;
    }
    if (!valid_position(x, y, z)) {
        clear_companion_position_override_unlocked();
        unlock_companion_position_state();
        return;
    }
    if (publish_position(companion_override(), x, y, z)) {
        companion_position_override_epoch.store(
            target_epoch, std::memory_order_relaxed);
        companion_position_override_active.store(1, std::memory_order_release);
    }
    unlock_companion_position_state();
}

void log_companion_position_probe() {
    // Cumulative loads avoid splitting one hit/match pair across reset windows.
    // Load matches first: every match increments hits first, so hits >= matches.
    const uint64_t matches = companion_position_probe_matches.load(
        std::memory_order_relaxed);
    const uint64_t hits = companion_position_probe_hits.load(std::memory_order_relaxed);
    const uint64_t coordinate_matches =
        companion_position_probe_coordinate_matches.load(std::memory_order_relaxed);
    const uint64_t override_writes = companion_position_override_writes.load(
        std::memory_order_relaxed);
    const uintptr_t target = companion_position_probe_target.load(
        std::memory_order_acquire);
    const uintptr_t last_r13 = companion_position_probe_last_r13.load(
        std::memory_order_relaxed);
    const uintptr_t last_rbx = companion_position_probe_last_rbx.load(
        std::memory_order_relaxed);
    const uintptr_t candidate_r13 = companion_position_probe_candidate_r13.load(
        std::memory_order_relaxed);
    const uintptr_t candidate_rbx = companion_position_probe_candidate_rbx.load(
        std::memory_order_relaxed);
    const uint32_t candidate_streak =
        companion_position_probe_candidate_streak.load(std::memory_order_relaxed);
    const uintptr_t locked_r13 = companion_position_probe_locked_r13.load(
        std::memory_order_acquire);
    const uintptr_t locked_rbx = companion_position_probe_locked_rbx.load(
        std::memory_order_acquire);
    const uint64_t test_expires_at = companion_position_test_expires_at.load(
        std::memory_order_acquire);
    const uint64_t now = GetTickCount64();
    const uint64_t test_ms_remaining = test_expires_at > now
        ? test_expires_at - now : 0;
    spdlog::info("CompanionPositionProbe: hits={} direct_matches={} coordinate_matches={} "
                 "target=0x{:X} candidate=0x{:X}/0x{:X}/{} locked_r13=0x{:X} "
                 "locked_rbx=0x{:X} writes={} test_ms_remaining={} last_r13=0x{:X} "
                 "last_rbx=0x{:X}",
                 hits, matches, coordinate_matches, target, candidate_r13, candidate_rbx,
                 candidate_streak, locked_r13, locked_rbx, override_writes,
                 test_ms_remaining, last_r13, last_rbx);
}

void __cdecl player_animation_detour(void* player, uint32_t anim_id, float blend) {
    player_animation_hook.call<void>(player, anim_id, blend);

    if (Session::instance().is_active()) {
        PlayerSync::instance().on_local_animation_changed(anim_id, blend, 1.0f, 0.0f);
    }
}

float __cdecl damage_calc_detour(void* attacker, void* target, float base_damage) {
    float damage = damage_calc_hook.call<float>(attacker, target, base_damage);

    if (Session::instance().is_active()) {
        auto& rt = get_runtime_offsets();
        auto attacker_addr = reinterpret_cast<uintptr_t>(attacker);
        auto target_addr = reinterpret_cast<uintptr_t>(target);

        // Check if the attacker is the remote player (companion entity)
        auto remote_entity = CompanionHijack::instance().get_entity_ptr();
        bool from_remote = (remote_entity != 0 && attacker_addr == remote_entity);

        // Compare to player only when the player pointer has actually
        // been resolved — otherwise both sides could be 0 and any null
        // attacker would be misclassified as "the player swung this".
        bool from_player = is_valid_ptr(rt.player_actor_ptr) &&
                           attacker_addr == rt.player_actor_ptr;

        if (from_remote || from_player) {
            // Report damage to EnemySync for co-op damage tracking
            // Use the target pointer as a simple entity ID
            uint32_t target_id = static_cast<uint32_t>(target_addr & 0xFFFFFFFF);
            EnemySync::instance().report_damage(target_id, damage);
        }
    }

    return damage;
}

void* __cdecl companion_spawn_detour(void* spawn_params) {
    void* companion = companion_spawn_hook.call<void*>(spawn_params);

    // If we're in a co-op session and need to hijack a companion slot
    if (Session::instance().is_active() && companion) {
        auto& hijack = CompanionHijack::instance();
        if (!hijack.is_active()) {
            spdlog::info("Companion spawned during co-op - attempting hijack");
            hijack.activate();
        }
    }

    return companion;
}

void __cdecl world_state_detour(void* world_obj, uint32_t state_id, uint32_t new_state) {
    world_state_hook.call<void>(world_obj, state_id, new_state);

    // This detour is wired to the STAT_WRITE signature (player HP/
    // stamina/spirit writes — see install_hooks "StatWrite"). Earlier
    // code forwarded every call to WorldSync::on_world_interact, which
    // sent a *reliable* WORLD_INTERACT packet for every stat tick.
    // Stat writes can fire dozens of times per second during combat,
    // so the channel was being flooded for no benefit — the receiver
    // (WorldSync::on_remote_interact) is a debug-log-only stub, and
    // player health is already broadcast at 5Hz via the PlayerSync
    // full-state packet. Drop the broadcast; keep the hook installed
    // so the original game function still runs and so the slot is
    // available for future stat-event use without re-scanning AOBs.
    (void)world_obj; (void)state_id; (void)new_state;
}

void __cdecl camera_detour(void* camera_struct, void* unused) {
    camera_hook.call<void>(camera_struct, unused);

    // Camera Zoom/FOV hook (from Send's CE table, game v1.00.03)
    // At hook point: r12 = camera struct base, offset 0xD8 = zoom/FOV float
    // The original instruction: movss [r12+0xD8], xmm0
    //
    // We capture the camera struct pointer for co-op use and can adjust
    // the zoom/FOV to pull the camera back when players are far apart.

    auto camera_addr = reinterpret_cast<uintptr_t>(camera_struct);
    if (!is_valid_ptr(camera_addr)) return;

    // Capture camera struct pointer for other systems to use
    auto& rt = get_runtime_offsets();
    rt.camera_struct_ptr = camera_addr;
    rt.camera_resolved = true;

    if (!Session::instance().is_active()) return;

    auto& ps = PlayerSync::instance();
    if (!ps.is_tether_active()) return;

    // When players are far apart, increase zoom distance to keep both in view
    auto local_pos = PlayerManager::instance().local_position();
    auto remote_pos = ps.remote_state().position;

    Vec3 diff = local_pos - remote_pos;
    float dist_sq = diff.length_sq();

    // Read current zoom/FOV from camera struct + 0xD8
    float current_fov = read_mem<float>(camera_addr, offsets::Camera::ZOOM_FOV);

    // If players are more than 15m apart, start pulling camera back
    constexpr float TETHER_START_SQ = 15.0f * 15.0f;   // 225
    constexpr float TETHER_MAX_SQ   = 40.0f * 40.0f;   // 1600
    constexpr float BASE_ZOOM       = 8.0f;             // Default max zoom out
    constexpr float MAX_ZOOM         = 14.0f;            // Extended zoom for co-op

    if (dist_sq > TETHER_START_SQ) {
        // Lerp zoom from BASE_ZOOM to MAX_ZOOM based on player distance
        float t = (dist_sq - TETHER_START_SQ) / (TETHER_MAX_SQ - TETHER_START_SQ);
        if (t > 1.0f) t = 1.0f;
        float target_zoom = BASE_ZOOM + (MAX_ZOOM - BASE_ZOOM) * t;

        // Only override if we'd pull further out than the game's current value
        if (target_zoom > current_fov) {
            write_mem<float>(camera_addr, offsets::Camera::ZOOM_FOV, target_zoom);
        }
    }
}

void __cdecl animation_evaluator_detour(void* evaluator) {
    animation_evaluator_hook.call<void>(evaluator);

    auto evaluator_addr = reinterpret_cast<uintptr_t>(evaluator);
    if (!is_valid_ptr(evaluator_addr)) return;

    auto& rt = get_runtime_offsets();
    if (rt.animation_evaluator_ptr != evaluator_addr) {
        rt.animation_evaluator_ptr = evaluator_addr;
        rt.animation_evaluator_resolved = true;
        // One-shot log so users see the evaluator was captured.
        static std::atomic<bool> logged{false};
        bool expected = false;
        if (logged.compare_exchange_strong(expected, true)) {
            spdlog::info("AnimationEvaluator captured at 0x{:X} (experimental hook)",
                         evaluator_addr);
        }
    }
}

void dragon_hp_probe_detour(SafetyHookContext& ctx) {
    // Mid-hook at the dragon timer write (`mov [r13+0x160], reg`).
    // r13 holds the dragon mount marker. Previous inline-hook detour
    // assumed rcx held the marker, which scanned random unrelated
    // memory because the calling convention doesn't apply mid-function.
    uintptr_t marker_addr = ctx.r13;
    if (!is_valid_ptr(marker_addr)) return;

    auto& rt = get_runtime_offsets();
    rt.dragon_marker_ptr = marker_addr;

    // Only run the scan once per mount session. dragon_hp_offset stays
    // non-zero after the first successful scan until hookmgr shutdown.
    if (rt.dragon_hp_resolved) return;

    uint32_t off = dynamic_scan_float(
        marker_addr,
        offsets::Mount::DRAGON_HP_SCAN_MIN_OFFSET,
        offsets::Mount::DRAGON_HP_SCAN_MAX_OFFSET,
        offsets::Mount::DRAGON_HP_SCAN_STRIDE,
        offsets::Mount::DRAGON_HP_PLAUSIBLE_MIN,
        offsets::Mount::DRAGON_HP_PLAUSIBLE_MAX);

    if (off != 0) {
        rt.dragon_hp_offset = off;
        rt.dragon_hp_resolved = true;
        float v = *reinterpret_cast<float*>(marker_addr + off);
        spdlog::info("Dragon HP resolved at marker+0x{:X} (initial value {})", off, v);
    }
}

void teleport_waypoint_detour(SafetyHookContext& ctx) {
    // Mid-hook at CrimsonDesert.exe+0xAB5594 (movsd [r14+0xD8], xmm0).
    // r15 = source waypoint struct, layout (from bbfox CT 174-176):
    //   r15+0x00 (word)   waypoint type id
    //   r15+0x1C (double) packed (X float, Y float)
    //   r15+0x24 (float)  Z
    if (!is_valid_ptr(ctx.r15)) return;

    auto* src = reinterpret_cast<const uint8_t*>(ctx.r15);
    float x = *reinterpret_cast<const float*>(src + offsets::Teleport::SRC_WAYPOINT_XY);
    float y = *reinterpret_cast<const float*>(src + offsets::Teleport::SRC_WAYPOINT_XY + 4);
    float z = *reinterpret_cast<const float*>(src + offsets::Teleport::SRC_WAYPOINT_Z);
    uint16_t type_id = *reinterpret_cast<const uint16_t*>(src);

    // Only the host broadcasts. Both sides hit this hook when fast-travelling
    // locally, but sending from a client would create a feedback loop.
    auto& session = Session::instance();
    if (!session.is_active() || !session.is_host()) {
        spdlog::debug("Teleport hook fired (not host, no broadcast): type=0x{:X} pos=({},{},{})",
                      type_id, x, y, z);
        return;
    }

    spdlog::info("Teleport waypoint captured: type=0x{:X} dest=({},{},{})", type_id, x, y, z);
    WorldSync::instance().on_teleport_trigger({x, y, z}, type_id);
}

void mount_ptr_capture_detour(SafetyHookContext& ctx) {
    // At offset +20 of the MOUNT_PTR_CAPTURE AOB, the next instruction
    // is `mov rax, [rdi+0x68]` — rdi holds the mount entity this-pointer.
    // The hook fires on every entry to this function while the player
    // is interacting with a mount; we cache the pointer only when it
    // changes to avoid log spam.
    if (!is_valid_ptr(ctx.rdi)) return;

    auto& rt = get_runtime_offsets();
    if (rt.mount_ptr == ctx.rdi && rt.mount_resolved) return;

    rt.mount_ptr = ctx.rdi;
    rt.mount_resolved = true;
    spdlog::info("Mount pointer captured at 0x{:X}", rt.mount_ptr);
}

} // namespace hooks

} // namespace cdcoop
