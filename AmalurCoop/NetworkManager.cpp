#include "NetworkManager.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <Windows.h>

#if __has_include(<natupnp.h>)
#include <natupnp.h>
#define AMALUR_HAS_NATUPNP 1
#else
#define AMALUR_HAS_NATUPNP 0
#endif

#include "LobbyManager.h"
#include "Logger.h"
#include "PositionTracker.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "Ole32.lib")
#pragma comment(lib, "OleAut32.lib")

namespace
{
    constexpr std::uint32_t kMagic = 0x504F4341; // "ACOP"
    constexpr std::uint16_t kProtocolVersion = 1;
    constexpr std::uint64_t kTransformIntervalMs = 50;   // 20 Hz
    constexpr std::uint64_t kHeartbeatIntervalMs = 1000;
    constexpr std::uint64_t kConnectionTimeoutMs = 8000;
    constexpr std::size_t kMaxPeers = 15;

    enum class PacketType : std::uint8_t
    {
        Hello = 1,
        Welcome = 2,
        Transform = 3,
        Ping = 4,
        Pong = 5,
        Goodbye = 6
    };

#pragma pack(push, 1)
    struct Packet
    {
        std::uint32_t Magic = kMagic;
        std::uint16_t Version = kProtocolVersion;
        PacketType Type = PacketType::Hello;
        std::uint8_t Reserved = 0;
        std::uint32_t PlayerId = 0;
        std::uint32_t Sequence = 0;
        std::uint64_t TimestampMs = 0;
        float X = 0.0f;
        float Y = 0.0f;
        float Z = 0.0f;
    };
#pragma pack(pop)

    static_assert(sizeof(Packet) == 36, "Unexpected network packet size");

    struct PeerState
    {
        sockaddr_in Endpoint{};
        std::uint32_t PlayerId = 0;
        std::uint32_t LastReceivedSequence = 0;
        std::uint32_t PingMs = 0;
        std::uint64_t LastSeenMs = 0;
        std::uint64_t LastPingSentMs = 0;
        bool Connected = false;
        bool HasTransform = false;
        NetworkManager::Vec3 Position{};
    };

    std::atomic<NetworkManager::Mode> g_mode{ NetworkManager::Mode::Offline };
    SOCKET g_socket = INVALID_SOCKET;
    bool g_winsockReady = false;
    std::string g_status = "Offline";
    std::mutex g_mutex;

    sockaddr_in g_serverAddress{};
    bool g_hasServerAddress = false;
    std::vector<PeerState> g_peers;

    std::uint32_t g_localPlayerId = 0;
    std::uint32_t g_nextPlayerId = 2;
    std::uint32_t g_sendSequence = 0;
    std::uint64_t g_lastTransformSentMs = 0;
    std::uint64_t g_lastHeartbeatMs = 0;
    std::uint16_t g_boundPort = 0;

    std::uint64_t g_packetsSent = 0;
    std::uint64_t g_packetsReceived = 0;
    std::uint64_t g_bytesSent = 0;
    std::uint64_t g_bytesReceived = 0;
    std::uint64_t g_droppedPackets = 0;
    std::uint64_t g_lastTransformReceiveLogMs = 0;

    bool g_upnpMapped = false;
    std::string g_upnpStatus = "Not requested";
    unsigned short g_upnpPort = 0;
#if AMALUR_HAS_NATUPNP
    IStaticPortMapping* g_upnpMapping = nullptr;
#endif

    std::uint64_t NowMs()
    {
        return static_cast<std::uint64_t>(GetTickCount64());
    }

    bool IsFinitePosition(const Packet& packet)
    {
        return std::isfinite(packet.X) &&
               std::isfinite(packet.Y) &&
               std::isfinite(packet.Z) &&
               std::fabs(packet.X) < 100000000.0f &&
               std::fabs(packet.Y) < 100000000.0f &&
               std::fabs(packet.Z) < 100000000.0f;
    }

    bool EndpointEquals(const sockaddr_in& a, const sockaddr_in& b)
    {
        return a.sin_family == b.sin_family &&
               a.sin_port == b.sin_port &&
               a.sin_addr.s_addr == b.sin_addr.s_addr;
    }

