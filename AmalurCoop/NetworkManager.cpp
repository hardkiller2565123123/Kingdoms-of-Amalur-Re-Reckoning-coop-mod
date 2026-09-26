#include "NetworkManager.h"

#include "Config.h"
#include "LobbyManager.h"
#include "Logger.h"
#include "NetworkProtocol.h"
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

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#pragma comment(lib, "Ws2_32.lib")

namespace
{
    using AmalurNet::Packet;
    using AmalurNet::PacketType;

    constexpr std::uint64_t kTransformIntervalMs = 33;
    constexpr std::uint64_t kHelloIntervalMs = 1000;
    constexpr std::uint64_t kHeartbeatIntervalMs = 1000;
    constexpr std::uint64_t kTimeoutMs = 8000;

    struct PeerState
    {
        sockaddr_in Address{};
        bool HasAddress = false;
        std::uint32_t PlayerId = 0;
        std::uint32_t LastSequence = 0;
        std::uint32_t LastMotionSequence = 0;
        std::uint64_t LastSeenMs = 0;
        std::uint32_t PingMs = 0;
        bool Connected = false;
        bool HasTransform = false;
        NetworkManager::Vec3 Position{};
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
    std::deque<NetworkManager::MotionEventPacket> g_motionQueue;

    std::uint32_t g_localPlayerId = 0;
    std::uint32_t g_nextHostPlayerId = 2;
    std::uint32_t g_sequence = 0;
    std::uint32_t g_serverPingMs = 0;

    std::uint64_t g_lastHello = 0;
    std::uint64_t g_lastTransform = 0;
    std::uint64_t g_lastHeartbeat = 0;
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

    bool SameAddress(const sockaddr_in& a, const sockaddr_in& b)
    {
        return a.sin_addr.s_addr == b.sin_addr.s_addr &&
            a.sin_port == b.sin_port;
    }

    std::string AddressText(const sockaddr_in& address)
    {
        char ip[INET_ADDRSTRLEN]{};
        if (!inet_ntop(AF_INET, &address.sin_addr, ip, sizeof(ip)))
            return "unknown";
        return ip;
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
        g_serverPingMs = 0;
        g_lastHello = 0;
        g_lastTransform = 0;
        g_lastHeartbeat = 0;
        g_serverLastSeen = 0;
    }

