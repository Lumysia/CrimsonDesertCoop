#include <cdcoop/network/session.h>
#include <cdcoop/network/steam_network.h>
#include <cdcoop/core/config.h>
#include <cdcoop/core/hooks.h>
#include <cdcoop/sync/player_sync.h>
#include <cdcoop/sync/enemy_sync.h>
#include <cdcoop/sync/world_sync.h>
#include <cdcoop/player/companion_hijack.h>
#include <cdcoop/player/player_manager.h>
#include <spdlog/spdlog.h>
#include <Windows.h>
#include <cstring>
#include <vector>

namespace cdcoop {

namespace {

bool game_state_ready() {
    Vec3 position{};
    Quat rotation{};
    float health = 0.0f;
    float max_health = 0.0f;
    return PlayerManager::instance().read_local_state(
        position, rotation, health, max_health);
}

} // namespace

Session& Session::instance() {
    static Session inst;
    return inst;
}

bool Session::host_session() {
    std::lock_guard lock(transition_mutex_);
    if (state_ != SessionState::DISCONNECTED) {
        spdlog::warn("Cannot host: already in state {}",
                     static_cast<int>(state_.load()));
        return false;
    }
    if (HookManager::instance().status().unsupported_build) {
        spdlog::error("Cannot host: this game build is not supported by the current offsets");
        return false;
    }
    if (!game_state_ready()) {
        spdlog::error("Cannot host: load into a supported game world first");
        return false;
    }

    auto& cfg = get_config();

    std::shared_ptr<INetworkTransport> t;
#if CDCOOP_STEAM
    if (cfg.use_steam_networking) {
        t = std::make_shared<SteamNetworkTransport>();
    }
#endif

    if (!t) {
        spdlog::error("No network transport available");
        return false;
    }

    const uint64_t generation = ++transport_generation_;
    t->set_packet_callback([this, generation](PacketType type, const uint8_t* data, size_t size) {
        on_packet_received(generation, type, data, size);
    });

    if (!t->host(cfg.port)) {
        spdlog::error("Failed to start hosting on port {}", cfg.port);
        return false;
    }

    role_ = SessionRole::HOST;
    heartbeat_timer_ = 0.0f;
    handshake_timer_ = 0.0f;
    connect_timer_ = 0.0f;
    time_since_last_recv_ = 0.0f;
    handshake_sent_ = false;
    PlayerSync::instance().reset_remote_state();
    {
        std::lock_guard tlock(transport_mutex_);
        transport_ = std::move(t);
    }
    state_ = SessionState::HOSTING;
    spdlog::info("Hosting co-op session on port {}...", cfg.port);
    spdlog::info("Waiting for player 2 to connect");

    return true;
}

bool Session::join_session(const std::string& target) {
    std::lock_guard lock(transition_mutex_);
    if (state_ != SessionState::DISCONNECTED) {
        spdlog::warn("Cannot join: already in state {}",
                     static_cast<int>(state_.load()));
        return false;
    }
    if (HookManager::instance().status().unsupported_build) {
        spdlog::error("Cannot join: this game build is not supported by the current offsets");
        return false;
    }
    if (!game_state_ready()) {
        spdlog::error("Cannot join: load into a supported game world first");
        return false;
    }
    if (target.empty()) {
        spdlog::error("Cannot join: Steam ID is empty");
        return false;
    }

    auto& cfg = get_config();

    std::shared_ptr<INetworkTransport> t;
#if CDCOOP_STEAM
    if (cfg.use_steam_networking) {
        t = std::make_shared<SteamNetworkTransport>();
    }
#endif

    if (!t) {
        spdlog::error("No network transport available");
        return false;
    }

    const uint64_t generation = ++transport_generation_;
    t->set_packet_callback([this, generation](PacketType type, const uint8_t* data, size_t size) {
        on_packet_received(generation, type, data, size);
    });

    if (!t->connect(target, cfg.port)) {
        spdlog::error("Failed to connect to {}", target);
        return false;
    }

    role_ = SessionRole::CLIENT;
    heartbeat_timer_ = 0.0f;
    handshake_timer_ = 0.0f;
    connect_timer_ = 0.0f;
    time_since_last_recv_ = 0.0f;
    handshake_sent_ = false;
    PlayerSync::instance().reset_remote_state();
    {
        std::lock_guard tlock(transport_mutex_);
        transport_ = std::move(t);
    }
    state_ = SessionState::CONNECTING;
    spdlog::info("Connecting to {}...", target);

    return true;
}

void Session::leave_session() {
    std::lock_guard lock(transition_mutex_);
    if (state_ == SessionState::DISCONNECTED) return;

    spdlog::info("Leaving session...");

    // Publish the inactive state before cleanup so hook/tick threads stop
    // sending packets or writing the hijacked entity during teardown.
    state_ = SessionState::DISCONNECTED;
    ++transport_generation_;

    // Clean up co-op state
    PlayerManager::instance().despawn_remote_player();
    EnemySync::instance().revert_coop_scaling();
    PlayerSync::instance().reset_remote_state();

    // Snapshot the transport, then publish nullptr so concurrent send()
    // / update() / poll() callers stop using it. Their already-held
    // shared_ptr copies keep the object alive until they return; we
    // hold our own copy here for the disconnect/cleanup sequence.
    std::shared_ptr<INetworkTransport> t;
    {
        std::lock_guard tlock(transport_mutex_);
        t = std::move(transport_);
    }

    if (t) {
        // Send disconnect packet over the captured transport directly,
        // bypassing send() since transport_ is already null.
        PacketHeader dc{};
        dc.type = PacketType::DISCONNECT;
        dc.payload_size = 0;
        if (t->is_connected()) {
            t->send(reinterpret_cast<const uint8_t*>(&dc), sizeof(dc), true);
        }
        t->disconnect();
        // t goes out of scope below — actual destruction happens once
        // every other shared_ptr copy in flight has been released.
    }

    role_ = SessionRole::NONE;
    sequence_ = 0;
    heartbeat_timer_ = 0.0f;
    handshake_timer_ = 0.0f;
    connect_timer_ = 0.0f;
    time_since_last_recv_ = 0.0f;
    handshake_sent_ = false;
    spdlog::info("Session ended");
}

void Session::send(const uint8_t* data, size_t size, bool reliable) {
    std::shared_ptr<INetworkTransport> t;
    {
        std::lock_guard lock(transport_mutex_);
        t = transport_;
    }
    if (t && t->is_connected()) {
        if (size >= sizeof(PacketHeader)) {
            // Copy packet data to stamp sequence/timestamp without mutating caller's data
            std::vector<uint8_t> buf(data, data + size);
            auto* hdr = reinterpret_cast<PacketHeader*>(buf.data());
            hdr->sequence = sequence_++;
            hdr->timestamp_ms = static_cast<uint32_t>(GetTickCount64() & 0xFFFFFFFF);
            t->send(buf.data(), buf.size(), reliable);
        } else {
            t->send(data, size, reliable);
        }
    }
}

void Session::update(float delta_time) {
    std::lock_guard transition_lock(transition_mutex_);
    std::shared_ptr<INetworkTransport> t;
    {
        std::lock_guard lock(transport_mutex_);
        t = transport_;
    }
    PlayerManager::instance().update(delta_time);
    if (!t) return;

    // Poll for incoming messages
    t->poll();

    if (state_ == SessionState::CONNECTING) {
        connect_timer_ += delta_time;

        if (t->is_connected()) {
            handshake_timer_ += delta_time;
            if (!handshake_sent_ || handshake_timer_ >= HANDSHAKE_INTERVAL) {
                send_handshake();
                handshake_sent_ = true;
                handshake_timer_ = 0.0f;
            }
        }

        if (connect_timer_ >= CONNECT_TIMEOUT_SECONDS) {
            spdlog::warn("Connection attempt timed out");
            leave_session();
        }
        return;
    }

    if (state_ != SessionState::CONNECTED) return;

    // Heartbeat
    heartbeat_timer_ += delta_time;
    if (heartbeat_timer_ >= HEARTBEAT_INTERVAL) {
        send_heartbeat();
        heartbeat_timer_ = 0.0f;
    }

    // Timeout detection
    time_since_last_recv_ += delta_time;
    if (time_since_last_recv_ >= TIMEOUT_SECONDS) {
        spdlog::warn("Connection timed out");
        leave_session();
        return;
    }

    PlayerSync::instance().update(delta_time);
    EnemySync::instance().update(delta_time);
    WorldSync::instance().update(delta_time);
}

void Session::register_handler(PacketType type, PacketCallback handler) {
    std::lock_guard lock(handler_mutex_);
    handlers_[type] = std::move(handler);
}

std::string Session::peer_name() const {
    std::shared_ptr<INetworkTransport> t;
    {
        std::lock_guard lock(transport_mutex_);
        t = transport_;
    }
    return t ? t->peer_name() : "";
}

void Session::invite_friend() {
    if (role_ != SessionRole::HOST) {
        spdlog::info("Invite: ignored — not hosting");
        return;
    }
#if CDCOOP_STEAM
    std::shared_ptr<INetworkTransport> t;
    {
        std::lock_guard lock(transport_mutex_);
        t = transport_;
    }
    // SteamNetworkTransport is the only implementation we have that
    // knows about lobbies; the abstract INetworkTransport interface
    // deliberately doesn't include invite_friend because it's a Steam-
    // specific concept. Downcast is safe here because host_session
    // only ever constructs SteamNetworkTransport when cfg.use_steam.
    if (auto* steam = dynamic_cast<SteamNetworkTransport*>(t.get())) {
        steam->invite_friend();
        return;
    }
#endif
    spdlog::warn("Invite: Steam networking disabled — share Steam ID manually");
}

void Session::on_packet_received(uint64_t generation, PacketType type,
                                 const uint8_t* data, size_t size) {
    std::lock_guard transition_lock(transition_mutex_);
    if (generation != transport_generation_.load()) return;
    // Handle system packets internally
    switch (type) {
        case PacketType::HANDSHAKE:
        case PacketType::HANDSHAKE_ACK:
            handle_handshake(data, size);
            return;
        case PacketType::DISCONNECT:
            spdlog::info("Peer disconnected");
            leave_session();
            return;
        case PacketType::HEARTBEAT:
            if (state_ == SessionState::CONNECTED) time_since_last_recv_ = 0.0f;
            return; // Just resets timeout timer (done above)
        default:
            break;
    }

    // Never dispatch gameplay packets until the handshake has completed.
    if (state_ != SessionState::CONNECTED) return;

    // Forward to registered handlers
    std::lock_guard lock(handler_mutex_);
    auto it = handlers_.find(type);
    if (it != handlers_.end()) {
        it->second(type, data, size);
        time_since_last_recv_ = 0.0f;
    }
}

void Session::handle_handshake(const uint8_t* data, size_t size) {
    if (size < sizeof(HandshakePacket)) return;

    auto* hs = reinterpret_cast<const HandshakePacket*>(data);
    if (hs->protocol_version != PROTOCOL_VERSION) {
        spdlog::warn("Rejected peer with protocol version {} (expected {})",
                     hs->protocol_version, PROTOCOL_VERSION);
        return;
    }

    if (hs->header.type == PacketType::HANDSHAKE && role_ == SessionRole::HOST &&
        (state_ == SessionState::HOSTING || state_ == SessionState::CONNECTED)) {
        time_since_last_recv_ = 0.0f;
        char peer_name[sizeof(hs->player_name) + 1]{};
        memcpy(peer_name, hs->player_name, sizeof(hs->player_name));
        spdlog::info("Player '{}' connected! (protocol v{}, mod v{})",
                      peer_name, hs->protocol_version, hs->mod_version);

        // Send ack
        HandshakePacket ack{};
        ack.header.type = PacketType::HANDSHAKE_ACK;
        ack.header.payload_size = sizeof(HandshakePacket) - sizeof(PacketHeader);
        ack.protocol_version = PROTOCOL_VERSION;
        auto& cfg = get_config();
        strncpy(ack.player_name, cfg.player_name.c_str(), sizeof(ack.player_name) - 1);
        ack.mod_version = 3;
        send_packet(ack);

        enter_connected();

    } else if (hs->header.type == PacketType::HANDSHAKE_ACK &&
               role_ == SessionRole::CLIENT && state_ == SessionState::CONNECTING) {
        time_since_last_recv_ = 0.0f;
        char peer_name[sizeof(hs->player_name) + 1]{};
        memcpy(peer_name, hs->player_name, sizeof(hs->player_name));
        spdlog::info("Connected to host '{}'!", peer_name);
        enter_connected();
    }
}

void Session::send_handshake() {
    HandshakePacket hs{};
    hs.header.type = PacketType::HANDSHAKE;
    hs.header.payload_size = sizeof(HandshakePacket) - sizeof(PacketHeader);
    hs.protocol_version = PROTOCOL_VERSION;
    const auto& cfg = get_config();
    strncpy(hs.player_name, cfg.player_name.c_str(), sizeof(hs.player_name) - 1);
    hs.mod_version = 3;
    send_packet(hs, true);
}

bool Session::enter_connected() {
    SessionState expected = role_ == SessionRole::HOST
        ? SessionState::HOSTING
        : SessionState::CONNECTING;
    if (!state_.compare_exchange_strong(expected, SessionState::CONNECTED)) {
        return false;
    }

    heartbeat_timer_ = 0.0f;
    time_since_last_recv_ = 0.0f;
    PlayerManager::instance().spawn_remote_player();
    if (role_ == SessionRole::HOST) {
        EnemySync::instance().apply_coop_scaling();
    }
    return true;
}

void Session::send_heartbeat() {
    PacketHeader hb{};
    hb.type = PacketType::HEARTBEAT;
    hb.payload_size = 0;
    send(reinterpret_cast<const uint8_t*>(&hb), sizeof(hb));
}

} // namespace cdcoop
