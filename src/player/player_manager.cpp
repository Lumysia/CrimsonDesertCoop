#include <cdcoop/player/player_manager.h>
#include <cdcoop/core/hooks.h>
#include <cdcoop/core/game_structures.h>
#include <cdcoop/player/companion_hijack.h>
#include <cdcoop/network/session.h>
#include <spdlog/spdlog.h>
#include <cmath>

namespace cdcoop {

PlayerManager& PlayerManager::instance() {
    static PlayerManager inst;
    return inst;
}

bool PlayerManager::initialize() {
    find_local_player();

    if (local_player_ == 0) {
        // Non-fatal: this happens when the mod initializes while the user
        // is still on the main menu / character select. The update() tick
        // re-runs find_local_player() each frame, so the pointer will
        // resolve as soon as the player loads into the world. Returning
        // false here used to abort the whole mod (sync systems, overlay,
        // input thread all skipped) — now we let init proceed so F7 works
        // the moment the world finishes loading.
        spdlog::warn("PlayerManager: local player not found yet — will retry on each tick");
        spdlog::warn("This is normal if the mod loaded while you were still in the menu");
        return true;
    }

    spdlog::info("PlayerManager: local player at 0x{:X}", local_player_);
    return true;
}

void PlayerManager::shutdown() {
    despawn_remote_player();
    local_player_ = 0;
    game_instance_ = 0;
}

void PlayerManager::update(float delta_time) {
    if (HookManager::instance().status().unsupported_build) return;

    if (local_player_ != 0) {
        const auto& rt = get_runtime_offsets();
        bool actor_chain_changed = false;
        if (is_valid_ptr(rt.player_component_table)) {
            if (!is_valid_ptr(rt.actor_manager_ptr)) {
                actor_chain_changed = true;
            } else {
                const uintptr_t current_actor_manager = read_mem<uintptr_t>(
                    rt.world_system_ptr, offsets::World::ACTOR_MANAGER);
                const uintptr_t current_child_actor = read_mem<uintptr_t>(
                    rt.actor_manager_ptr, offsets::World::CHILD_ACTOR);
                const uintptr_t current_component_table = read_mem<uintptr_t>(
                    current_child_actor, offsets::Player::COMPONENT_TABLE);
                const uintptr_t current_status = read_mem<uintptr_t>(
                    current_component_table, offsets::Player::STATUS_COMPONENT);
                const uintptr_t current_player = read_mem<uintptr_t>(
                    current_status, offsets::Player::STATUS_PLAYER_DATA);
                const uintptr_t current_transform = read_mem<uintptr_t>(
                    current_component_table, offsets::Player::TRANSFORM_COMPONENT);
                actor_chain_changed = current_actor_manager != rt.actor_manager_ptr ||
                                      current_component_table != rt.player_component_table ||
                                      current_player != local_player_ ||
                                      current_transform != rt.player_transform_component;
            }
        }
        if (!is_readable(local_player_, sizeof(uintptr_t)) || actor_chain_changed) {
            invalidate_local_player();
        }
    }

    // Re-acquire player pointer if lost (e.g., after loading screen)
    if (local_player_ == 0) {
        local_resolve_retry_timer_ += delta_time;
        if (local_resolve_retry_timer_ >= 1.0f) {
            local_resolve_retry_timer_ = 0.0f;
            find_local_player();
        }
    } else {
        local_resolve_retry_timer_ = 0.0f;
    }

    if (Session::instance().is_active() && !is_remote_spawned()) {
        remote_spawn_retry_timer_ += delta_time;
        if (remote_spawn_retry_timer_ >= 1.0f) {
            remote_spawn_retry_timer_ = 0.0f;
            spawn_remote_player();
        }
    } else {
        remote_spawn_retry_timer_ = 0.0f;
    }
}

Vec3 PlayerManager::local_position() const {
    if (local_player_ == 0) return {0, 0, 0};

    auto& rt = get_runtime_offsets();
    if (is_readable(rt.player_transform_component + offsets::Player::TRANSFORM_POSITION,
                    sizeof(Vec3))) {
        return read_mem<Vec3>(rt.player_transform_component,
                              offsets::Player::TRANSFORM_POSITION);
    }

    // Verified authoritative position chain (from position_research.md):
    //   actor -> +0x40 -> +0x08 -> player_core (-> +0x248 -> pos_struct -> +0x90)
    // Try the verified chain first, fall back to direct position hook pointer.
    uintptr_t player_core = resolve_ptr_chain(local_player_, {
        offsets::Player::ACTOR_TO_INNER,
        offsets::Player::INNER_TO_CORE
    });

    if (is_valid_ptr(player_core)) {
        uintptr_t pos_struct = resolve_ptr_chain(player_core, {
            offsets::Player::POS_OWNER_TO_STRUCT
        });
        if (is_valid_ptr(pos_struct)) {
            return {
                read_mem<float>(pos_struct, offsets::Player::POS_STRUCT_X),
                read_mem<float>(pos_struct, offsets::Player::POS_STRUCT_Y),
                read_mem<float>(pos_struct, offsets::Player::POS_STRUCT_Z)
            };
        }
    }

    return {0, 0, 0};
}

Quat PlayerManager::local_rotation() const {
    if (local_player_ == 0) return {0, 0, 0, 1};
    auto& rt = get_runtime_offsets();
    if (is_readable(rt.player_transform_component +
                        offsets::Player::TRANSFORM_ROTATION_QUAT,
                    sizeof(Quat))) {
        return read_mem<Quat>(rt.player_transform_component,
                              offsets::Player::TRANSFORM_ROTATION_QUAT);
    }
    // Rotation follows the position float4 (x,y,z,w) at +0x90.
    // The next float4 at +0xA0 is likely rotation (needs verification).
    uintptr_t player_core = resolve_ptr_chain(local_player_, {
        offsets::Player::ACTOR_TO_INNER,
        offsets::Player::INNER_TO_CORE
    });
    if (is_valid_ptr(player_core)) {
        uintptr_t pos_struct = resolve_ptr_chain(player_core, {
            offsets::Player::POS_OWNER_TO_STRUCT
        });
        if (is_valid_ptr(pos_struct)) {
            return read_mem<Quat>(pos_struct, offsets::Player::ROTATION_QUAT);
        }
    }
    return {0, 0, 0, 1};
}

float PlayerManager::local_health() const {
    if (local_player_ == 0) return 0;
    auto& rt = get_runtime_offsets();
    if (rt.player_stats_component != 0) {
        int64_t raw = read_mem<int64_t>(rt.player_stats_component, StatEntry::CURRENT_VALUE);
        return static_cast<float>(raw) / 1000.0f;
    }
    return 0;
}

float PlayerManager::local_max_health() const {
    if (local_player_ == 0) return 0;
    auto& rt = get_runtime_offsets();
    if (rt.player_stats_component != 0) {
        int64_t raw = read_mem<int64_t>(rt.player_stats_component, StatEntry::MAX_VALUE);
        return static_cast<float>(raw) / 1000.0f;
    }
    return 0;
}

bool PlayerManager::read_local_state(Vec3& position, Quat& rotation,
                                     float& health, float& max_health) {
    if (!is_valid_ptr(local_player_)) return false;

    auto& rt = get_runtime_offsets();
    uintptr_t stat_base = resolve_ptr_chain(local_player_, {
        offsets::Player::STAT_COMPONENT
    });
    if (!is_readable(rt.player_transform_component +
                         offsets::Player::TRANSFORM_ROTATION_QUAT,
                     sizeof(Quat) + sizeof(Vec3)) ||
        !is_valid_ptr(stat_base)) {
        invalidate_local_player();
        return false;
    }

    rotation = read_mem<Quat>(rt.player_transform_component,
                              offsets::Player::TRANSFORM_ROTATION_QUAT);
    position = read_mem<Vec3>(rt.player_transform_component,
                              offsets::Player::TRANSFORM_POSITION);
    int64_t raw_health = read_mem<int64_t>(stat_base, StatEntry::CURRENT_VALUE);
    int64_t raw_max_health = read_mem<int64_t>(stat_base, StatEntry::MAX_VALUE);

    constexpr float kMaxCoordinate = 10'000'000.0f;
    constexpr int64_t kMaxRawHealth = 10'000'000'000LL;
    const float quat_len_sq = rotation.x * rotation.x + rotation.y * rotation.y +
                              rotation.z * rotation.z + rotation.w * rotation.w;
    bool valid = std::isfinite(position.x) && std::isfinite(position.y) &&
                 std::isfinite(position.z) &&
                 std::abs(position.x) <= kMaxCoordinate &&
                 std::abs(position.y) <= kMaxCoordinate &&
                 std::abs(position.z) <= kMaxCoordinate &&
                 std::isfinite(quat_len_sq) && quat_len_sq >= 0.25f &&
                 quat_len_sq <= 4.0f && raw_health >= 0 && raw_max_health > 0 &&
                 raw_health <= kMaxRawHealth && raw_max_health <= kMaxRawHealth;
    if (!valid) {
        invalidate_local_player();
        return false;
    }

    const float inv_len = 1.0f / std::sqrt(quat_len_sq);
    rotation.x *= inv_len;
    rotation.y *= inv_len;
    rotation.z *= inv_len;
    rotation.w *= inv_len;
    health = static_cast<float>(raw_health) / 1000.0f;
    max_health = static_cast<float>(raw_max_health) / 1000.0f;
    get_runtime_offsets().position_resolved = true;
    return true;
}

uintptr_t PlayerManager::remote_player() const {
    return CompanionHijack::instance().get_entity_ptr();
}

void PlayerManager::spawn_remote_player() {
    spdlog::info("Selecting remote companion candidate...");

    auto& hijack = CompanionHijack::instance();
    if (!hijack.is_active()) {
        if (!hijack.activate()) {
            spdlog::error("Failed to spawn remote player - no companion to hijack");
            spdlog::info("Tip: Make sure a companion (Oongka/Yann/Naira) is in your party");
            return;
        }
    }

    spdlog::warn("Remote companion candidate selected; pose application is disabled");
}

void PlayerManager::despawn_remote_player() {
    CompanionHijack::instance().deactivate();
    remote_spawn_retry_timer_ = 0.0f;
    spdlog::info("Remote player despawned");
}

bool PlayerManager::is_remote_spawned() const {
    return CompanionHijack::instance().is_active();
}

void PlayerManager::find_local_player() {
    // Player finding strategy uses the WorldSystem chain discovered by
    // CrimsonDesertTools/EquipHide:
    //   WorldSystem -> ActorManager (+0x30) -> UserActor (+0x28)
    //
    // The WorldSystem singleton is resolved via RIP-relative sig scan
    // in HookManager::resolve_world_system().
    //
    // Additionally, the PlayerPointerCapture hook (from player-status-modifier)
    // gives us the player pointer at runtime via rax register.

    auto& rt = get_runtime_offsets();

    // Method 1: Use the pre-resolved player actor from WorldSystem chain
    if (rt.player_resolved && is_valid_ptr(rt.player_actor_ptr)) {
        local_player_ = rt.player_actor_ptr;
        game_instance_ = rt.world_system_ptr;
        spdlog::info("PlayerManager: found player via WorldSystem chain at 0x{:X}", local_player_);
        return;
    }

    // Method 2: Re-resolve WorldSystem. A supported signature can match while
    // its singleton is still null on the main menu, then become valid later.
    auto& hooks = HookManager::instance();
    if (!rt.world_system_resolved) {
        hooks.resolve_world_system();
        if (rt.player_resolved && is_valid_ptr(rt.player_actor_ptr)) {
            local_player_ = rt.player_actor_ptr;
            game_instance_ = rt.world_system_ptr;
            spdlog::info("PlayerManager: resolved player after world load at 0x{:X}",
                         local_player_);
            return;
        }
    }

    // Method 3: Try resolving the chain ourselves if WorldSystem is known
    if (rt.world_system_resolved && is_valid_ptr(rt.world_system_ptr)) {
        if (hooks.resolve_player_actor()) {
            local_player_ = rt.player_actor_ptr;
            game_instance_ = rt.world_system_ptr;
            spdlog::info("PlayerManager: resolved player at 0x{:X}", local_player_);
            return;
        }
    }

    // Method 4: Retry the independent PlayerBase signature/static chain.
    if (hooks.resolve_player_base() && rt.player_resolved && is_valid_ptr(rt.player_actor_ptr)) {
        local_player_ = rt.player_actor_ptr;
        game_instance_ = rt.world_system_ptr;
        spdlog::info("PlayerManager: resolved player via PlayerBase at 0x{:X}", local_player_);
        return;
    }

    spdlog::debug("PlayerManager: player not available yet");
}

void PlayerManager::invalidate_local_player() {
    CompanionHijack::instance().deactivate();
    local_player_ = 0;
    game_instance_ = 0;
    local_resolve_retry_timer_ = 0.0f;

    auto& rt = get_runtime_offsets();
    rt.player_actor_ptr = 0;
    rt.player_component_table = 0;
    rt.player_transform_component = 0;
    rt.player_position_ptr = 0;
    rt.player_stats_component = 0;
    rt.actor_manager_ptr = 0;
    rt.player_resolved = false;
    rt.position_resolved = false;
}

} // namespace cdcoop
