#include <cdcoop/network/packet.h>

#include <cstdint>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition) {
    if (!condition) ++failures;
}

} // namespace

int main() {
    cdcoop::HandshakePacket packet{};
    packet.header.type = cdcoop::PacketType::HANDSHAKE;
    packet.header.payload_size = sizeof(packet) - sizeof(cdcoop::PacketHeader);
    packet.protocol_version = 1;

    auto bytes = cdcoop::PacketBuilder::serialize(packet);
    expect(cdcoop::PacketBuilder::validate(bytes.data(), bytes.size()));
    expect(!cdcoop::PacketBuilder::validate(bytes.data(), sizeof(cdcoop::PacketHeader) - 1));

    auto bad_magic = bytes;
    bad_magic[0] = 'X';
    expect(!cdcoop::PacketBuilder::validate(bad_magic.data(), bad_magic.size()));

    auto truncated = bytes;
    truncated.pop_back();
    expect(!cdcoop::PacketBuilder::validate(truncated.data(), truncated.size()));

    auto under_declared = bytes;
    reinterpret_cast<cdcoop::PacketHeader*>(under_declared.data())->payload_size = 0;
    expect(!cdcoop::PacketBuilder::validate(under_declared.data(), under_declared.size()));

    auto unknown = bytes;
    reinterpret_cast<cdcoop::PacketHeader*>(unknown.data())->type =
        static_cast<cdcoop::PacketType>(0xFE);
    expect(!cdcoop::PacketBuilder::validate(unknown.data(), unknown.size()));

    cdcoop::PacketHeader heartbeat{};
    heartbeat.type = cdcoop::PacketType::HEARTBEAT;
    heartbeat.payload_size = 0;
    expect(cdcoop::PacketBuilder::validate(
        reinterpret_cast<const std::uint8_t*>(&heartbeat), sizeof(heartbeat)));

    return failures == 0 ? 0 : 1;
}