    std::string EndpointAddress(const sockaddr_in& endpoint)
    {
        char ip[INET_ADDRSTRLEN]{};
        inet_ntop(AF_INET, &endpoint.sin_addr, ip, static_cast<socklen_t>(sizeof(ip)));
        return ip;
    }

    void CloseSocketLocked()
    {
        if (g_socket != INVALID_SOCKET)
        {
            closesocket(g_socket);
            g_socket = INVALID_SOCKET;
        }
    }

    void ResetSessionLocked()
    {
        CloseSocketLocked();
        g_mode.store(NetworkManager::Mode::Offline);
        g_status = "Offline";
        g_hasServerAddress = false;
        g_serverAddress = {};
        g_peers.clear();
        g_localPlayerId = 0;
        g_nextPlayerId = 2;
        g_sendSequence = 0;
        g_lastTransformSentMs = 0;
        g_lastHeartbeatMs = 0;
        g_boundPort = 0;
        LobbyManager::SetPlayerCount(1);
    }

    bool SendPacketLocked(const sockaddr_in& endpoint, Packet packet)
    {
        if (g_socket == INVALID_SOCKET)
            return false;

        packet.Magic = kMagic;
        packet.Version = kProtocolVersion;
        if (packet.TimestampMs == 0)
            packet.TimestampMs = NowMs();

        const int sent = sendto(
            g_socket,
            reinterpret_cast<const char*>(&packet),
            static_cast<int>(sizeof(packet)),
            0,
            reinterpret_cast<const sockaddr*>(&endpoint),
            sizeof(endpoint));

        if (sent != static_cast<int>(sizeof(packet)))
        {
            ++g_droppedPackets;
            return false;
        }

        ++g_packetsSent;
        g_bytesSent += static_cast<std::uint64_t>(sent);
        return true;
    }

    PeerState* FindPeerByEndpointLocked(const sockaddr_in& endpoint)
    {
        for (auto& peer : g_peers)
        {
            if (EndpointEquals(peer.Endpoint, endpoint))
                return &peer;
        }
        return nullptr;
    }

    PeerState* FindPeerByIdLocked(std::uint32_t playerId)
    {
        for (auto& peer : g_peers)
        {
            if (peer.PlayerId == playerId)
                return &peer;
        }
        return nullptr;
    }

    void UpdateLobbyCountLocked()
    {
        std::size_t connected = 0;
        for (const auto& peer : g_peers)
        {
            if (peer.Connected)
                ++connected;
        }

        LobbyManager::SetPlayerCount(
            static_cast<int>(std::min<std::size_t>(
                connected + 1,
                static_cast<std::size_t>(LobbyManager::GetMaxPlayers()))));
    }

    PeerState* AddHostPeerLocked(const sockaddr_in& endpoint)
    {
        if (PeerState* existing = FindPeerByEndpointLocked(endpoint))
            return existing;

        if (g_peers.size() >= kMaxPeers)
            return nullptr;

        PeerState peer{};
        peer.Endpoint = endpoint;
        peer.PlayerId = g_nextPlayerId++;
        peer.LastSeenMs = NowMs();
        peer.Connected = true;
        g_peers.push_back(peer);
        return &g_peers.back();
    }

    void BroadcastLocked(Packet packet, const sockaddr_in* exceptEndpoint = nullptr)
    {
        for (const auto& peer : g_peers)
        {
            if (!peer.Connected)
                continue;
            if (exceptEndpoint && EndpointEquals(peer.Endpoint, *exceptEndpoint))
                continue;
            SendPacketLocked(peer.Endpoint, packet);
        }
    }

