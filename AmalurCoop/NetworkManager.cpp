#include "NetworkManager.h"

#include "Config.h"
#include "EngineRuntime.h"
#include "LobbyManager.h"
#include "Logger.h"
#include "NetworkProtocol.h"
#include "CharacterVisualRuntime.h"
#include "VisualTransfer.h"
#include <map>
#include "PositionTracker.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <Windows.h>
#include <iphlpapi.h>
#include <bcrypt.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <utility>
#include <vector>
#include <set>
#include <iterator>
#include <cstdlib>

#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "Iphlpapi.lib")
#pragma comment(lib, "Bcrypt.lib")

namespace
{
    using AmalurNet::Packet;
    using AmalurNet::PacketType;

    constexpr std::uint64_t kTransformIntervalMs = 33;
    constexpr std::uint64_t kHelloIntervalMs = 1000;
    constexpr std::uint64_t kHeartbeatIntervalMs = 1000;
    constexpr std::uint64_t kSnapshotIntervalMs = 2000;
    constexpr std::uint64_t kTimeoutMs = 8000;
    constexpr std::uint64_t kDiscoveryExpireMs = 6500;

    constexpr unsigned int kTransformComponentId = 6;
    constexpr unsigned int kActorStateComponentId = 7;
    constexpr unsigned int kComponent15Id = 15;

    struct DiscoveredHostState
    {
        sockaddr_in Address{};
        std::uint32_t Players = 0;
        std::uint32_t MaxPlayers = 0;
        std::uint32_t PingMs = 0;
        std::uint64_t LastSeenMs = 0;
    };

    struct PeerState
    {
        sockaddr_in Address{};
        bool HasAddress = false;
        std::uint32_t PlayerId = 0;
        std::uint32_t LastSequence = 0;
        std::uint32_t LastMotionSequence = 0;
        bool HasMotionSequence = false;
        std::uint64_t ConnectionId = 0;
        std::uint64_t LastTransformMs = 0;
        std::uint64_t LastSeenMs = 0;
        std::uint32_t PingMs = 0;
        bool Connected = false;
        bool HasTransform = false;
        bool HasSnapshot = false;
        NetworkManager::Vec3 Position{};
        float Rotation[4]{ 0, 0, 0, 1 };
        bool HasRotation = false;
        NetworkManager::PlayerSnapshot Snapshot{};
        AmalurNet::VisualTransfer VisualPending;
        std::vector<std::uint8_t> VisualBytes;
        std::uint32_t VisualRevision = 0;
        std::map<std::uint32_t, std::uint32_t> VisualAcks;
        std::map<std::uint32_t, std::uint64_t> VisualSent;
    };

    std::mutex g_mutex;
    SOCKET g_socket = INVALID_SOCKET;
    bool g_winsockReady = false;
    std::atomic<NetworkManager::Mode> g_mode{ NetworkManager::Mode::Offline };

    sockaddr_in g_server{};
    bool g_hasServer = false;
    bool g_autoDiscover = false;
    std::string g_status = "Offline";

    std::vector<PeerState> g_peers;
    std::vector<DiscoveredHostState> g_discoveredHosts;
    std::deque<NetworkManager::MotionEventPacket> g_motionQueue;

    std::uint32_t g_localPlayerId = 0;
    std::uint32_t g_nextHostPlayerId = 2;
    std::uint32_t g_sequence = 0;
    std::uint64_t g_sessionId = 0;
    std::uint64_t g_connectionId = 0;
    std::uint32_t g_rosterSequence = 0;
    bool g_hasRoster = false;
    std::set<std::uint32_t> g_rosterIds;
    std::uint64_t g_lastRoster = 0;
    std::vector<std::uint8_t> g_localVisual;
    std::uint32_t g_localVisualRevision = 0;
    std::uint64_t g_lastVisualCapture = 0;
    float g_localRotation[4]{ 0, 0, 0, 1 };
    bool g_hasLocalRotation = false;
    std::deque<std::pair<sockaddr_in, Packet>> g_visualOutgoing;

