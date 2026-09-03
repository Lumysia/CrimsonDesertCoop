#include <cdcoop/player/companion_hijack.h>
#include <cdcoop/core/hooks.h>
#include <cdcoop/core/game_structures.h>
#include <spdlog/spdlog.h>
#include <cmath>

namespace cdcoop {

CompanionHijack& CompanionHijack::instance() {
    static CompanionHijack inst;
    return inst;
}

bool CompanionHijack::initialize() {
    // Body slots are read directly from the player actor; ActorManager is not
    // required when the independent PlayerBase path resolved successfully.
    if (!is_valid_ptr(get_runtime_offsets().player_actor_ptr)) {
        spdlog::warn("CompanionHijack: player actor is not available yet");
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

    // Strategy: find the first active companion and take it over
    //
    // Crimson Desert has companion characters (Oongka, Yann, Naira) that
    // fight alongside the player. We reuse one as the remote rendered actor.
    //
    // Steps:
    // 1. Scan the player's body slots
    // 2. Find the first active companion
    // 3. Apply validated remote position/health state

    // Iterate companion/NPC actor body slots from the ActorManager.
    // The player actor uses body slots at offsets 0xD0-0x108 (8 slots, 8 bytes each)
    // which hold pointers to child actors including companions.
    // We scan these slots on the player actor to find companion entities.
    constexpr int MAX_BODY_SLOTS = 8;
    constexpr uint32_t BODY_SLOT_BASE = ActorStructure::BODY_SLOT_0; // 0xD0

    uintptr_t player_actor = get_runtime_offsets().player_actor_ptr;
    if (!is_valid_ptr(player_actor)) {
        spdlog::warn("CompanionHijack: player actor not available for companion scan");
    } else {
        for (int i = 0; i < MAX_BODY_SLOTS; i++) {
            uint32_t slot_offset = BODY_SLOT_BASE + static_cast<uint32_t>(i * 8);
            uintptr_t companion = read_mem<uintptr_t>(player_actor, slot_offset);
            if (!is_valid_ptr(companion)) continue;

            // Skip if this is the player actor itself
            if (companion == player_actor) continue;

            // Check if the entity has an AI controller (companions do, player doesn't)
            uintptr_t ai_ctrl = read_mem<uintptr_t>(companion, offsets::Companion::AI_CONTROLLER);
            if (!is_valid_ptr(ai_ctrl)) continue;

            uintptr_t type_ptr = resolve_ptr_chain(companion, {
                ActorStructure::TYPE_CHAIN_COMP,
                ActorStructure::TYPE_CHAIN_ACTOR,
                ActorStructure::TYPE_CHAIN_TYPE
            });
            uint8_t actor_type = read_mem<uint8_t>(type_ptr, ActorStructure::TYPE_BYTE);
            if (actor_type < ActorStructure::TYPE_PARTY_MIN ||
                actor_type > ActorStructure::TYPE_PARTY_MAX) {
                continue;
            }
            if (get_runtime_offsets().child_actor_vtbl != 0 && !is_child_actor(companion)) {
                continue;
            }

            hijacked_entity_ = companion;
            hijacked_slot_ = i;
            active_ = true;
            spdlog::info("CompanionHijack: found companion at body slot {} (0x{:X})",
                          i, companion);
            break;
        }
    }

    if (!active_) {
        spdlog::error("CompanionHijack: no active companion found to hijack");
        return false;
    }

    spdlog::info("CompanionHijack: hijacked companion slot {} (entity: 0x{:X})",
                  hijacked_slot_, hijacked_entity_);
    return true;
}

void CompanionHijack::deactivate() {
    if (!active_) return;

    spdlog::info("CompanionHijack: released companion slot {}", hijacked_slot_);

    hijacked_entity_ = 0;
    hijacked_slot_ = -1;
    active_ = false;
}

void CompanionHijack::invalidate() {
    hijacked_entity_ = 0;
    hijacked_slot_ = -1;
    active_ = false;
}

bool CompanionHijack::is_active() const {
    if (!active_ || hijacked_slot_ < 0 || !is_valid_ptr(hijacked_entity_)) return false;
    uintptr_t player = get_runtime_offsets().player_actor_ptr;
    if (!is_valid_ptr(player)) return false;
    uint32_t slot = ActorStructure::BODY_SLOT_0 +
                    static_cast<uint32_t>(hijacked_slot_) * sizeof(uintptr_t);
    return read_mem<uintptr_t>(player, slot) == hijacked_entity_;
}

void CompanionHijack::set_position(const Vec3& pos, const Quat& rot) {
    if (!active_ || !is_valid_ptr(hijacked_entity_)) return;
    const float quat_len_sq = rot.x * rot.x + rot.y * rot.y +
                              rot.z * rot.z + rot.w * rot.w;
    if (!std::isfinite(pos.x) || !std::isfinite(pos.y) || !std::isfinite(pos.z) ||
        std::abs(pos.x) > 10'000'000.0f || std::abs(pos.y) > 10'000'000.0f ||
        std::abs(pos.z) > 10'000'000.0f || !std::isfinite(quat_len_sq) ||
        quat_len_sq < 0.9f || quat_len_sq > 1.1f) {
        return;
    }

    // Use the verified position chain: actor -> +0x40 -> +0x08 -> core -> +0x248 -> +0x90
    // This writes to the authoritative position, not a cache.
    uintptr_t core = resolve_ptr_chain(hijacked_entity_, {
        offsets::Player::ACTOR_TO_INNER,
        offsets::Player::INNER_TO_CORE
    });

    if (is_valid_ptr(core)) {
        uintptr_t pos_struct = resolve_ptr_chain(core, {
            offsets::Player::POS_OWNER_TO_STRUCT
        });
        if (is_valid_ptr(pos_struct)) {
            write_mem<float>(pos_struct, offsets::Player::POS_STRUCT_X, pos.x);
            write_mem<float>(pos_struct, offsets::Player::POS_STRUCT_Y, pos.y);
            write_mem<float>(pos_struct, offsets::Player::POS_STRUCT_Z, pos.z);
            // Rotation quaternion at +0xA0 (right after position at +0x90)
            write_mem<Quat>(pos_struct, offsets::Player::ROTATION_QUAT, rot);
            return;
        }
    }

    // Never guess position fields on the actor base. Offsets 0/4/8 describe
    // the hook-time r13 vector, and writing them here would corrupt the vtable.
}

void CompanionHijack::set_animation(uint32_t anim_id, float blend,
                                     [[maybe_unused]] float speed,
                                     [[maybe_unused]] float time) {
    if (!active_ || !is_valid_ptr(hijacked_entity_)) return;

    // Write animation state to the companion entity.
    // Companions share the same actor layout as the player.
    write_mem<uint32_t>(hijacked_entity_, offsets::Companion::ANIM_STATE, anim_id);
    write_mem<float>(hijacked_entity_, offsets::Player::ANIM_BLEND, blend);
}

void CompanionHijack::set_health(float health, float max_health) {
    if (!active_ || hijacked_entity_ == 0) return;
    if (!std::isfinite(health) || !std::isfinite(max_health) ||
        health < 0.0f || max_health <= 0.0f || health > max_health * 2.0f ||
        max_health > 10'000'000.0f) {
        return;
    }

    // Write player 2's health to the companion entity's stat component so the
    // game engine renders the correct health bar. Uses the same StatEntry format
    // as the player (int64, displayed_value * 1000).
    uintptr_t stat_base = resolve_ptr_chain(hijacked_entity_, {offsets::Player::STAT_COMPONENT});
    if (is_valid_ptr(stat_base)) {
        write_mem<int64_t>(stat_base, StatEntry::CURRENT_VALUE,
                           static_cast<int64_t>(health * 1000.0f));
        write_mem<int64_t>(stat_base, StatEntry::MAX_VALUE,
                           static_cast<int64_t>(max_health * 1000.0f));
    }
}

} // namespace cdcoop
