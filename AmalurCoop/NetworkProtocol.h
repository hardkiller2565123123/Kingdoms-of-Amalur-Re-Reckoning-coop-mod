#pragma once
#include <cstddef>
#include <cstdint>

namespace AmalurNet
{
    constexpr std::uint32_t Magic = 0x504F4341; // ACOP
    constexpr std::uint16_t ProtocolVersion = 3;
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
        PlayerLeft = 8
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
    };
#pragma pack(pop)

    static_assert(sizeof(Packet) == 64, "Unexpected Amalur network packet size");
}