    std::uint64_t NewConnectionId()
    {
        std::uint64_t value = 0;
        if (BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(&value), sizeof(value),
            BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
            return 0;
        return value;
    }
    std::uint32_t g_serverPingMs = 0;

    NetworkManager::PlayerSnapshot g_localSnapshot{};
    bool g_hasLocalSnapshot = false;
    std::uint32_t g_localSnapshotRevision = 0;

    std::uint64_t g_lastHello = 0;
    std::uint64_t g_lastTransform = 0;
    std::uint64_t g_lastHeartbeat = 0;
    std::uint64_t g_lastSnapshot = 0;
    std::uint64_t g_serverLastSeen = 0;
    std::uint16_t g_port = AmalurNet::DefaultPort;

    std::uint64_t g_packetsSent = 0;
    std::uint64_t g_packetsReceived = 0;
    std::uint64_t g_bytesSent = 0;
    std::uint64_t g_bytesReceived = 0;
    std::uint64_t g_dropped = 0;

    std::uint64_t NowMs()
    {
        return GetTickCount64();
    }

    bool SendTo(const sockaddr_in& to, Packet packet);
    PeerState* FindPeerByAddress(const sockaddr_in& address);
    void SendRosterTo(const sockaddr_in& address);

    bool SameAddress(const sockaddr_in& a, const sockaddr_in& b)
    {
        return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port;
    }

    std::string AddressText(const sockaddr_in& address)
    {
        char ip[INET_ADDRSTRLEN]{};
        if (!inet_ntop(AF_INET, &address.sin_addr, ip, sizeof(ip)))
            return "unknown";
        return ip;
    }

    bool SameHostEndpoint(const sockaddr_in& a, const sockaddr_in& b)
    {
        return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port;
    }

    void PruneDiscoveredHosts(std::uint64_t now)
    {
        g_discoveredHosts.erase(
            std::remove_if(g_discoveredHosts.begin(), g_discoveredHosts.end(),
                [now](const DiscoveredHostState& host)
                {
                    return now - host.LastSeenMs > kDiscoveryExpireMs;
                }),
            g_discoveredHosts.end());
    }

    void StoreDiscoveryResponse(const Packet& packet, const sockaddr_in& from)
    {
        const std::uint64_t now = NowMs();
        sockaddr_in endpoint = from;
        if (packet.SenderTick > 0 && packet.SenderTick <= 65535)
            endpoint.sin_port = htons(static_cast<unsigned short>(packet.SenderTick));

        auto it = std::find_if(g_discoveredHosts.begin(), g_discoveredHosts.end(),
            [&endpoint](const DiscoveredHostState& host)
            {
                return SameHostEndpoint(host.Address, endpoint);
            });

        if (it == g_discoveredHosts.end())
        {
            g_discoveredHosts.push_back({});
            it = std::prev(g_discoveredHosts.end());
            it->Address = endpoint;
        }

        it->Players = packet.ParameterA > 0 ? static_cast<std::uint32_t>(packet.ParameterA) : 1u;
        it->MaxPlayers = packet.ParameterB > 0 ? static_cast<std::uint32_t>(packet.ParameterB) : 4u;
        it->PingMs = packet.TimestampMs <= now
            ? static_cast<std::uint32_t>(std::min<std::uint64_t>(9999, now - packet.TimestampMs))
            : 0u;
        it->LastSeenMs = now;
    }

    void ReplyToDiscovery(const Packet& request, const sockaddr_in& from)
    {
        if (g_mode.load(std::memory_order_acquire) != NetworkManager::Mode::Host)
            return;

        Packet response{};
        response.Type = PacketType::DiscoveryResponse;
        response.PlayerId = 1;
        response.Sequence = ++g_sequence;
        response.TimestampMs = request.TimestampMs ? request.TimestampMs : NowMs();
        response.ParameterA = LobbyManager::GetPlayerCount();
        response.ParameterB = LobbyManager::GetMaxPlayers();
        response.SenderTick = g_port;
        SendTo(from, response);
    }

    std::vector<sockaddr_in> BuildDiscoveryTargets(unsigned short port)
    {
        std::vector<sockaddr_in> result;
        std::set<std::uint32_t> addresses;

        auto addTarget = [&](std::uint32_t address)
        {
            if (!addresses.insert(address).second)
                return;

            sockaddr_in target{};
            target.sin_family = AF_INET;
            target.sin_port = htons(port);
            target.sin_addr.s_addr = address;
            result.push_back(target);
        };

        // Limited broadcast plus loopback cover normal LAN and same-PC testing.
        addTarget(htonl(INADDR_BROADCAST));
        addTarget(htonl(INADDR_LOOPBACK));

        // Also send a directed broadcast to every active IPv4 adapter. This is
        // important for Radmin/Hamachi-style virtual adapters where the useful
        // broadcast is often not the machine's default physical adapter.
        ULONG bufferSize = 16 * 1024;
        std::vector<unsigned char> buffer(bufferSize);
        auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
        const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
        ULONG status = GetAdaptersAddresses(AF_INET, flags, nullptr, adapters, &bufferSize);
        if (status == ERROR_BUFFER_OVERFLOW)
        {
            buffer.resize(bufferSize);
            adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
            status = GetAdaptersAddresses(AF_INET, flags, nullptr, adapters, &bufferSize);
        }

        if (status == NO_ERROR)
        {
            for (auto* adapter = adapters; adapter; adapter = adapter->Next)
            {
                if (adapter->OperStatus != IfOperStatusUp)
                    continue;

                for (auto* unicast = adapter->FirstUnicastAddress; unicast; unicast = unicast->Next)
                {
                    if (!unicast->Address.lpSockaddr || unicast->Address.lpSockaddr->sa_family != AF_INET)
                        continue;

                    const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(unicast->Address.lpSockaddr);
                    const std::uint32_t hostIp = ntohl(ipv4->sin_addr.s_addr);
                    const ULONG prefix = std::min<ULONG>(unicast->OnLinkPrefixLength, 32);
                    const std::uint32_t mask = prefix == 0 ? 0u : (0xFFFFFFFFu << (32 - prefix));
                    const std::uint32_t broadcast = (hostIp & mask) | (~mask);
                    addTarget(htonl(broadcast));
                }
            }
        }

        return result;
    }

    void CloseSocket()
    {
        if (g_socket != INVALID_SOCKET)
        {
            closesocket(g_socket);
            g_socket = INVALID_SOCKET;
        }
    }

    bool OpenSocket(unsigned short bindPort)
    {
        CloseSocket();

        g_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (g_socket == INVALID_SOCKET)
            return false;

        BOOL enabled = TRUE;
        setsockopt(g_socket, SOL_SOCKET, SO_BROADCAST,
            reinterpret_cast<const char*>(&enabled), sizeof(enabled));
        setsockopt(g_socket, SOL_SOCKET, SO_REUSEADDR,
            reinterpret_cast<const char*>(&enabled), sizeof(enabled));

        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = htonl(INADDR_ANY);
        local.sin_port = htons(bindPort);

        if (bind(g_socket, reinterpret_cast<sockaddr*>(&local), sizeof(local)) == SOCKET_ERROR)
        {
            CloseSocket();
            return false;
        }

        u_long nonBlocking = 1;
        if (ioctlsocket(g_socket, FIONBIO, &nonBlocking) == SOCKET_ERROR)
        {
            CloseSocket();
            return false;
        }

        return true;
    }

    void ResetSessionState()
    {
        g_server = {};
        g_hasServer = false;
        g_autoDiscover = false;
        g_peers.clear();
        g_motionQueue.clear();
        g_localPlayerId = 0;
        g_nextHostPlayerId = 2;
        g_sequence = 0;
        g_sessionId = 0;
        g_connectionId = NewConnectionId();
        g_hasRoster = false;
        g_rosterIds.clear();
        g_rosterSequence = 0;
        g_lastRoster = 0;
        g_localVisual.clear();
        g_localVisualRevision = 0;
        g_lastVisualCapture = 0;
        g_hasLocalRotation = false;
        g_visualOutgoing.clear();
        g_serverPingMs = 0;
        g_localSnapshot = {};
        g_hasLocalSnapshot = false;
        g_localSnapshotRevision = 0;
        g_lastHello = 0;
        g_lastTransform = 0;
        g_lastHeartbeat = 0;
        g_lastSnapshot = 0;
        g_serverLastSeen = 0;
    }

    bool SendTo(const sockaddr_in& to, Packet packet)
    {
        if (g_socket == INVALID_SOCKET)
            return false;

        packet.MagicValue = AmalurNet::Magic;
        packet.Version = AmalurNet::ProtocolVersion;
        packet.SessionId = g_sessionId;
        if (g_mode.load(std::memory_order_acquire) == NetworkManager::Mode::Host)
        {
            const auto* recipient = FindPeerByAddress(to);
            packet.ConnectionId = recipient ? recipient->ConnectionId : 0;
        }
        else
            packet.ConnectionId = g_connectionId;
        if (!packet.TimestampMs)
            packet.TimestampMs = NowMs();

        const int sent = sendto(
            g_socket,
            reinterpret_cast<const char*>(&packet),
            sizeof(packet),
            0,
            reinterpret_cast<const sockaddr*>(&to),
            sizeof(to));

        if (sent != sizeof(packet))
        {
            ++g_dropped;
            return false;
        }

        ++g_packetsSent;
        g_bytesSent += static_cast<std::uint64_t>(sent);
        return true;
    }

    PeerState* FindPeer(std::uint32_t id)
    {
        for (auto& peer : g_peers)
        {
            if (peer.PlayerId == id)
                return &peer;
        }
        return nullptr;
    }

    PeerState* FindPeerByAddress(const sockaddr_in& address)
    {
        for (auto& peer : g_peers)
        {
            if (peer.HasAddress && SameAddress(peer.Address, address))
                return &peer;
        }
        return nullptr;
    }

    PeerState& GetOrAddPeer(std::uint32_t id)
    {
        if (auto* peer = FindPeer(id))
            return *peer;

        g_peers.push_back({});
        g_peers.back().PlayerId = id;
        return g_peers.back();
    }

    void UpdateLobby()
    {
        std::size_t playerCount = g_localPlayerId ? 1u : 0u;
        for (const auto& peer : g_peers)
        {
            if (peer.Connected && peer.PlayerId != g_localPlayerId)
                ++playerCount;
        }

        LobbyManager::SetPlayerCount(
            static_cast<int>(std::max<std::size_t>(1u, playerCount)));
    }

    bool ResolveServer(const std::string& address, unsigned short port)
    {
        g_server = {};
        g_server.sin_family = AF_INET;
        g_server.sin_port = htons(port);

        if (address.empty() || address == "auto")
        {
            g_server.sin_addr.s_addr = htonl(INADDR_BROADCAST);
            g_autoDiscover = true;
            g_hasServer = true;
            return true;
        }

        g_autoDiscover = false;
        if (inet_pton(AF_INET, address.c_str(), &g_server.sin_addr) == 1)
        {
            g_hasServer = true;
            return true;
        }

        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;

        addrinfo* result = nullptr;
        if (getaddrinfo(address.c_str(), nullptr, &hints, &result) != 0 || !result)
            return false;

        g_server.sin_addr = reinterpret_cast<sockaddr_in*>(result->ai_addr)->sin_addr;
        freeaddrinfo(result);
        g_hasServer = true;
        return true;
    }

    void BroadcastHostPacket(Packet packet, std::uint32_t exceptPlayerId = 0)
    {
        for (const auto& peer : g_peers)
        {
            if (!peer.Connected || !peer.HasAddress || peer.PlayerId == exceptPlayerId)
                continue;
            SendTo(peer.Address, packet);
        }
    }

    NetworkManager::PlayerSnapshot BuildLocalSnapshot()
    {
        NetworkManager::PlayerSnapshot snapshot{};
        const uintptr_t object = PositionTracker::GetRuntimeObject();
        if (!object || !EngineRuntime::IsActiveObject(object))
            return snapshot;

        snapshot.DefinitionHandle = PositionTracker::GetDefinitionHandle();
        snapshot.PlayerSetupValue = PositionTracker::GetPlayerSetupValue();

        if (EngineRuntime::GetComponent(object, kTransformComponentId, true))
            snapshot.Flags |= NetworkManager::SnapshotHasTransformComponent;
        if (EngineRuntime::GetComponent(object, kActorStateComponentId, true))
            snapshot.Flags |= NetworkManager::SnapshotHasActorStateComponent;
        if (EngineRuntime::GetComponent(object, kComponent15Id, true))
            snapshot.Flags |= NetworkManager::SnapshotHasComponent15;

        return snapshot;
    }

    bool EnsureLocalSnapshot()
    {
        return g_hasLocalSnapshot;
    }

    Packet MakeSnapshotPacket(std::uint32_t playerId, const NetworkManager::PlayerSnapshot& snapshot)
    {
        Packet packet{};
        packet.Type = PacketType::PlayerSnapshot;
        packet.PlayerId = playerId;
        packet.Sequence = ++g_sequence;
        packet.DefinitionHandle = snapshot.DefinitionHandle;
        packet.PlayerSetupValue = snapshot.PlayerSetupValue;
        packet.SnapshotFlags = snapshot.Flags;
        packet.SnapshotRevision = snapshot.Revision;
        return packet;
    }

    void SendLocalSnapshotTo(const sockaddr_in& address)
    {
        if (!g_localPlayerId || !EnsureLocalSnapshot())
            return;
        SendTo(address, MakeSnapshotPacket(g_localPlayerId, g_localSnapshot));
    }

    void SendLocalTransformTo(const sockaddr_in& address)
    {
        if (!g_localPlayerId || !PositionTracker::HasPosition())
            return;

        const auto position = PositionTracker::GetPosition();
        Packet packet{};
        packet.Type = PacketType::Transform;
        packet.PlayerId = g_localPlayerId;
        packet.Sequence = ++g_sequence;
        packet.X = position.X;
        packet.Y = position.Y;
        packet.Z = position.Z;
        if (g_hasLocalRotation)
        {
            packet.Reserved |= 1;
            std::memcpy(packet.Rotation, g_localRotation, sizeof(packet.Rotation));
        }
        SendTo(address, packet);
    }

    void SendPeerJoinedTo(const sockaddr_in& address, std::uint32_t playerId)
    {
        Packet joined{};
        joined.Type = PacketType::PeerJoined;
        joined.PlayerId = playerId;
        joined.Sequence = ++g_sequence;
        SendTo(address, joined);
    }

    void SendStoredPeerStateTo(const sockaddr_in& address, const PeerState& peer)
    {
        if (!peer.PlayerId || !peer.Connected)
            return;

        SendPeerJoinedTo(address, peer.PlayerId);

        if (peer.HasTransform)
        {
            Packet transform{};
            transform.Type = PacketType::Transform;
            transform.PlayerId = peer.PlayerId;
            transform.Sequence = peer.LastSequence ? peer.LastSequence : ++g_sequence;
            transform.X = peer.Position.X;
            transform.Y = peer.Position.Y;
            transform.Z = peer.Position.Z;
            if (peer.HasRotation)
            {
                transform.Reserved |= 1;
                std::memcpy(transform.Rotation, peer.Rotation, sizeof(transform.Rotation));
            }
            SendTo(address, transform);
        }

        if (peer.HasSnapshot)
            SendTo(address, MakeSnapshotPacket(peer.PlayerId, peer.Snapshot));
    }

    void QueueMotion(const Packet& packet, PeerState& peer)
    {
        const std::uint32_t sequence = packet.Sequence;
        if (peer.HasMotionSequence && !AmalurNet::IsNewer(sequence, peer.LastMotionSequence))
        {
            ++g_dropped;
            return;
        }

        peer.LastMotionSequence = sequence;
        peer.HasMotionSequence = true;

        NetworkManager::MotionEventPacket event{};
        event.ActorNetId = packet.PlayerId;
        event.MotionHash = packet.MotionHash;
        event.RequestedVariant = packet.RequestedVariant;
        event.ParameterA = packet.ParameterA;
        event.ParameterB = packet.ParameterB;
        event.SenderTick = packet.SenderTick;
        event.PacketSequence = sequence;
        event.FlagA = packet.FlagA;
        event.ReceivedAtMs = NowMs();

        if (g_motionQueue.size() >= 128)
            g_motionQueue.pop_front();
        g_motionQueue.push_back(event);
    }

    bool ReceiveVisual(const Packet& packet, PeerState& peer)
    {
        if (!packet.VisualRevision) return false;
        if (peer.VisualRevision && packet.VisualRevision != peer.VisualRevision &&
            !AmalurNet::IsNewer(packet.VisualRevision, peer.VisualRevision)) return false;
        if (packet.VisualRevision != peer.VisualRevision)
        {
            if (!peer.VisualPending.Accept(packet)) return false;
            if (!peer.VisualPending.Complete()) return true;
            std::vector<std::uint8_t> validated;
            if (!CharacterVisualArchive::Normalize(peer.VisualPending.Bytes,
                CharacterVisualRuntime::RenderingTag, false, validated))
            {
                peer.VisualPending = {};
                return false;
            }
            std::uint32_t nativeLength = 0;
            std::memcpy(&nativeLength, validated.data() + 5, 4);
            if (validated.size() != nativeLength + 9 + CharacterVisualArchive::MaterialBytes)
            {
                peer.VisualPending = {};
                return false;
            }
            peer.VisualRevision = packet.VisualRevision;
            peer.VisualBytes = std::move(validated);
            peer.VisualPending = {};
        }
        Packet ack{};
        ack.Type = PacketType::VisualAck;
        ack.PlayerId = peer.PlayerId;
        ack.VisualRevision = peer.VisualRevision;
        if (g_mode.load() == NetworkManager::Mode::Host)
            SendTo(peer.Address, ack);
        else
            SendTo(g_server, ack);
        return true;
    }

    void QueueVisual(const sockaddr_in& address, PeerState& recipient, std::uint32_t source,
        std::uint32_t revision, const std::vector<std::uint8_t>& bytes, std::uint64_t now)
    {
        if (!revision || bytes.empty() || recipient.VisualAcks[source] == revision ||
            (recipient.VisualSent.count(source) && now - recipient.VisualSent[source] < 2000)) return;
        const auto count = (bytes.size() + 959) / 960;
        // Bound retained work. Unacknowledged snapshots retry on the next pass.
        if (g_visualOutgoing.size() + count > 4096) return;
        recipient.VisualSent[source] = now;
        for (std::size_t i = 0; i < count; ++i)
        {
            Packet chunk{};
            chunk.Type = PacketType::VisualChunk; chunk.PlayerId = source;
            chunk.VisualRevision = revision; chunk.VisualBytes = static_cast<std::uint32_t>(bytes.size());
            chunk.VisualIndex = static_cast<std::uint16_t>(i); chunk.VisualCount = static_cast<std::uint16_t>(count);
            std::memcpy(chunk.VisualData, bytes.data() + i * 960, std::min<std::size_t>(960, bytes.size() - i * 960));
            g_visualOutgoing.emplace_back(address, chunk);
        }
    }

    void PumpVisuals(std::uint64_t now)
    {
        if (!g_localPlayerId) return;
        if (g_mode.load() == NetworkManager::Mode::Host)
        {
            for (auto& recipient : g_peers)
            {
                QueueVisual(recipient.Address, recipient, 1, g_localVisualRevision, g_localVisual, now);
                for (const auto& source : g_peers)
                    if (source.PlayerId != recipient.PlayerId)
                        QueueVisual(recipient.Address, recipient, source.PlayerId,
                            source.VisualRevision, source.VisualBytes, now);
            }
        }
        else if (auto* host = FindPeer(1))
            QueueVisual(g_server, *host, g_localPlayerId, g_localVisualRevision, g_localVisual, now);
        for (unsigned i = 0; i < 32 && !g_visualOutgoing.empty(); ++i)
        {
            auto job = std::move(g_visualOutgoing.front()); g_visualOutgoing.pop_front();
            if (g_mode.load() == NetworkManager::Mode::Host)
            {
                if (!FindPeerByAddress(job.first) ||
                    (job.second.PlayerId != 1 && !FindPeer(job.second.PlayerId))) continue;
            }
            SendTo(job.first, job.second);
        }
    }

    bool ApplyRemotePacket(const Packet& packet)
    {
        if (!packet.PlayerId || packet.PlayerId == g_localPlayerId)
            return false;

        // Roster membership is authoritative. State cannot resurrect a player
        // after removal or manufacture an unbounded number of peer entries.
        auto* found = FindPeer(packet.PlayerId);
        if (!found || !found->Connected)
            return false;
        auto& peer = *found;
        switch (packet.Type)
        {
        case PacketType::VisualChunk:
            if (!ReceiveVisual(packet, peer)) return false;
            break;
        case PacketType::Transform:
            if ((peer.HasTransform && !AmalurNet::IsNewer(packet.Sequence, peer.LastSequence)) ||
                !std::isfinite(packet.X) || !std::isfinite(packet.Y) || !std::isfinite(packet.Z))
                return false;
            if (packet.Reserved & ~1u) return false;
            if (packet.Reserved & 1)
            {
                float length = 0;
                for (const float value : packet.Rotation)
                {
                    if (!std::isfinite(value)) return false;
                    length += value * value;
                }
                if (!std::isfinite(length) || length < 0.5f || length > 1.5f) return false;
                for (unsigned i = 0; i < 4; ++i) peer.Rotation[i] = packet.Rotation[i] / std::sqrt(length);
                peer.HasRotation = true;
            }
            peer.LastSequence = packet.Sequence;
            peer.Position = { packet.X, packet.Y, packet.Z };
            peer.HasTransform = true;
            peer.LastTransformMs = NowMs();
            break;
        case PacketType::PlayerSnapshot:
            if (!packet.SnapshotRevision || packet.DefinitionHandle < 2 ||
                (packet.SnapshotFlags & ~7u) ||
                (peer.HasSnapshot && packet.SnapshotRevision != peer.Snapshot.Revision &&
                 !AmalurNet::IsNewer(packet.SnapshotRevision, peer.Snapshot.Revision)))
                return false;
            peer.Snapshot = { packet.DefinitionHandle, packet.PlayerSetupValue,
                packet.SnapshotFlags, packet.SnapshotRevision };
            peer.HasSnapshot = true;
            break;
        case PacketType::MotionEvent:
        {
            const auto seq = packet.Sequence;
            if (!packet.MotionHash || packet.FlagA > 1 ||
                (peer.HasMotionSequence && !AmalurNet::IsNewer(seq, peer.LastMotionSequence)))
                return false;
            QueueMotion(packet, peer);
            break;
        }
        default:
            return false;
        }
        peer.LastSeenMs = NowMs();
        return true;
    }

    void PurgePeerMotion(std::uint32_t playerId)
    {
        g_motionQueue.erase(std::remove_if(g_motionQueue.begin(), g_motionQueue.end(),
            [playerId](const NetworkManager::MotionEventPacket& event)
            { return event.ActorNetId == playerId; }), g_motionQueue.end());
    }

    bool ApplyRoster(const Packet& packet)
    {
        if (g_hasRoster && !AmalurNet::IsNewer(packet.Sequence, g_rosterSequence))
            return false;
        std::set<std::uint32_t> ids;
        for (auto id : packet.RosterIds)
            if (id && !ids.insert(id).second)
                return false;
        if (!ids.count(1) || !ids.count(g_localPlayerId))
            return false;
        for (auto it = g_peers.begin(); it != g_peers.end();)
        {
            if (!ids.count(it->PlayerId))
            {
                PurgePeerMotion(it->PlayerId);
                it = g_peers.erase(it);
            }
            else
                ++it;
        }
        for (auto id : ids)
        {
            if (id == g_localPlayerId)
                continue;
            auto& peer = GetOrAddPeer(id);
            peer.Connected = true;
            peer.LastSeenMs = NowMs();
        }
        g_rosterIds = std::move(ids);
        g_rosterSequence = packet.Sequence;
        g_hasRoster = true;
        UpdateLobby();
        return true;
    }

    void SendHello(std::uint64_t now)
    {
        if (g_mode.load(std::memory_order_acquire) != NetworkManager::Mode::Client ||
            !g_hasServer || g_localPlayerId || now - g_lastHello < kHelloIntervalMs)
        {
            return;
        }

        Packet packet{};
        packet.Type = PacketType::Hello;
        packet.Sequence = ++g_sequence;
        SendTo(g_server, packet);
        g_lastHello = now;
    }

    void HandleClientPacket(const Packet& packet, const sockaddr_in& from)
    {
        const std::uint64_t now = NowMs();

        if (!g_hasServer ||
            ((!g_autoDiscover || g_localPlayerId) && !SameAddress(from, g_server)) ||
            packet.ConnectionId != g_connectionId || !g_connectionId)
        {
            ++g_dropped;
            return;
        }

        if (packet.Type == PacketType::Welcome)
        {
            if (g_localPlayerId || packet.PlayerId < 2 || !packet.SessionId)
                return;
            g_sessionId = packet.SessionId;
            g_server = from;
            g_hasServer = true;
            g_localPlayerId = packet.PlayerId;
            g_serverLastSeen = now;

            // Player 1 is always the host. Create it immediately so the client
            // has a deterministic remote-player target before the next normal
            // transform tick arrives.
            auto& host = GetOrAddPeer(1);
            host.PlayerId = 1;
            host.Address = from;
            host.HasAddress = true;
            host.Connected = true;
            host.LastSeenMs = now;

            g_status = "Connected as player " + std::to_string(g_localPlayerId);
            Logger::Write(g_status);
            UpdateLobby();

            // Send our player metadata immediately instead of waiting for the
            // periodic snapshot tick.
            SendLocalSnapshotTo(g_server);
            SendLocalTransformTo(g_server);
            return;
        }

        if (!g_localPlayerId || packet.SessionId != g_sessionId)
            return;

        if (packet.Type == PacketType::VisualAck)
        {
            if (packet.PlayerId == g_localPlayerId && packet.VisualRevision == g_localVisualRevision)
                if (auto* host = FindPeer(1)) host->VisualAcks[g_localPlayerId] = packet.VisualRevision;
            return;
        }
        if (packet.Type == PacketType::Roster)
        {
            if (ApplyRoster(packet))
                g_serverLastSeen = now;
            else
                ++g_dropped;
            return;
        }

        if (packet.Type == PacketType::Ping && packet.PlayerId == 1)
        {
            g_serverLastSeen = now;
            Packet pong{};
            pong.Type = PacketType::Pong;
            pong.PlayerId = g_localPlayerId;
            pong.Sequence = packet.Sequence;
            pong.TimestampMs = packet.TimestampMs;
            SendTo(g_server, pong);
            return;
        }

        if (packet.Type == PacketType::Pong && packet.PlayerId == 1)
        {
            g_serverLastSeen = now;
            if (packet.TimestampMs <= now)
            {
                g_serverPingMs = static_cast<std::uint32_t>(
                    std::min<std::uint64_t>(9999, now - packet.TimestampMs));
            }
            return;
        }

        if (packet.Type == PacketType::Goodbye && packet.PlayerId == 1)
        {
            g_localPlayerId = 0;
            g_peers.clear();
            g_motionQueue.clear();
            g_visualOutgoing.clear();
            g_sessionId = 0;
            g_connectionId = NewConnectionId();
            g_hasRoster = false;
            g_hasServer = false;
            g_status = "Host disconnected";
            UpdateLobby();
            return;
        }

        // Roster packets replace the old lossy joined/left notifications.
        if (ApplyRemotePacket(packet))
            g_serverLastSeen = now;
        else
            ++g_dropped;
    }

    void RemoveHostPeer(std::uint32_t playerId)
    {
        auto it = std::find_if(
            g_peers.begin(), g_peers.end(),
            [playerId](const PeerState& peer)
            {
                return peer.PlayerId == playerId;
            });

        if (it == g_peers.end())
            return;

        Packet left{};
        left.Type = PacketType::PlayerLeft;
        left.PlayerId = playerId;
        left.Sequence = ++g_sequence;
        BroadcastHostPacket(left, playerId);

        Logger::WriteFormat(
            Logger::Level::Info,
            "Network peer %u disconnected",
            playerId);

        PurgePeerMotion(playerId);
        g_peers.erase(it);
        for (const auto& remaining : g_peers)
            SendRosterTo(remaining.Address);
        UpdateLobby();
    }

    void SendRosterTo(const sockaddr_in& address)
    {
        Packet roster{};
        roster.Type = PacketType::Roster;
        roster.PlayerId = 1;
        roster.Sequence = ++g_sequence;
        roster.RosterIds[0] = 1;
        std::size_t index = 1;
        for (const auto& peer : g_peers)
            if (peer.Connected && index < AmalurNet::MaxPlayers)
                roster.RosterIds[index++] = peer.PlayerId;
        SendTo(address, roster);
    }

    void SendInitialRosterTo(PeerState& newcomer)
    {
        SendRosterTo(newcomer.Address);
        // The host is always player 1.
        SendPeerJoinedTo(newcomer.Address, g_localPlayerId);
        SendLocalTransformTo(newcomer.Address);
        SendLocalSnapshotTo(newcomer.Address);

        for (const auto& peer : g_peers)
        {
            if (peer.PlayerId == newcomer.PlayerId)
                continue;
            SendStoredPeerStateTo(newcomer.Address, peer);
        }
    }

    void HandleHostPacket(Packet packet, const sockaddr_in& from)
    {
        const std::uint64_t now = NowMs();

        if (packet.Type == PacketType::Hello)
        {
            if (!packet.ConnectionId || packet.SessionId || packet.PlayerId)
            {
                ++g_dropped;
                return;
            }
            PeerState* peer = FindPeerByAddress(from);
            // A new challenge at a still-live endpoint cannot evict its owner.
            if (peer && peer->ConnectionId != packet.ConnectionId)
            {
                ++g_dropped;
                return;
            }
            const bool isNewPeer = peer == nullptr;
            if (!peer)
            {
                if (g_peers.size() >= static_cast<std::size_t>(
                    std::clamp(LobbyManager::GetMaxPlayers(), 1, static_cast<int>(AmalurNet::MaxPlayers)) - 1) ||
                    g_nextHostPlayerId < 2)
                {
                    ++g_dropped;
                    return;
                }

                g_peers.push_back({});
                peer = &g_peers.back();
                peer->Address = from;
                peer->HasAddress = true;
                peer->PlayerId = g_nextHostPlayerId++;
                peer->ConnectionId = packet.ConnectionId;
                peer->Connected = true;

                Logger::WriteFormat(
                    Logger::Level::Success,
                    "Network peer %u joined from %s:%u",
                    peer->PlayerId,
                    AddressText(from).c_str(),
                    static_cast<unsigned int>(ntohs(from.sin_port)));
            }

            peer->Connected = true;
            peer->LastSeenMs = now;

            Packet welcome{};
            welcome.Type = PacketType::Welcome;
            welcome.PlayerId = peer->PlayerId;
            welcome.Sequence = ++g_sequence;
            SendTo(peer->Address, welcome);

            if (isNewPeer)
            {
                // Existing clients learn about the newcomer immediately.
                Packet joined{};
                joined.Type = PacketType::PeerJoined;
                joined.PlayerId = peer->PlayerId;
                joined.Sequence = ++g_sequence;
                BroadcastHostPacket(joined, peer->PlayerId);
                for (const auto& existing : g_peers)
                    if (existing.PlayerId != peer->PlayerId)
                        SendRosterTo(existing.Address);
            }

            // The joining client receives the host plus all currently connected
            // peers and their latest known transform/snapshot state right away.
            SendInitialRosterTo(*peer);
            UpdateLobby();
            return;
        }

        PeerState* peer = FindPeerByAddress(from);
        if (!peer || packet.SessionId != g_sessionId ||
            packet.ConnectionId != peer->ConnectionId ||
            (packet.Type != PacketType::VisualAck && packet.PlayerId != peer->PlayerId))
        {
            ++g_dropped;
            return;
        }

        if (packet.Type == PacketType::VisualAck)
        {
            const auto* source = FindPeer(packet.PlayerId);
            const auto revision = packet.PlayerId == 1 ? g_localVisualRevision :
                (source ? source->VisualRevision : 0);
            if (revision && packet.VisualRevision == revision)
                peer->VisualAcks[packet.PlayerId] = revision;
            return;
        }
        if (packet.Type == PacketType::Goodbye)
        {
            const std::uint32_t playerId = peer->PlayerId;
            RemoveHostPeer(playerId);
            return;
        }

        if (packet.Type == PacketType::Ping)
        {
            peer->LastSeenMs = now;
            Packet pong{};
            pong.Type = PacketType::Pong;
            pong.PlayerId = g_localPlayerId;
            pong.Sequence = packet.Sequence;
            pong.TimestampMs = packet.TimestampMs;
            SendTo(peer->Address, pong);
            return;
        }

        if (packet.Type == PacketType::Pong)
        {
            peer->LastSeenMs = now;
            if (packet.TimestampMs <= now)
            {
                peer->PingMs = static_cast<std::uint32_t>(
                    std::min<std::uint64_t>(9999, now - packet.TimestampMs));
            }
            return;
        }

        // Clients do not author roster events. The host is authoritative.
        if (packet.Type == PacketType::PeerJoined || packet.Type == PacketType::PlayerLeft)
        {
            ++g_dropped;
            return;
        }

        if (ApplyRemotePacket(packet))
        {
            if (packet.Type != PacketType::VisualChunk)
                BroadcastHostPacket(packet, peer->PlayerId);
        }
        else
            ++g_dropped;
    }

    void SendLocalTransform(std::uint64_t now)
    {
        if (!g_localPlayerId || !PositionTracker::HasPosition() ||
            now - g_lastTransform < kTransformIntervalMs)
        {
            return;
        }

        const auto position = PositionTracker::GetPosition();
        Packet packet{};
        packet.Type = PacketType::Transform;
        packet.PlayerId = g_localPlayerId;
        packet.Sequence = ++g_sequence;
        packet.X = position.X;
        packet.Y = position.Y;
        packet.Z = position.Z;
        if (g_hasLocalRotation)
        {
            packet.Reserved |= 1;
            std::memcpy(packet.Rotation, g_localRotation, sizeof(packet.Rotation));
        }

        if (g_mode.load(std::memory_order_acquire) == NetworkManager::Mode::Host)
            BroadcastHostPacket(packet);
        else if (g_hasServer)
            SendTo(g_server, packet);

        g_lastTransform = now;
    }

    void SendLocalSnapshot(std::uint64_t now)
    {
        if (!g_localPlayerId || now - g_lastSnapshot < kSnapshotIntervalMs || !EnsureLocalSnapshot())
            return;

        Packet packet = MakeSnapshotPacket(g_localPlayerId, g_localSnapshot);
        if (g_mode.load(std::memory_order_acquire) == NetworkManager::Mode::Host)
            BroadcastHostPacket(packet);
        else if (g_hasServer)
            SendTo(g_server, packet);

        g_lastSnapshot = now;
    }

    void SendHeartbeat(std::uint64_t now)
    {
        if (!g_localPlayerId || now - g_lastHeartbeat < kHeartbeatIntervalMs)
            return;

        Packet ping{};
        ping.Type = PacketType::Ping;
        ping.PlayerId = g_localPlayerId;
        ping.Sequence = ++g_sequence;
        ping.TimestampMs = now;

        if (g_mode.load(std::memory_order_acquire) == NetworkManager::Mode::Host)
            BroadcastHostPacket(ping);
        else if (g_hasServer)
            SendTo(g_server, ping);

        g_lastHeartbeat = now;
    }

    void CheckHostTimeouts(std::uint64_t now)
    {
        std::vector<std::uint32_t> expired;
        for (const auto& peer : g_peers)
        {
            if (peer.Connected && now - peer.LastSeenMs > kTimeoutMs)
                expired.push_back(peer.PlayerId);
        }

        for (const std::uint32_t playerId : expired)
            RemoveHostPeer(playerId);
    }
}

namespace NetworkManager
{
    void CaptureLocalSnapshot()
    {
        // Native metadata is sampled at 30 Hz; wire transforms keep their own cadence.
        static ULONGLONG lastMetadataCapture = 0;
        const auto captureNow = GetTickCount64();
        if (lastMetadataCapture && captureNow - lastMetadataCapture < 33) return;
        lastMetadataCapture = captureNow;
        auto current = BuildLocalSnapshot();
        float rotation[4]{};
        const auto transform = PositionTracker::GetTransformComponent();
        bool hasRotation = transform && EngineRuntime::ReadBytes(transform + 0x34, rotation, sizeof(rotation));
        float length = 0;
        for (auto value : rotation) length += value * value;
        hasRotation = hasRotation && std::isfinite(length) && length > 0.5f && length < 1.5f;
        std::vector<std::uint8_t> visual;
        const auto now = NowMs();
        bool capture = false;
        std::uint64_t captureSession = 0, captureConnection = 0;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            captureSession = g_sessionId; captureConnection = g_connectionId;
            capture = g_localPlayerId && now - g_lastVisualCapture >= 1000;
            if (capture) g_lastVisualCapture = now;
        }
        const bool captured = capture && CharacterVisualRuntime::Capture(visual);
        std::lock_guard<std::mutex> lock(g_mutex);
        if (captureSession != g_sessionId || captureConnection != g_connectionId) return;
        g_hasLocalRotation = hasRotation;
        if (hasRotation) std::memcpy(g_localRotation, rotation, sizeof(rotation));
        if (captured && visual != g_localVisual)
        {
            g_localVisual = std::move(visual);
            if (!++g_localVisualRevision) ++g_localVisualRevision;
        }
        if (current.DefinitionHandle < 2)
        {
            g_hasLocalSnapshot = false;
            return;
        }
        if (!g_hasLocalSnapshot ||
            current.DefinitionHandle != g_localSnapshot.DefinitionHandle ||
            current.PlayerSetupValue != g_localSnapshot.PlayerSetupValue ||
            current.Flags != g_localSnapshot.Flags)
        {
            if (!++g_localSnapshotRevision)
                ++g_localSnapshotRevision;
            g_lastSnapshot = 0;
        }
        current.Revision = g_localSnapshotRevision;
        g_localSnapshot = current;
        g_hasLocalSnapshot = true;
    }