    void SendLocalTransformLocked(std::uint64_t now)
    {
        if (now - g_lastTransformSentMs < kTransformIntervalMs)
            return;
        if (!PositionTracker::HasPosition())
            return;

        const PositionTracker::Vec3 position = PositionTracker::GetPosition();
        Packet packet{};
        packet.Type = PacketType::Transform;
        packet.PlayerId = g_localPlayerId;
        packet.Sequence = ++g_sendSequence;
        packet.X = position.X;
        packet.Y = position.Y;
        packet.Z = position.Z;

        const NetworkManager::Mode mode = g_mode.load();
        if (mode == NetworkManager::Mode::Host)
            BroadcastLocked(packet);
        else if (mode == NetworkManager::Mode::Client && g_hasServerAddress && g_localPlayerId != 0)
            SendPacketLocked(g_serverAddress, packet);

        g_lastTransformSentMs = now;
    }

    void SendHeartbeatsLocked(std::uint64_t now)
    {
        if (now - g_lastHeartbeatMs < kHeartbeatIntervalMs)
            return;

        Packet ping{};
        ping.Type = PacketType::Ping;
        ping.PlayerId = g_localPlayerId;
        ping.Sequence = ++g_sendSequence;

        const NetworkManager::Mode mode = g_mode.load();
        if (mode == NetworkManager::Mode::Host)
        {
            for (auto& peer : g_peers)
            {
                if (!peer.Connected)
                    continue;
                peer.LastPingSentMs = now;
                SendPacketLocked(peer.Endpoint, ping);
            }
        }
        else if (mode == NetworkManager::Mode::Client && g_hasServerAddress)
        {
            if (!g_peers.empty())
                g_peers.front().LastPingSentMs = now;
            SendPacketLocked(g_serverAddress, ping);
        }

        g_lastHeartbeatMs = now;
    }

    void RemoveTimedOutPeersLocked(std::uint64_t now)
    {
        bool changed = false;
        for (auto& peer : g_peers)
        {
            if (peer.Connected && now - peer.LastSeenMs > kConnectionTimeoutMs)
            {
                peer.Connected = false;
                changed = true;
                Logger::Write("Network peer timed out");
            }
        }

        if (changed)
        {
            UpdateLobbyCountLocked();
            if (g_mode.load() == NetworkManager::Mode::Client)
                g_status = "Connection timed out";
        }
    }

    void HandlePacketLocked(const Packet& packet, const sockaddr_in& from)
    {
        const std::uint64_t now = NowMs();
        const NetworkManager::Mode mode = g_mode.load();

        if (mode == NetworkManager::Mode::Host && packet.Type == PacketType::Hello)
        {
            PeerState* peer = AddHostPeerLocked(from);
            if (!peer)
            {
                g_status = "Host full";
                return;
            }

            peer->Connected = true;
            peer->LastSeenMs = now;

            Packet welcome{};
            welcome.Type = PacketType::Welcome;
            welcome.PlayerId = peer->PlayerId;
            welcome.Sequence = ++g_sendSequence;
            SendPacketLocked(from, welcome);

            g_status = "Hosting: " + std::to_string(g_peers.size()) + " remote peer(s)";
            UpdateLobbyCountLocked();
            Logger::Write("Client joined from " + EndpointAddress(from));
            return;
        }

        if (mode == NetworkManager::Mode::Client && packet.Type == PacketType::Welcome)
        {
            if (!EndpointEquals(from, g_serverAddress))
                return;

            g_localPlayerId = packet.PlayerId;
            if (g_peers.empty())
            {
                PeerState host{};
                host.Endpoint = from;
                host.PlayerId = 1;
                host.Connected = true;
                host.LastSeenMs = now;
                g_peers.push_back(host);
            }
            else
            {
                g_peers.front().Connected = true;
                g_peers.front().LastSeenMs = now;
            }

            g_status = "Connected to host as player " + std::to_string(g_localPlayerId);
            LobbyManager::SetPlayerCount(2);
            Logger::Write(g_status);
            return;
        }

        PeerState* peer = FindPeerByEndpointLocked(from);
        if (!peer && mode == NetworkManager::Mode::Client && EndpointEquals(from, g_serverAddress))
        {
            PeerState host{};
            host.Endpoint = from;
            host.PlayerId = 1;
            host.Connected = true;
            host.LastSeenMs = now;
            g_peers.push_back(host);
            peer = &g_peers.back();
        }

        if (!peer)
            return;

        peer->Connected = true;
        peer->LastSeenMs = now;

        switch (packet.Type)
        {
        case PacketType::Transform:
            if (!IsFinitePosition(packet) || packet.Sequence <= peer->LastReceivedSequence)
            {
                ++g_droppedPackets;
                return;
            }
            peer->LastReceivedSequence = packet.Sequence;
            peer->PlayerId = packet.PlayerId ? packet.PlayerId : peer->PlayerId;
            peer->Position = { packet.X, packet.Y, packet.Z };
            peer->HasTransform = true;

            if (now - g_lastTransformReceiveLogMs >= 2000)
            {
                g_lastTransformReceiveLogMs = now;
                Logger::WriteFormat(
                    Logger::Level::Debug,
                    "NET received transform player=%u seq=%u XYZ=(%.3f, %.3f, %.3f)",
                    peer->PlayerId, packet.Sequence, packet.X, packet.Y, packet.Z);
            }

            // Host relays one client's transform to all other clients.
            if (mode == NetworkManager::Mode::Host)
                BroadcastLocked(packet, &from);
            break;

        case PacketType::Ping:
        {
            Packet pong{};
            pong.Type = PacketType::Pong;
            pong.PlayerId = g_localPlayerId;
            pong.Sequence = packet.Sequence;
            pong.TimestampMs = packet.TimestampMs;
            SendPacketLocked(from, pong);
            break;
        }

        case PacketType::Pong:
            if (packet.TimestampMs <= now)
                peer->PingMs = static_cast<std::uint32_t>(std::min<std::uint64_t>(now - packet.TimestampMs, 9999));
            break;

        case PacketType::Goodbye:
            peer->Connected = false;
            UpdateLobbyCountLocked();
            break;

        default:
            break;
        }
    }

