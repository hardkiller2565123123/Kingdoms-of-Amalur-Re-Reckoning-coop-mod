#pragma once
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <cstring>

namespace AmalurNet
{
    constexpr std::uint32_t Magic = 0x504F4341; // ACOP
    constexpr std::uint16_t ProtocolVersion = 7;
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

    // Native clip-queue requests: values and stable resource keys only.
    // MotionEvent reuses VisualData; no packet growth or IP fragmentation.
    struct AnimationRequest
    {
        std::uint32_t RigKey = 0;
        std::uint32_t ResourceKeyA = 0;
        std::uint32_t ResourceKeyB = 0;
        std::uint32_t StartOffset = 0;
        float Blend[6]{};
        std::uint8_t BlendEnabled = 0;
        std::uint8_t Reserved[3]{};
        std::int32_t Variants[4]{ -1, -1, -1, -1 };
        float Weights[4]{ 1, 0, 0, 0 };
        std::uint8_t Track = 0;
        std::uint8_t SelectedVariants = 0;
        std::uint8_t QueueReserved[2]{};
        std::uint32_t CatalogKey = 0;
    };
    static_assert(sizeof(AnimationRequest) == 84);

    inline bool ValidAnimation(const Packet& packet)
    {
        AnimationRequest request{};
        std::memcpy(&request, packet.VisualData, sizeof(request));
        float speed = 0;
        std::memcpy(&speed, &packet.ParameterA, sizeof(speed));
        if (!packet.MotionHash || !request.RigKey || packet.FlagA > 1 ||
            packet.RequestedVariant < -1 || request.BlendEnabled > 1 ||
            request.Reserved[0] || request.Reserved[1] || request.Reserved[2] ||
            request.Track >= 8 || request.SelectedVariants > 1 ||
            (request.SelectedVariants && !request.CatalogKey) ||
            request.QueueReserved[0] || request.QueueReserved[1] ||
            !std::isfinite(speed) || speed < 0 || speed > 100) return false;
        for (unsigned i = 0; i < 4; ++i)
            if (request.Variants[i] < -1 || request.Variants[i] > 65535 ||
                !std::isfinite(request.Weights[i]) || request.Weights[i] < 0 || request.Weights[i] > 1) return false;
        for (const auto value : request.Blend)
            if (!std::isfinite(value)) return false;
        return true;
    }

    // Serial arithmetic: zero is a valid sequence after wraparound.
    constexpr bool IsNewer(std::uint32_t value, std::uint32_t previous)
    {
        const auto distance = value - previous;
        return distance != 0 && distance < 0x80000000u;
    }
}