    bool SendTo(const sockaddr_in& to, Packet packet)
    {
        if (g_socket == INVALID_SOCKET)
            return false;

        packet.MagicValue = AmalurNet::Magic;
        packet.Version = AmalurNet::ProtocolVersion;
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

        g_server.sin_addr =
            reinterpret_cast<sockaddr_in*>(result->ai_addr)->sin_addr;
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

    void QueueMotion(const Packet& packet, PeerState& peer)
    {
        const std::uint32_t sequence =
            packet.PacketSequence ? packet.PacketSequence : packet.Sequence;
        if (sequence <= peer.LastMotionSequence)
        {
            ++g_dropped;
            return;
        }

        peer.LastMotionSequence = sequence;

        NetworkManager::MotionEventPacket event{};
        event.ActorNetId = packet.PlayerId;
        event.MotionHash = packet.MotionHash;
        event.RequestedVariant = packet.RequestedVariant;
        event.ParameterA = packet.ParameterA;
        event.ParameterB = packet.ParameterB;
        event.SenderTick = packet.SenderTick;
        event.PacketSequence = sequence;
        event.FlagA = packet.FlagA;

        if (g_motionQueue.size() >= 128)
            g_motionQueue.pop_front();
        g_motionQueue.push_back(event);
    }

    void ApplyRemotePacket(const Packet& packet)
    {
        if (!packet.PlayerId || packet.PlayerId == g_localPlayerId)
            return;

        const std::uint64_t now = NowMs();
        auto& peer = GetOrAddPeer(packet.PlayerId);
        peer.Connected = true;
        peer.LastSeenMs = now;

        switch (packet.Type)
        {
        case PacketType::Transform:
            if (packet.Sequence <= peer.LastSequence ||
                !std::isfinite(packet.X) ||
                !std::isfinite(packet.Y) ||
                !std::isfinite(packet.Z))
            {
                ++g_dropped;
                return;
            }

            peer.LastSequence = packet.Sequence;
            peer.Position = { packet.X, packet.Y, packet.Z };
            peer.HasTransform = true;
            UpdateLobby();
            break;

        case PacketType::MotionEvent:
            QueueMotion(packet, peer);
            break;

        case PacketType::PlayerLeft:
        case PacketType::Goodbye:
            peer.Connected = false;
            UpdateLobby();
            break;

        default:
            break;
        }
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

        if (packet.Type == PacketType::Welcome)
        {
            g_server = from;
            g_hasServer = true;
            g_localPlayerId = packet.PlayerId;
            g_serverLastSeen = now;
            g_status = "Connected as player " + std::to_string(g_localPlayerId);
            Logger::Write(g_status);
            UpdateLobby();
            return;
        }

        if (!g_localPlayerId)
            return;

        g_serverLastSeen = now;

        if (packet.Type == PacketType::Ping)
        {
            Packet pong{};
            pong.Type = PacketType::Pong;
            pong.PlayerId = g_localPlayerId;
            pong.Sequence = packet.Sequence;
            pong.TimestampMs = packet.TimestampMs;
            SendTo(g_server, pong);
            return;
        }

        if (packet.Type == PacketType::Pong)
        {
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
            g_hasServer = false;
            g_status = "Host disconnected";
            UpdateLobby();
            return;
        }

        ApplyRemotePacket(packet);
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
        BroadcastHostPacket(left, playerId);

        Logger::WriteFormat(
            Logger::Level::Info,
            "Network peer %u disconnected",
            playerId);

        g_peers.erase(it);
        UpdateLobby();
    }

    void HandleHostPacket(Packet packet, const sockaddr_in& from)
    {
        const std::uint64_t now = NowMs();

        if (packet.Type == PacketType::Hello)
        {
            PeerState* peer = FindPeerByAddress(from);
            if (!peer)
            {
                if (g_peers.size() >= AmalurNet::MaxPlayers - 1)
                {
                    ++g_dropped;
                    return;
                }

                g_peers.push_back({});
                peer = &g_peers.back();
                peer->Address = from;
                peer->HasAddress = true;
                peer->PlayerId = g_nextHostPlayerId++;
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
            UpdateLobby();
            return;
        }

        PeerState* peer = FindPeerByAddress(from);
        if (!peer)
        {
            ++g_dropped;
            return;
        }

        peer->Connected = true;
        peer->LastSeenMs = now;
        packet.PlayerId = peer->PlayerId;

        if (packet.Type == PacketType::Goodbye)
        {
            const std::uint32_t playerId = peer->PlayerId;
            RemoveHostPeer(playerId);
            return;
        }

        if (packet.Type == PacketType::Ping)
        {
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
            if (packet.TimestampMs <= now)
            {
                peer->PingMs = static_cast<std::uint32_t>(
                    std::min<std::uint64_t>(9999, now - packet.TimestampMs));
            }
            return;
        }

        ApplyRemotePacket(packet);
        BroadcastHostPacket(packet, peer->PlayerId);
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

        if (g_mode.load(std::memory_order_acquire) == NetworkManager::Mode::Host)
            BroadcastHostPacket(packet);
        else if (g_hasServer)
            SendTo(g_server, packet);

        g_lastTransform = now;
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

        for (;;)
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
                if (WSAGetLastError() != WSAEWOULDBLOCK)
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
            SendHeartbeat(now);

            if (g_localPlayerId && now - g_serverLastSeen > kTimeoutMs)
            {
                g_localPlayerId = 0;
                g_peers.clear();
                g_serverPingMs = 0;

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
            SendHeartbeat(now);
            CheckHostTimeouts(now);
        }
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
            info.Connected = peer.Connected;
            info.HasTransform = peer.HasTransform;
            info.Position = peer.Position;

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

    std::uint32_t GetLocalPlayerId()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        return g_localPlayerId;
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
                return now - peer.LastSeenMs;
        }
        return 0;
    }
}