    std::string GetLocalIpv4()
    {
        char hostName[256]{};
        if (gethostname(hostName, static_cast<int>(sizeof(hostName))) == SOCKET_ERROR)
            return "127.0.0.1";

        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;

        addrinfo* results = nullptr;
        if (getaddrinfo(hostName, nullptr, &hints, &results) != 0)
            return "127.0.0.1";

        std::string selected = "127.0.0.1";
        for (addrinfo* item = results; item; item = item->ai_next)
        {
            const auto* address = reinterpret_cast<const sockaddr_in*>(item->ai_addr);
            const std::string candidate = EndpointAddress(*address);
            if (candidate.rfind("127.", 0) != 0)
            {
                selected = candidate;
                break;
            }
        }
        freeaddrinfo(results);
        return selected;
    }
}

namespace NetworkManager
{
    void Initialize()
    {
        std::lock_guard<std::mutex> lock(g_mutex);

        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) == 0)
        {
            g_winsockReady = true;
            ResetSessionLocked();
            Logger::Write("NetworkManager initialized (UDP protocol v1)");
        }
        else
        {
            g_winsockReady = false;
            g_status = "Winsock initialization failed";
            Logger::Write(g_status);
        }
    }

    void Shutdown()
    {
        std::lock_guard<std::mutex> lock(g_mutex);

        if (g_socket != INVALID_SOCKET)
        {
            Packet goodbye{};
            goodbye.Type = PacketType::Goodbye;
            goodbye.PlayerId = g_localPlayerId;
            if (g_mode.load() == Mode::Host)
                BroadcastLocked(goodbye);
            else if (g_mode.load() == Mode::Client && g_hasServerAddress)
                SendPacketLocked(g_serverAddress, goodbye);
        }

        RemoveUpnpMapping();
        ResetSessionLocked();

        if (g_winsockReady)
        {
            WSACleanup();
            g_winsockReady = false;
        }

        Logger::Write("NetworkManager shutdown");
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
                static_cast<int>(sizeof(packet)),
                0,
                reinterpret_cast<sockaddr*>(&from),
                &fromSize);

            if (received == SOCKET_ERROR)
            {
                const int error = WSAGetLastError();
                if (error != WSAEWOULDBLOCK)
                    g_status = "Receive error: " + std::to_string(error);
                break;
            }

            if (received == 0)
                break;

            ++g_packetsReceived;
            g_bytesReceived += static_cast<std::uint64_t>(received);

            if (received != static_cast<int>(sizeof(Packet)) ||
                packet.Magic != kMagic ||
                packet.Version != kProtocolVersion)
            {
                ++g_droppedPackets;
                continue;
            }

            HandlePacketLocked(packet, from);
        }

        const std::uint64_t now = NowMs();
        SendLocalTransformLocked(now);
        SendHeartbeatsLocked(now);
        RemoveTimedOutPeersLocked(now);
    }

    bool Host(unsigned short port)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_winsockReady)
            return false;

        ResetSessionLocked();
        g_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (g_socket == INVALID_SOCKET)
        {
            g_status = "Host failed: socket creation";
            return false;
        }

        BOOL reuse = TRUE;
        setsockopt(g_socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_ANY);
        address.sin_port = htons(port);

        if (bind(g_socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR)
        {
            const int error = WSAGetLastError();
            CloseSocketLocked();
            g_status = "Host failed: bind error " + std::to_string(error);
            Logger::Write(g_status);
            return false;
        }

        u_long nonBlocking = 1;
        ioctlsocket(g_socket, FIONBIO, &nonBlocking);

        g_mode.store(Mode::Host);
        g_localPlayerId = 1;
        g_boundPort = port;
        g_status = "Hosting on " + GetLocalIpv4() + ":" + std::to_string(port);
        LobbyManager::SetPlayerCount(1);
        Logger::Write(g_status);
        return true;
    }

    bool Join(const std::string& ip, unsigned short port)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_winsockReady)
            return false;

        ResetSessionLocked();
        g_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (g_socket == INVALID_SOCKET)
        {
            g_status = "Join failed: socket creation";
            return false;
        }

        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = htonl(INADDR_ANY);
        local.sin_port = htons(0); // ephemeral port allows two instances on one PC
        if (bind(g_socket, reinterpret_cast<sockaddr*>(&local), sizeof(local)) == SOCKET_ERROR)
        {
            CloseSocketLocked();
            g_status = "Join failed: local bind";
            return false;
        }

        u_long nonBlocking = 1;
        ioctlsocket(g_socket, FIONBIO, &nonBlocking);

        g_serverAddress = {};
        g_serverAddress.sin_family = AF_INET;
        g_serverAddress.sin_port = htons(port);

        if (inet_pton(AF_INET, ip.c_str(), &g_serverAddress.sin_addr) != 1)
        {
            addrinfo hints{};
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_DGRAM;
            addrinfo* results = nullptr;
            if (getaddrinfo(ip.c_str(), nullptr, &hints, &results) != 0 || !results)
            {
                CloseSocketLocked();
                g_status = "Join failed: invalid address";
                return false;
            }
            g_serverAddress.sin_addr = reinterpret_cast<sockaddr_in*>(results->ai_addr)->sin_addr;
            freeaddrinfo(results);
        }

        g_hasServerAddress = true;
        g_mode.store(Mode::Client);
        g_boundPort = port;

        Packet hello{};
        hello.Type = PacketType::Hello;
        hello.Sequence = ++g_sendSequence;
        SendPacketLocked(g_serverAddress, hello);

        g_status = "Connecting to " + ip + ":" + std::to_string(port);
        Logger::Write(g_status);
        return true;
    }

    void Disconnect()
    {
        std::lock_guard<std::mutex> lock(g_mutex);

        if (g_socket != INVALID_SOCKET)
        {
            Packet goodbye{};
            goodbye.Type = PacketType::Goodbye;
            goodbye.PlayerId = g_localPlayerId;
            if (g_mode.load() == Mode::Host)
                BroadcastLocked(goodbye);
            else if (g_mode.load() == Mode::Client && g_hasServerAddress)
                SendPacketLocked(g_serverAddress, goodbye);
        }

        RemoveUpnpMapping();
        ResetSessionLocked();
        Logger::Write("Network disconnected");
    }

    bool TryMapPortUpnp(unsigned short port)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
