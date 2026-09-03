#pragma once

#include <cstdint>
#include <cdcoop/core/game_structures.h>

namespace cdcoop {

// Locates and tracks a companion candidate for a future player-2 takeover.
// Current-build state writes are disabled until the engine's AI/update path can
// be intercepted without racing or invalidating component ownership.
//
// This approach is chosen because:
// 1. The game already handles companion rendering, collision, and camera
// 2. Companions already have combat animations and can deal damage
// 3. It avoids the complexity of spawning a completely new entity type
class CompanionHijack {
public:
    static CompanionHijack& instance();

    // Initialize after the local player actor has resolved
    bool initialize();
    void shutdown();

    // Select and track a companion candidate without mutating it.
    bool activate();

    // Deactivate: stop applying remote state to the companion
    void deactivate();
    void invalidate();

    // Get the selected companion's entity pointer (used by sync systems)
    uintptr_t get_entity_ptr() const;
    bool is_active() const;

    // Disabled current-build state application entry points.
    void set_position(const Vec3& pos, const Quat& rot);
    void set_animation(uint32_t anim_id, float blend, float speed, float time);
    void set_health(float health, float max_health);

private:
    CompanionHijack() = default;

    uintptr_t actor_registry_ = 0;
    uintptr_t hijacked_entity_ = 0;    // ClientChildOnlyInGameActor
    uintptr_t hijacked_transform_ = 0; // ClientTransformSyncActorComponent
    uintptr_t hijacked_stats_ = 0;     // Health StatEntry
    int hijacked_slot_ = -1;           // Index in ActorManager registry
    bool active_ = false;
};

} // namespace cdcoop
