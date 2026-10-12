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

    struct PlayerSnapshot
    {
        std::uint32_t DefinitionHandle = 0;
        std::uint32_t PlayerSetupValue = 0;
        std::uint32_t Flags = 0;
        std::uint32_t Revision = 0;
    };

    enum PlayerSnapshotFlags : std::uint32_t
    {
        SnapshotHasTransformComponent = 1u << 0,
        SnapshotHasActorStateComponent = 1u << 1,
        SnapshotHasComponent15 = 1u << 2
    };

    struct PeerInfo
    {
        std::uint32_t PlayerId = 0;
        std::string Address;
        std::uint16_t Port = 0;
        std::uint32_t PingMs = 0;
        std::uint64_t LastSeenMs = 0;
        std::uint64_t LastTransformMs = 0;
        bool Connected = false;
        bool HasTransform = false;
        bool HasSnapshot = false;
        std::uint32_t VisualRevision = 0;
        Vec3 Position{};
        float Rotation[4]{ 0, 0, 0, 1 };
        bool HasRotation = false;
        PlayerSnapshot Snapshot{};
    };


    struct DiscoveredHost
    {
        std::string Address;
        std::uint16_t Port = 0;
        std::uint32_t Players = 0;
        std::uint32_t MaxPlayers = 0;
        std::uint32_t PingMs = 0;
        std::uint64_t LastSeenMs = 0;
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
        std::uint64_t ReceivedAtMs = 0; // receiver clock only; never serialized
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
    // Capture native metadata on the render/game pump; UDP work reads a copy.
    void CaptureLocalSnapshot();
    bool GetVisualState(std::uint32_t playerId, std::uint32_t& revision,
        std::vector<std::uint8_t>& bytes, std::uint32_t skipRevision = 0);

    bool Host(unsigned short port);
    bool Join(const std::string& address, unsigned short port);
    void Disconnect();

    Mode GetMode();
    std::string GetStatusText();
    std::string GetModeText();
    Stats GetStats();
    std::vector<PeerInfo> GetPeers();
    std::uint32_t GetLocalPlayerId();

    void RequestServerDiscovery(unsigned short port);
    std::vector<DiscoveredHost> GetDiscoveredHosts();

    bool SendMotionEvent(const MotionEventPacket& packet);
    bool PopMotionEvent(MotionEventPacket& packet);

    bool HasRemoteTransform();
    Vec3 GetRemoteTransform();
    std::uint64_t GetRemoteTransformAgeMs();
}