#if AMALUR_HAS_NATUPNP
        RemoveUpnpMapping();

        const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool shouldUninitialize = SUCCEEDED(init);

        IUPnPNAT* nat = nullptr;
        IStaticPortMappingCollection* mappings = nullptr;
        IStaticPortMapping* mapping = nullptr;

        HRESULT hr = CoCreateInstance(
            __uuidof(UPnPNAT),
            nullptr,
            CLSCTX_INPROC_SERVER,
            __uuidof(IUPnPNAT),
            reinterpret_cast<void**>(&nat));

        if (SUCCEEDED(hr) && nat)
            hr = nat->get_StaticPortMappingCollection(&mappings);

        const std::string localIp = GetLocalIpv4();
        BSTR protocol = SysAllocString(L"UDP");
        wchar_t localIpWide[64]{};
        MultiByteToWideChar(CP_UTF8, 0, localIp.c_str(), -1, localIpWide, static_cast<int>(_countof(localIpWide)));
        BSTR client = SysAllocString(localIpWide);
        BSTR description = SysAllocString(L"AmalurCoop UDP Host");

        if (SUCCEEDED(hr) && mappings)
        {
            hr = mappings->Add(
                port,
                protocol,
                port,
                client,
                VARIANT_TRUE,
                description,
                &mapping);
        }

        if (protocol) SysFreeString(protocol);
        if (client) SysFreeString(client);
        if (description) SysFreeString(description);
        if (mappings) mappings->Release();
        if (nat) nat->Release();
        if (shouldUninitialize) CoUninitialize();

        if (SUCCEEDED(hr) && mapping)
        {
            g_upnpMapping = mapping;
            g_upnpMapped = true;
            g_upnpPort = port;
            g_upnpStatus = "UPnP mapped UDP " + std::to_string(port);
            Logger::Write(g_upnpStatus);
            return true;
        }

        if (mapping) mapping->Release();
        g_upnpMapped = false;
        g_upnpStatus = "UPnP unavailable; forward UDP " + std::to_string(port) + " manually";
        Logger::Write(g_upnpStatus);
        return false;