    void Initialize()
    {
        std::lock_guard<std::mutex> lock(g_mutex);

        WSADATA winsock{};
        g_winsockReady = WSAStartup(MAKEWORD(2, 2), &winsock) == 0;
        if (!g_winsockReady)
        {
            g_status = "Winsock initialization failed";
            return;
        }

        ResetSessionState();
        if (!OpenSocket(0))
        {
            g_status = "UDP socket initialization failed";
            return;
        }

        const Config::Settings settings = Config::Get();
        g_port = static_cast<std::uint16_t>(settings.ServerPort);

        if (settings.AutoConnect)
        {
            if (!ResolveServer(settings.ServerAddress, g_port))
            {
                g_status = "Invalid server address";
                g_mode.store(Mode::Offline, std::memory_order_release);
                return;
            }

            g_mode.store(Mode::Client, std::memory_order_release);
            g_status = g_autoDiscover
                ? "Searching for an AmalurCoop host"
                : "Connecting to AmalurCoop host";
        }
        else
        {
            g_mode.store(Mode::Offline, std::memory_order_release);
            g_status = "Offline";
        }

        Logger::Write(g_status);
    }

    void Shutdown()
    {
        std::lock_guard<std::mutex> lock(g_mutex);

        if (g_socket != INVALID_SOCKET && g_localPlayerId)
        {
            Packet goodbye{};
            goodbye.Type = PacketType::Goodbye;
            goodbye.PlayerId = g_localPlayerId;

            if (g_mode.load(std::memory_order_acquire) == Mode::Host)
                BroadcastHostPacket(goodbye);
            else if (g_hasServer)
                SendTo(g_server, goodbye);
        }

        CloseSocket();
        ResetSessionState();
        g_mode.store(Mode::Offline, std::memory_order_release);

        if (g_winsockReady)
        {
            WSACleanup();
            g_winsockReady = false;
        }
    }

