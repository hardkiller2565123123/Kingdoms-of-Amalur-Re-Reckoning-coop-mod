#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace NetworkManager
{
    enum class Mode
    {
        Offline,
        Host,
        Client
    };

    struct Vec3
    {
        float X = 0.0f;
        float Y = 0.0f;
        float Z = 0.0f;
    };

    struct PeerInfo
    {
        std::uint32_t PlayerId = 0;
        std::string Address;
        std::uint16_t Port = 0;
        std::uint32_t PingMs = 0;
        std::uint64_t LastSeenMs = 0;
        bool Connected = false;
        bool HasTransform = false;
        Vec3 Position{};
    };

    struct MotionEventPacket
    {
        std::uint32_t ActorNetId = 0;
        std::uint32_t MotionHash = 0;
        std::int32_t RequestedVariant = 0;
        std::int32_t ParameterA = 0;
        std::int32_t ParameterB = 0;
        std::uint32_t SenderTick = 0;
        std::uint32_t PacketSequence = 0;
        std::uint8_t FlagA = 0;
    };

    struct Stats
    {
        std::uint64_t PacketsSent = 0;
        std::uint64_t PacketsReceived = 0;
        std::uint64_t BytesSent = 0;
        std::uint64_t BytesReceived = 0;
        std::uint64_t DroppedPackets = 0;
        std::uint32_t LocalPlayerId = 0;
        std::uint32_t CurrentPingMs = 0;
        std::uint16_t BoundPort = 0;
        std::size_t ConnectedPeers = 0;
    };

    void Initialize();
    void Shutdown();
    void Update();

    bool Host(unsigned short port);
    bool Join(const std::string& address, unsigned short port);
    void Disconnect();

    Mode GetMode();
    std::string GetStatusText();
    std::string GetModeText();
    Stats GetStats();
    std::vector<PeerInfo> GetPeers();
    std::uint32_t GetLocalPlayerId();

    bool SendMotionEvent(const MotionEventPacket& packet);
    bool PopMotionEvent(MotionEventPacket& packet);

    bool HasRemoteTransform();
    Vec3 GetRemoteTransform();
    std::uint64_t GetRemoteTransformAgeMs();
}