#else
        (void)port;
        g_upnpMapped = false;
        g_upnpStatus = "UPnP SDK header unavailable";
        return false;
#endif
    }

    void RemoveUpnpMapping()
    {
#if AMALUR_HAS_NATUPNP
        if (g_upnpMapping)
        {
            g_upnpMapping->Release();
            g_upnpMapping = nullptr;
        }
#endif
        g_upnpMapped = false;
        g_upnpPort = 0;
    }

    Mode GetMode()
    {
        return g_mode.load();
    }

    std::string GetStatusText()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        return g_status;
    }

    std::string GetModeText()
    {
        switch (g_mode.load())
        {
        case Mode::Host: return "HOST";
        case Mode::Client: return "CLIENT";
        default: return "OFFLINE";
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
        stats.DroppedPackets = g_droppedPackets;
        stats.LocalPlayerId = g_localPlayerId;
        stats.BoundPort = g_boundPort;
        stats.UpnpMapped = g_upnpMapped;
        stats.UpnpStatus = g_upnpStatus;

        for (const auto& peer : g_peers)
        {
            if (peer.Connected)
            {
                ++stats.ConnectedPeers;
                if (stats.CurrentPingMs == 0 || peer.PingMs < stats.CurrentPingMs)
                    stats.CurrentPingMs = peer.PingMs;
            }
        }
        return stats;
    }

    std::vector<PeerInfo> GetPeers()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        std::vector<PeerInfo> result;
        result.reserve(g_peers.size());
        for (const auto& peer : g_peers)
        {
            PeerInfo info{};
            info.PlayerId = peer.PlayerId;
            info.Address = EndpointAddress(peer.Endpoint);
            info.Port = ntohs(peer.Endpoint.sin_port);
            info.PingMs = peer.PingMs;
            info.LastSeenMs = peer.LastSeenMs;
            info.Connected = peer.Connected;
            info.HasTransform = peer.HasTransform;
            info.Position = peer.Position;
            result.push_back(info);
        }
        return result;
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
                return now >= peer.LastSeenMs ? now - peer.LastSeenMs : 0;
        }
        return 0;
    }
}
