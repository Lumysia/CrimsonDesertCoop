#include <cdcoop/sync/animation_sync.h>
#include <spdlog/spdlog.h>

namespace cdcoop {

AnimationSync& AnimationSync::instance() {
    static AnimationSync inst;
    return inst;
}

void AnimationSync::initialize() {
    spdlog::warn("AnimationSync initialized with remote animation application disabled");
}

void AnimationSync::shutdown() {
    anim_remap_table_.clear();
}

void AnimationSync::apply_remote_animation(uintptr_t entity_ptr, uint32_t anim_id,
                                             float blend_weight, float speed,
                                             float normalized_time) {
    (void)entity_ptr;
    (void)anim_id;
    (void)blend_weight;
    (void)speed;
    (void)normalized_time;
}

uint32_t AnimationSync::remap_animation(uint32_t source_anim_id, int /*source_model*/,
                                         int /*target_model*/) {
    // Passthrough mode: always use the source anim ID directly.
    // Cross-model remapping requires real animation IDs extracted from PAZ archives.
    // For now, this works correctly when source_model == target_model.
    return source_anim_id;
}

} // namespace cdcoop
