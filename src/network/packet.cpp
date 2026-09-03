#include <cdcoop/network/packet.h>

namespace cdcoop {

namespace {

size_t expected_packet_size(PacketType type) {
    switch (type) {
        case PacketType::HANDSHAKE:
        case PacketType::HANDSHAKE_ACK:
            return sizeof(HandshakePacket);
        case PacketType::DISCONNECT:
        case PacketType::HEARTBEAT:
            return sizeof(PacketHeader);
        case PacketType::PLAYER_POSITION:
            return sizeof(PlayerPositionPacket);
        case PacketType::PLAYER_ANIMATION:
            return sizeof(PlayerAnimationPacket);
        case PacketType::PLAYER_COMBAT:
            return sizeof(PlayerCombatPacket);
        case PacketType::PLAYER_FULL_STATE:
            return sizeof(PlayerFullStatePacket);
        case PacketType::ENEMY_STATE:
        case PacketType::ENEMY_DEATH:
        case PacketType::ENEMY_SPAWN:
            return sizeof(EnemyStatePacket);
        case PacketType::ENEMY_DAMAGE:
            return sizeof(EnemyDamagePacket);
        case PacketType::WORLD_INTERACT:
        case PacketType::QUEST_UPDATE:
        case PacketType::CUTSCENE_TRIGGER:
            return sizeof(WorldInteractPacket);
        case PacketType::TELEPORT_TRIGGER:
            return sizeof(TeleportPacket);
        case PacketType::MOUNT_STATE:
            return sizeof(MountStatePacket);
        default:
            return 0;
    }
}

} // namespace

std::vector<uint8_t> PacketBuilder::serialize(const PacketHeader& header, const void* payload, size_t size) {
    std::vector<uint8_t> buf(sizeof(PacketHeader) + size);
    memcpy(buf.data(), &header, sizeof(PacketHeader));
    if (payload && size > 0) {
        memcpy(buf.data() + sizeof(PacketHeader), payload, size);
    }
    return buf;
}

bool PacketBuilder::validate(const uint8_t* data, size_t size) {
    if (size < sizeof(PacketHeader)) return false;
    auto* header = reinterpret_cast<const PacketHeader*>(data);
    if (header->magic[0] != 'C' || header->magic[1] != 'D') return false;
    const size_t expected = expected_packet_size(header->type);
    if (expected == 0 || size != expected) return false;
    return header->payload_size == expected - sizeof(PacketHeader);
}

} // namespace cdcoop
