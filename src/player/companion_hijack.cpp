#include <cdcoop/player/companion_hijack.h>
#include <cdcoop/core/hooks.h>
#include <cdcoop/core/game_structures.h>
#include <spdlog/spdlog.h>
#include <cmath>
#include <limits>

namespace cdcoop {

CompanionHijack& CompanionHijack::instance() {
    static CompanionHijack inst;
    return inst;
}

bool CompanionHijack::initialize() {
    auto& rt = get_runtime_offsets();
    const uintptr_t current_actor_manager = read_mem<uintptr_t>(
        rt.world_system_ptr, offsets::World::ACTOR_MANAGER);
    if (current_actor_manager != rt.actor_manager_ptr &&
        !HookManager::instance().resolve_player_actor()) {
        spdlog::warn("CompanionHijack: current actor manager could not be refreshed");
        return false;
    }

    if (!is_valid_ptr(rt.actor_manager_ptr) ||
        !is_valid_ptr(rt.player_transform_component) ||
        rt.child_actor_vtbl == 0) {
        spdlog::warn("CompanionHijack: actor manager or player components are not available yet");
        return false;
    }

    return true;
}

void CompanionHijack::shutdown() {
    deactivate();
}

bool CompanionHijack::activate() {
    if (active_) {
        if (is_active()) return true;
        deactivate();
    }

    if (!initialize()) return false;

    const auto& rt = get_runtime_offsets();
    actor_registry_ = read_mem<uintptr_t>(rt.actor_manager_ptr,
        offsets::World::ACTOR_REGISTRY_ARRAY);
    const uint32_t capacity = read_mem<uint32_t>(rt.actor_manager_ptr,
        offsets::World::ACTOR_REGISTRY_CAPACITY);
    if (capacity == 0 || capacity > offsets::World::MAX_ACTORS ||
        !is_readable(actor_registry_,
                     static_cast<std::size_t>(capacity) * sizeof(uintptr_t))) {
        spdlog::warn("CompanionHijack: actor registry is not available");
        actor_registry_ = 0;
        return false;
    }

    if (!is_readable(rt.player_transform_component +
                         offsets::Player::TRANSFORM_ROTATION_QUAT,
                     sizeof(Quat) + sizeof(Vec3))) {
        spdlog::warn("CompanionHijack: player transform is not readable");
        actor_registry_ = 0;
        return false;
    }

    const Vec3 local_position = read_mem<Vec3>(rt.player_transform_component,
                                                offsets::Player::TRANSFORM_POSITION);
    const uintptr_t current_child_actor = read_mem<uintptr_t>(
        rt.actor_manager_ptr, offsets::World::CHILD_ACTOR);
    if (!is_child_actor(current_child_actor)) {
        spdlog::warn("CompanionHijack: current controlled actor is not available");
        actor_registry_ = 0;
        return false;
    }
    constexpr float kMaxCoordinate = 10'000'000.0f;
    if (!std::isfinite(local_position.x) || !std::isfinite(local_position.y) ||
        !std::isfinite(local_position.z) || std::abs(local_position.x) > kMaxCoordinate ||
        std::abs(local_position.y) > kMaxCoordinate ||
        std::abs(local_position.z) > kMaxCoordinate) {
        spdlog::warn("CompanionHijack: player position is invalid");
        actor_registry_ = 0;
        return false;
    }

    float nearest_distance_sq = std::numeric_limits<float>::infinity();

    for (uint32_t i = 0; i < capacity; ++i) {
        const uintptr_t companion = read_mem<uintptr_t>(
            actor_registry_, i * static_cast<uint32_t>(sizeof(uintptr_t)));
        if (companion == current_child_actor || !is_child_actor(companion)) continue;

        const uintptr_t component_table = read_mem<uintptr_t>(
            companion, offsets::Player::COMPONENT_TABLE);
        const uintptr_t ai = read_mem<uintptr_t>(
            component_table, offsets::Player::AI_COMPONENT);
        const uintptr_t mercenary = read_mem<uintptr_t>(
            component_table, offsets::Player::MERCENARY_COMPONENT);
        const uintptr_t transform = read_mem<uintptr_t>(
            component_table, offsets::Player::TRANSFORM_COMPONENT);
        const uintptr_t status = read_mem<uintptr_t>(
            component_table, offsets::Player::STATUS_COMPONENT);
        const uintptr_t player_data = read_mem<uintptr_t>(
            status, offsets::Player::STATUS_PLAYER_DATA);
        const uintptr_t stats = read_mem<uintptr_t>(
            player_data, offsets::Player::STAT_COMPONENT);
        const int64_t max_health = read_mem<int64_t>(stats, StatEntry::MAX_VALUE);
        if (!is_readable(ai, sizeof(uintptr_t)) ||
            !is_readable(mercenary, sizeof(uintptr_t)) ||
            !is_readable(transform + offsets::Player::TRANSFORM_ROTATION_QUAT,
                         sizeof(Quat) + sizeof(Vec3)) ||
            !is_readable(stats, StatEntry::MAX_VALUE + sizeof(int64_t)) ||
            read_mem<int32_t>(stats, StatEntry::TYPE) != StatEntry::HEALTH_ID ||
            max_health <= 0 || max_health > 10'000'000'000LL) {
            continue;
        }

        const Vec3 position = read_mem<Vec3>(transform,
                                             offsets::Player::TRANSFORM_POSITION);
        const Vec3 difference = position - local_position;
        const float distance_sq = difference.length_sq();
        if (!std::isfinite(distance_sq) || distance_sq >= nearest_distance_sq) continue;

        nearest_distance_sq = distance_sq;
        hijacked_entity_ = companion;
        hijacked_transform_ = transform;
        hijacked_stats_ = stats;
        hijacked_slot_ = static_cast<int>(i);
    }

    active_ = hijacked_slot_ >= 0;

    if (!active_) {
        spdlog::error("CompanionHijack: no active mercenary companion found");
        actor_registry_ = 0;
        return false;
    }

    spdlog::info("CompanionHijack: selected actor {} at 0x{:X} ({:.1f}m away)",
                 hijacked_slot_, hijacked_entity_, std::sqrt(nearest_distance_sq));
    return true;
}

void CompanionHijack::deactivate() {
    if (active_) {
        spdlog::info("CompanionHijack: released companion slot {}", hijacked_slot_);
    }

    actor_registry_ = 0;
    hijacked_entity_ = 0;
    hijacked_transform_ = 0;
    hijacked_stats_ = 0;
    hijacked_slot_ = -1;
    active_ = false;
}

void CompanionHijack::invalidate() {
    deactivate();
}

bool CompanionHijack::is_active() const {
    if (!active_ || hijacked_slot_ < 0 || !is_valid_ptr(actor_registry_) ||
        !is_valid_ptr(hijacked_entity_) || !is_valid_ptr(hijacked_transform_) ||
        !is_valid_ptr(hijacked_stats_)) {
        return false;
    }

    const auto& rt = get_runtime_offsets();
    const uintptr_t current_actor_manager = read_mem<uintptr_t>(
        rt.world_system_ptr, offsets::World::ACTOR_MANAGER);
    const uintptr_t current_child_actor = read_mem<uintptr_t>(
        rt.actor_manager_ptr, offsets::World::CHILD_ACTOR);
    const uintptr_t current_registry = read_mem<uintptr_t>(
        rt.actor_manager_ptr, offsets::World::ACTOR_REGISTRY_ARRAY);
    const uint32_t capacity = read_mem<uint32_t>(
        rt.actor_manager_ptr, offsets::World::ACTOR_REGISTRY_CAPACITY);
    if (current_actor_manager != rt.actor_manager_ptr ||
        !is_child_actor(current_child_actor) ||
        current_registry != actor_registry_ ||
        current_child_actor == hijacked_entity_ ||
        capacity == 0 || capacity > offsets::World::MAX_ACTORS ||
        static_cast<uint32_t>(hijacked_slot_) >= capacity ||
        read_mem<uintptr_t>(actor_registry_,
            static_cast<uint32_t>(hijacked_slot_) *
                static_cast<uint32_t>(sizeof(uintptr_t))) != hijacked_entity_ ||
        !is_child_actor(hijacked_entity_)) {
        return false;
    }

    const uintptr_t component_table = read_mem<uintptr_t>(
        hijacked_entity_, offsets::Player::COMPONENT_TABLE);
    const uintptr_t ai = read_mem<uintptr_t>(
        component_table, offsets::Player::AI_COMPONENT);
    const uintptr_t mercenary = read_mem<uintptr_t>(
        component_table, offsets::Player::MERCENARY_COMPONENT);
    const uintptr_t transform = read_mem<uintptr_t>(
        component_table, offsets::Player::TRANSFORM_COMPONENT);
    const uintptr_t status = read_mem<uintptr_t>(
        component_table, offsets::Player::STATUS_COMPONENT);
    const uintptr_t player_data = read_mem<uintptr_t>(
        status, offsets::Player::STATUS_PLAYER_DATA);
    const uintptr_t stats = read_mem<uintptr_t>(
        player_data, offsets::Player::STAT_COMPONENT);
    return is_readable(ai, sizeof(uintptr_t)) &&
           is_readable(mercenary, sizeof(uintptr_t)) && transform == hijacked_transform_ &&
           is_readable(transform + offsets::Player::TRANSFORM_ROTATION_QUAT,
                       sizeof(Quat) + sizeof(Vec3)) && stats == hijacked_stats_ &&
           read_mem<int32_t>(stats, StatEntry::TYPE) == StatEntry::HEALTH_ID;
}

uintptr_t CompanionHijack::get_entity_ptr() const {
    return is_active() ? hijacked_entity_ : 0;
}

void CompanionHijack::set_position(const Vec3& pos, const Quat& rot) {
    // Direct TransformSync writes race the companion AI and visibly oscillate.
    // Clearing the AI component slot held the pose but later crashed the game,
    // so remote pose application stays disabled until a safe AI/update hook is found.
    (void)pos;
    (void)rot;
}

void CompanionHijack::set_animation(uint32_t anim_id, float blend,
                                     float speed, float time) {
    // Actor-base animation offsets are unverified on the current build.
    (void)anim_id;
    (void)blend;
    (void)speed;
    (void)time;
}

void CompanionHijack::set_health(float health, float max_health) {
    // Keep remote health in PlayerSync/overlay only. Writing it into a local
    // companion can persist the peer's max HP into the host's save.
    (void)health;
    (void)max_health;
}

} // namespace cdcoop
