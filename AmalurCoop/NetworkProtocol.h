#pragma once
#include <cstddef>
#include <cstdint>

namespace AmalurNet
{
    constexpr std::uint32_t Magic = 0x504F4341; // ACOP
    constexpr std::uint16_t ProtocolVersion = 5;
    constexpr std::uint16_t DefaultPort = 7777;
    constexpr std::size_t MaxPlayers = 16;

    enum class PacketType : std::uint8_t
    {
        Hello = 1,
        Welcome = 2,
        Transform = 3,
        Ping = 4,
        Pong = 5,
        Goodbye = 6,
        MotionEvent = 7,
        PlayerLeft = 8,
        PlayerSnapshot = 9,
        PeerJoined = 10,
        DiscoveryRequest = 11,
        DiscoveryResponse = 12,
        Roster = 13,
        VisualChunk = 14,
        VisualAck = 15
    };

#pragma pack(push, 1)
    struct Packet
    {
        std::uint32_t MagicValue = Magic;
        std::uint16_t Version = ProtocolVersion;
        PacketType Type = PacketType::Hello;
        std::uint8_t Reserved = 0;
        std::uint32_t PlayerId = 0;
        std::uint32_t Sequence = 0;
        std::uint64_t TimestampMs = 0;
        float X = 0.0f;
        float Y = 0.0f;
        float Z = 0.0f;
        std::uint32_t MotionHash = 0;
        std::int32_t RequestedVariant = 0;
        std::int32_t ParameterA = 0;
        std::int32_t ParameterB = 0;
        std::uint32_t SenderTick = 0;
        std::uint32_t PacketSequence = 0;
        std::uint8_t FlagA = 0;
        std::uint8_t MotionReserved[3]{};

        // Protocol v4 player-state snapshot. These are game/resource handles or
        // compact flags, never raw process pointers.
        std::uint32_t DefinitionHandle = 0;
        std::uint32_t PlayerSetupValue = 0;
        std::uint32_t SnapshotFlags = 0;
        std::uint32_t SnapshotRevision = 0;

        // Host session and per-connection challenge. Never accept packets from
        // an earlier connection even when the UDP endpoint is reused.
        std::uint64_t SessionId = 0;
        std::uint64_t ConnectionId = 0;
        std::uint32_t RosterIds[MaxPlayers]{};
        std::uint32_t VisualRevision = 0;
        std::uint32_t VisualBytes = 0;
        std::uint16_t VisualIndex = 0;
        std::uint16_t VisualCount = 0;
        std::uint8_t VisualData[960]{};
        float Rotation[4]{ 0, 0, 0, 1 };
    };
#pragma pack(pop)

    static_assert(sizeof(Packet) == 1148, "Unexpected Amalur network packet size");

    // Serial arithmetic: zero is a valid sequence after wraparound.
    constexpr bool IsNewer(std::uint32_t value, std::uint32_t previous)
    {
        const auto distance = value - previous;
        return distance != 0 && distance < 0x80000000u;
    }
}
