#pragma once

#include <cstdint>
#include <string>
#include <cdcoop/core/game_structures.h>

namespace cdcoop {

// Manages the local and remote player entities
class PlayerManager {
public:
    static PlayerManager& instance();

    bool initialize();
    void shutdown();
    void update(float delta_time);

    // Local player (always the game's actual player character)
    uintptr_t local_player() const { return local_player_; }
    Vec3 local_position() const;
    Quat local_rotation() const;
    float local_health() const;
    float local_max_health() const;
    bool read_local_state(Vec3& position, Quat& rotation,
                          float& health, float& max_health);

    // Remote player (the hijacked companion entity)
    uintptr_t remote_player() const;
    void spawn_remote_player();
    void despawn_remote_player();

    bool is_remote_spawned() const;

private:
    PlayerManager() = default;

    void find_local_player();
    void invalidate_local_player();
    void initialize_position_control();
    void poll_position_control(float delta_time);

    uintptr_t local_player_ = 0;
    uintptr_t game_instance_ = 0;
    float local_resolve_retry_timer_ = 0.0f;
    float remote_spawn_retry_timer_ = 0.0f;
    float companion_probe_log_timer_ = 0.0f;
    float position_control_poll_timer_ = 0.0f;
    uint64_t last_position_command_id_ = 0;
    std::string position_control_path_;
    bool position_control_parse_error_logged_ = false;
    bool position_control_ready_ = false;
    bool position_control_disabled_ = false;
};

} // namespace cdcoop
