#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <cdcoop/core/game_structures.h>

namespace cdcoop {

// Tracks animation remapping research for the remote player entity. Application
// is disabled until the current build's evaluator ownership is mapped safely.
class AnimationSync {
public:
    static AnimationSync& instance();

    void initialize();
    void shutdown();

    // Currently a no-op: current-build direct animation writes are unsafe.
    void apply_remote_animation(uintptr_t entity_ptr, uint32_t anim_id,
                                float blend_weight, float speed, float normalized_time);

    // Map animation IDs between players if they use different character models
    uint32_t remap_animation(uint32_t source_anim_id, int source_model, int target_model);

private:
    AnimationSync() = default;

    // Animation ID mapping tables (populated from game data)
    // Key: (source_model_id, anim_id) -> target_anim_id
    std::unordered_map<uint64_t, uint32_t> anim_remap_table_;
};

} // namespace cdcoop