    void Update()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_socket == INVALID_SOCKET)
            return;

        // Bound receive work so a busy socket cannot starve heartbeats/game work.
        for (unsigned int budget = 0; budget < 256; ++budget)
        {
            Packet packet{};
            sockaddr_in from{};
            int fromSize = sizeof(from);

            const int received = recvfrom(
                g_socket,
                reinterpret_cast<char*>(&packet),
                sizeof(packet),
                0,
                reinterpret_cast<sockaddr*>(&from),
                &fromSize);

            if (received == SOCKET_ERROR)
            {
                const auto error = WSAGetLastError();
                if (error == WSAEMSGSIZE || error == WSAECONNRESET)
                {
                    ++g_dropped;
                    continue;
                }
                if (error != WSAEWOULDBLOCK)
                    g_status = "UDP receive error";
                break;
            }

            if (received != sizeof(packet) ||
                packet.MagicValue != AmalurNet::Magic ||
                packet.Version != AmalurNet::ProtocolVersion)
            {
                ++g_dropped;
                continue;
            }

            ++g_packetsReceived;
            g_bytesReceived += static_cast<std::uint64_t>(received);

            if (packet.Type == PacketType::DiscoveryResponse)
            {
                StoreDiscoveryResponse(packet, from);
                continue;
            }

            if (packet.Type == PacketType::DiscoveryRequest)
            {
                ReplyToDiscovery(packet, from);
                continue;
            }

            if (g_mode.load(std::memory_order_acquire) == Mode::Host)
                HandleHostPacket(packet, from);
            else if (g_mode.load(std::memory_order_acquire) == Mode::Client)
                HandleClientPacket(packet, from);
        }

        const std::uint64_t now = NowMs();
        const Mode mode = g_mode.load(std::memory_order_acquire);

        if (mode == Mode::Client)
        {
            SendHello(now);
            SendLocalTransform(now);
            SendLocalSnapshot(now);
            SendHeartbeat(now);

            if (g_localPlayerId && now - g_serverLastSeen > kTimeoutMs)
            {
                g_localPlayerId = 0;
                g_peers.clear();
                g_motionQueue.clear();
                g_visualOutgoing.clear();
                g_sessionId = 0;
                g_connectionId = NewConnectionId();
                g_hasRoster = false;
                g_rosterIds.clear();
                g_lastHello = 0;
                g_serverPingMs = 0;
                g_hasLocalSnapshot = false;

                if (g_autoDiscover)
                {
                    g_server.sin_addr.s_addr = htonl(INADDR_BROADCAST);
                    g_status = "Host lost; searching again";
                }
                else
                {
                    g_status = "Host connection timed out";
                }

                UpdateLobby();
            }
        }
        else if (mode == Mode::Host)
        {
            SendLocalTransform(now);
            SendLocalSnapshot(now);
            SendHeartbeat(now);
            CheckHostTimeouts(now);
            if (now - g_lastRoster >= kSnapshotIntervalMs)
            {
                for (auto& peer : g_peers)
                    SendInitialRosterTo(peer);
                g_lastRoster = now;
            }
        }

        PumpVisuals(now);
        PruneDiscoveredHosts(now);
    }

    bool Host(unsigned short port)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_winsockReady)
            return false;

        ResetSessionState();
        g_port = port;

        if (!OpenSocket(port))
        {
            OpenSocket(0);
            g_mode.store(Mode::Offline, std::memory_order_release);
            g_status = "Could not bind UDP port " + std::to_string(port);
            Logger::Write(Logger::Level::Error, g_status);
            return false;
        }

        g_sessionId = NewConnectionId();
        if (!g_sessionId)
        {
            g_mode.store(Mode::Offline, std::memory_order_release);
            g_status = "Could not create session identity";
            return false;
        }
        g_localPlayerId = 1;
        g_mode.store(Mode::Host, std::memory_order_release);
        g_status = "Hosting in-game on UDP " + std::to_string(port);
        UpdateLobby();
        Logger::Write(Logger::Level::Success, g_status);
        return true;
    }

    bool Join(const std::string& address, unsigned short port)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_winsockReady)
            return false;

        ResetSessionState();
        g_port = port;

        if (!OpenSocket(0))
        {
            g_status = "Could not open client UDP socket";
            g_mode.store(Mode::Offline, std::memory_order_release);
            return false;
        }

        if (!ResolveServer(address, port))
        {
            g_status = "Invalid host address";
            g_mode.store(Mode::Offline, std::memory_order_release);
            return false;
        }

        g_mode.store(Mode::Client, std::memory_order_release);
        g_status = g_autoDiscover
            ? "Searching for an AmalurCoop host"
            : "Connecting to " + address + ":" + std::to_string(port);
        Logger::Write(g_status);
        return true;
    }

    void Disconnect()
    {
        std::lock_guard<std::mutex> lock(g_mutex);

        if (g_socket != INVALID_SOCKET && g_localPlayerId)
        {
            Packet goodbye{};
            goodbye.Type = PacketType::Goodbye;
            goodbye.PlayerId = g_localPlayerId;

            if (g_mode.load(std::memory_order_acquire) == Mode::Host)
                BroadcastHostPacket(goodbye);
            else if (g_hasServer)
                SendTo(g_server, goodbye);
        }

        ResetSessionState();
        OpenSocket(0);
        g_mode.store(Mode::Offline, std::memory_order_release);
        g_status = "Disconnected";
        UpdateLobby();
    }

    Mode GetMode()
    {
        return g_mode.load(std::memory_order_acquire);
    }

    std::string GetStatusText()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        return g_status;
    }

    std::string GetModeText()
    {
        switch (g_mode.load(std::memory_order_acquire))
        {
        case Mode::Host:
            return "HOST";
        case Mode::Client:
            return "CLIENT";
        default:
            return "OFFLINE";
        }
    }

    Stats GetStats()
    {
        std::lock_guard<std::mutex> lock(g_mutex);

        Stats stats{};
        stats.PacketsSent = g_packetsSent;
        stats.PacketsReceived = g_packetsReceived;
        stats.BytesSent = g_bytesSent;
        stats.BytesReceived = g_bytesReceived;
        stats.DroppedPackets = g_dropped;
        stats.LocalPlayerId = g_localPlayerId;
        stats.BoundPort = g_port;

        if (g_mode.load(std::memory_order_acquire) == Mode::Client)
            stats.CurrentPingMs = g_serverPingMs;

        for (const auto& peer : g_peers)
        {
            if (!peer.Connected)
                continue;

            ++stats.ConnectedPeers;
            if (g_mode.load(std::memory_order_acquire) == Mode::Host &&
                peer.PingMs &&
                (!stats.CurrentPingMs || peer.PingMs < stats.CurrentPingMs))
            {
                stats.CurrentPingMs = peer.PingMs;
            }
        }

        return stats;
    }

    std::vector<PeerInfo> GetPeers()
    {
        std::lock_guard<std::mutex> lock(g_mutex);

        std::vector<PeerInfo> peers;
        peers.reserve(g_peers.size());

        for (const auto& peer : g_peers)
        {
            PeerInfo info{};
            info.PlayerId = peer.PlayerId;
            info.PingMs = peer.PingMs;
            info.LastSeenMs = peer.LastSeenMs;
            info.LastTransformMs = peer.LastTransformMs;
            info.Connected = peer.Connected;
            info.HasTransform = peer.HasTransform;
            info.HasSnapshot = peer.HasSnapshot;
            info.VisualRevision = peer.VisualRevision;
            info.Position = peer.Position;
            info.HasRotation = peer.HasRotation;
            std::memcpy(info.Rotation, peer.Rotation, sizeof(info.Rotation));
            info.Snapshot = peer.Snapshot;

            if (peer.HasAddress)
            {
                info.Address = AddressText(peer.Address);
                info.Port = ntohs(peer.Address.sin_port);
            }
            else if (g_mode.load(std::memory_order_acquire) == Mode::Client)
            {
                info.Address = "via host";
                info.Port = g_port;
            }

            peers.push_back(std::move(info));
        }

        return peers;
    }

    bool GetVisualState(std::uint32_t playerId, std::uint32_t& revision, std::vector<std::uint8_t>& bytes, std::uint32_t skipRevision)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        const auto* peer = FindPeer(playerId);
        if (!peer || !peer->Connected || !peer->VisualRevision || peer->VisualRevision == skipRevision) return false;
        revision = peer->VisualRevision; bytes = peer->VisualBytes; return true;
    }

    std::uint32_t GetLocalPlayerId()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        return g_localPlayerId;
    }

    void RequestServerDiscovery(unsigned short port)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_winsockReady)
            return;

        if (g_socket == INVALID_SOCKET && !OpenSocket(0))
            return;

        const std::uint64_t now = NowMs();
        PruneDiscoveredHosts(now);

        Packet request{};
        request.Type = PacketType::DiscoveryRequest;
        request.Sequence = ++g_sequence;
        request.TimestampMs = now;

        const auto targets = BuildDiscoveryTargets(port);
        for (const auto& target : targets)
            SendTo(target, request);
    }

    std::vector<DiscoveredHost> GetDiscoveredHosts()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        PruneDiscoveredHosts(NowMs());

        std::vector<DiscoveredHost> result;
        result.reserve(g_discoveredHosts.size());
        for (const auto& host : g_discoveredHosts)
        {
            DiscoveredHost out{};
            out.Address = AddressText(host.Address);
            out.Port = ntohs(host.Address.sin_port);
            out.Players = host.Players;
            out.MaxPlayers = host.MaxPlayers;
            out.PingMs = host.PingMs;
            out.LastSeenMs = host.LastSeenMs;
            result.push_back(std::move(out));
        }

        std::sort(result.begin(), result.end(),
            [](const DiscoveredHost& a, const DiscoveredHost& b)
            {
                if (a.PingMs != b.PingMs)
                    return a.PingMs < b.PingMs;
                if (a.Address != b.Address)
                    return a.Address < b.Address;
                return a.Port < b.Port;
            });
        return result;
    }

    bool SendMotionEvent(const MotionEventPacket& event)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_localPlayerId)
            return false;

        Packet packet{};
        packet.Type = PacketType::MotionEvent;
        packet.PlayerId = g_localPlayerId;
        packet.Sequence = ++g_sequence;
        packet.MotionHash = event.MotionHash;
        packet.RequestedVariant = event.RequestedVariant;
        packet.ParameterA = event.ParameterA;
        packet.ParameterB = event.ParameterB;
        packet.SenderTick = event.SenderTick;
        packet.PacketSequence = event.PacketSequence ? event.PacketSequence : packet.Sequence;
        packet.FlagA = event.FlagA;

        if (g_mode.load(std::memory_order_acquire) == Mode::Host)
        {
            BroadcastHostPacket(packet);
            return true;
        }

        return g_hasServer && SendTo(g_server, packet);
    }

    bool PopMotionEvent(MotionEventPacket& event)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_motionQueue.empty())
            return false;

        event = g_motionQueue.front();
        g_motionQueue.pop_front();
        return true;
    }

    bool HasRemoteTransform()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (const auto& peer : g_peers)
        {
            if (peer.Connected && peer.HasTransform)
                return true;
        }
        return false;
    }

    Vec3 GetRemoteTransform()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (const auto& peer : g_peers)
        {
            if (peer.Connected && peer.HasTransform)
                return peer.Position;
        }
        return {};
    }

    std::uint64_t GetRemoteTransformAgeMs()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        const std::uint64_t now = NowMs();
        for (const auto& peer : g_peers)
        {
            if (peer.Connected && peer.HasTransform)
                return now - peer.LastTransformMs;
        }
        return 0;
    }
}
