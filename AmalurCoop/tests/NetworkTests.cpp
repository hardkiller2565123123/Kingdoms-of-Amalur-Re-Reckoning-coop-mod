// Exercise the actual UDP implementation; only game services are replaced.
#include "../NetworkManager.cpp"
#include <cassert>
#include <cstdio>
#include <limits>

namespace Config { Settings Get() { Settings s; s.AutoConnect = false; return s; } }
namespace LobbyManager
{
    int count = 1, max = 4;
    int GetPlayerCount() { return count; }
    int GetMaxPlayers() { return max; }
    void SetPlayerCount(int n) { count = n; }
}
namespace Logger
{
    void Write(const std::string&) {}
    void Write(const char*) {}
    void Write(Level, const std::string&) {}
    void Write(Level, const char*) {}
    void WriteFormat(Level, const char*, ...) {}
}
namespace PositionTracker
{
    uintptr_t GetRuntimeObject() { return 0; }
    uintptr_t GetTransformComponent() { return 0; }
    std::uint32_t GetDefinitionHandle() { return 0; }
    std::uint32_t GetPlayerSetupValue() { return 0; }
    bool HasPosition() { return false; }
    Vec3 GetPosition() { return {}; }
}
namespace EngineRuntime
{
    bool IsActiveObject(uintptr_t) { return false; }
    uintptr_t GetComponent(uintptr_t, unsigned int, bool) { return 0; }
    bool ReadBytes(uintptr_t, void*, std::size_t) { return false; }
}
namespace CharacterVisualRuntime { bool Capture(std::vector<std::uint8_t>&) { return false; } }

std::vector<std::uint8_t> MakeArchive(bool motion, unsigned children = 0)
{
    using namespace CharacterVisualArchive;
    std::vector<std::uint8_t> out;
    const auto root = Begin(out, CharacterVisualRuntime::RenderingTag, 0);
    out.push_back(0); Write32(out, 0);
    const auto reference = Begin(out, ActorReferenceTag, 0);
    out.push_back(1); Write32(out, 0x12345678);
    const auto actor = Begin(out, ActorTag, 3);
    const auto matrices = out.size();
    out.resize(out.size() + 114);
    const float one = 1;
    std::memcpy(out.data() + matrices + 12, &one, 4);
    std::memcpy(out.data() + matrices + 57, &one, 4);
    if (motion) { const auto m = Begin(out, MotionTag, 1); out.resize(out.size() + 24); End(out, m); }
    Write32(out, children);
    for (unsigned i = 0; i < children; ++i)
    {
        const auto ref = Begin(out, ActorReferenceTag, 0); out.push_back(0); End(out, ref);
    }
    End(out, actor); End(out, reference); End(out, root); return out;
}
sockaddr_in Endpoint(unsigned short port)
{
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(port); return a;
}
Packet Hello(std::uint64_t nonce)
{
    Packet p{}; p.Type = PacketType::Hello; p.ConnectionId = nonce; return p;
}
Packet ClientPacket(PacketType type, const PeerState& peer, std::uint32_t sequence)
{
    Packet p{}; p.Type = type; p.PlayerId = peer.PlayerId; p.SessionId = g_sessionId;
    p.ConnectionId = peer.ConnectionId; p.Sequence = sequence; return p;
}

int main()
{
    using namespace CharacterVisualArchive;
    std::vector<std::uint8_t> normalized;
    auto archive = MakeArchive(true, 2);
    assert(Normalize(archive, CharacterVisualRuntime::RenderingTag, true, normalized));
    assert(normalized == MakeArchive(false, 2));
    std::vector<std::uint8_t> checked;
    assert(Normalize(normalized, CharacterVisualRuntime::RenderingTag, false, checked));
    assert(!Normalize(archive, CharacterVisualRuntime::RenderingTag, false, checked));
    for (std::size_t n = 0; n < normalized.size(); ++n)
    {
        auto shortArchive = normalized; shortArchive.resize(n);
        assert(!Normalize(shortArchive, CharacterVisualRuntime::RenderingTag, false, checked));
    }
    auto bad = normalized; bad[5] = 0xFF; bad[6] = 0xFF;
    assert(!Normalize(bad, CharacterVisualRuntime::RenderingTag, false, checked));
    assert(!Normalize(MakeArchive(false, 33), CharacterVisualRuntime::RenderingTag, false, checked));
    auto withMaterials = normalized;
    Write32(withMaterials, MaterialTag);
    for (unsigned i = 0; i < 13; ++i) Write32(withMaterials, i * 0x01020304u);
    assert(Normalize(withMaterials, CharacterVisualRuntime::RenderingTag, false, checked));
    assert(checked == withMaterials);
    auto brokenMaterials = withMaterials; brokenMaterials.back() = 255;
    brokenMaterials.resize(brokenMaterials.size() - 1);
    assert(!Normalize(brokenMaterials, CharacterVisualRuntime::RenderingTag, false, checked));
    brokenMaterials = withMaterials; brokenMaterials[normalized.size()] = 0;
    assert(!Normalize(brokenMaterials, CharacterVisualRuntime::RenderingTag, false, checked));
    normalized = withMaterials;
    assert(AmalurNet::IsNewer(0, UINT32_MAX));
    assert(!AmalurNet::IsNewer(UINT32_MAX, 0));
    assert(!AmalurNet::IsNewer(1, 1));

    AmalurNet::VisualTransfer transfer;
    Packet chunk{}; chunk.VisualRevision = 1; chunk.VisualBytes = 1500;
    chunk.VisualCount = 2; chunk.VisualIndex = 1; chunk.VisualData[0] = 23;
    assert(transfer.Accept(chunk) && !transfer.Complete());
    assert(transfer.Accept(chunk));
    chunk.VisualData[0] = 24; assert(!transfer.Accept(chunk));
    chunk.VisualIndex = 0; assert(transfer.Accept(chunk) && transfer.Complete());
    chunk.VisualRevision = 2; chunk.VisualBytes = 65537; assert(!transfer.Accept(chunk));
    chunk.VisualBytes = 1500; chunk.VisualCount = 3; assert(!transfer.Accept(chunk));

    NetworkManager::Initialize();
    assert(NetworkManager::Host(0));
    const auto a = Endpoint(42001), b = Endpoint(42002), c = Endpoint(42003), d = Endpoint(42004);
    HandleHostPacket(Hello(101), a); HandleHostPacket(Hello(102), b); HandleHostPacket(Hello(103), c);
    assert(g_peers.size() == 3 && LobbyManager::count == 4);
    HandleHostPacket(Hello(104), d); assert(g_peers.size() == 3);
    auto p = ClientPacket(PacketType::Transform, g_peers[0], UINT32_MAX);
    p.X = 10; HandleHostPacket(p, a); assert(g_peers[0].Position.X == 10);
    p.Sequence = 0; p.X = 20; HandleHostPacket(p, a); assert(g_peers[0].Position.X == 20);
    p.Sequence = UINT32_MAX; p.X = 30; HandleHostPacket(p, a); assert(g_peers[0].Position.X == 20);
    p.Sequence = 1; p.X = std::numeric_limits<float>::infinity();
    HandleHostPacket(p, a); assert(g_peers[0].Position.X == 20);
    p.X = 30; ++p.ConnectionId; HandleHostPacket(p, a); assert(g_peers[0].Position.X == 20);
    --p.ConnectionId; ++p.SessionId; HandleHostPacket(p, a); assert(g_peers[0].Position.X == 20);
    --p.SessionId; p.PlayerId = 3; HandleHostPacket(p, a); assert(g_peers[0].Position.X == 20);
    auto motion = ClientPacket(PacketType::MotionEvent, g_peers[0], 1);
    motion.MotionHash = 123; motion.RequestedVariant = -1; motion.PacketSequence = 77;
    const float animationSpeed = 1.25f;
    std::memcpy(&motion.ParameterA, &animationSpeed, 4);
    AmalurNet::AnimationRequest animation{};
    animation.RigKey = 0x123456; animation.ResourceKeyA = 0xABC;
    animation.StartOffset = 150; animation.BlendEnabled = 1; animation.Blend[2] = 0.5f;
    std::memcpy(motion.VisualData, &animation, sizeof(animation));
    assert(AmalurNet::ValidAnimation(motion));
    auto trackAnimation = animation;
    trackAnimation.SelectedVariants = 1; trackAnimation.Track = 7;
    trackAnimation.CatalogKey = 0x321; trackAnimation.Variants[0] = 2;
    auto trackPacket = motion;
    std::memcpy(trackPacket.VisualData, &trackAnimation, sizeof(trackAnimation));
    assert(AmalurNet::ValidAnimation(trackPacket));
    trackAnimation.Track = 8;
    std::memcpy(trackPacket.VisualData, &trackAnimation, sizeof(trackAnimation));
    assert(!AmalurNet::ValidAnimation(trackPacket));
    trackAnimation.Track = 0; trackAnimation.CatalogKey = 0;
    std::memcpy(trackPacket.VisualData, &trackAnimation, sizeof(trackAnimation));
    assert(!AmalurNet::ValidAnimation(trackPacket));
    trackAnimation.CatalogKey = 0x321; trackAnimation.Weights[0] = std::numeric_limits<float>::quiet_NaN();
    std::memcpy(trackPacket.VisualData, &trackAnimation, sizeof(trackAnimation));
    assert(!AmalurNet::ValidAnimation(trackPacket));
    NetworkManager::MotionEventPacket outbound{};
    outbound.MotionHash = motion.MotionHash; outbound.ParameterA = motion.ParameterA;
    outbound.Animation = animation; outbound.PacketSequence = 90;
    const auto sequenceBeforeEnqueue = g_sequence;
    assert(NetworkManager::SendMotionEvent(outbound));
    assert(g_sequence == sequenceBeforeEnqueue); // render-side enqueue performs no UDP send
    assert(g_motionOutgoing.size() == 1 && g_motionOutgoing.front().PacketSequence == 90);

    auto malformedMotion = motion;
    const float nan = std::numeric_limits<float>::quiet_NaN();
    std::memcpy(&malformedMotion.ParameterA, &nan, 4);
    HandleHostPacket(malformedMotion, a); assert(g_motionQueue.empty());
    auto invalidAnimation = animation; invalidAnimation.BlendEnabled = 2;
    std::memcpy(malformedMotion.VisualData, &invalidAnimation, sizeof(invalidAnimation));
    malformedMotion.ParameterA = motion.ParameterA;
    HandleHostPacket(malformedMotion, a); assert(g_motionQueue.empty());
    HandleHostPacket(motion, a); HandleHostPacket(motion, a);
    assert(g_motionQueue.size() == 1);
    assert(g_motionQueue.front().PacketSequence == 77);
    assert(g_motionQueue.front().Animation.RigKey == animation.RigKey);
    assert(g_motionQueue.front().Animation.ResourceKeyA == animation.ResourceKeyA);
    assert(g_motionQueue.front().Animation.StartOffset == 150);
    assert(g_motionQueue.front().Animation.Blend[2] == 0.5f);
    ++motion.Sequence; HandleHostPacket(motion, a);
    assert(g_motionQueue.size() == 2 && g_motionQueue.back().PacketSequence == 77);
    // A heartbeat has a fresh transport sequence but must not restart the clip.
    auto visual = ClientPacket(PacketType::VisualChunk, g_peers[0], 2);
    visual.VisualRevision = 1; visual.VisualBytes = static_cast<std::uint32_t>(normalized.size());
    visual.VisualCount = 1; std::memcpy(visual.VisualData, normalized.data(), normalized.size());
    HandleHostPacket(visual, a);
    assert(g_peers[0].VisualRevision == 1 && g_peers[0].VisualBytes == normalized);
    RemoveHostPeer(2); assert(g_motionQueue.empty());

    assert(NetworkManager::Join("127.0.0.1", 42001));
    assert(g_motionOutgoing.empty()); // no queued animation leaks across sessions
    Packet welcome{}; welcome.Type = PacketType::Welcome; welcome.PlayerId = 2;
    welcome.SessionId = 55; welcome.ConnectionId = g_connectionId;
    HandleClientPacket(welcome, b); assert(g_localPlayerId == 0);
    HandleClientPacket(welcome, a); assert(g_localPlayerId == 2);
    auto forged = welcome; forged.PlayerId = 3;
    HandleClientPacket(forged, a); assert(g_localPlayerId == 2);
    Packet roster{}; roster.Type = PacketType::Roster; roster.PlayerId = 1;
    roster.SessionId = 55; roster.ConnectionId = g_connectionId; roster.Sequence = 10;
    roster.RosterIds[0] = 1; roster.RosterIds[1] = 2; roster.RosterIds[2] = 3;
    HandleClientPacket(roster, a); assert(FindPeer(3));
    roster.Sequence = 11; roster.RosterIds[2] = 0;
    HandleClientPacket(roster, a); assert(!FindPeer(3));
    roster.Sequence = 10; roster.RosterIds[2] = 3;
    HandleClientPacket(roster, a); assert(!FindPeer(3));
    p.Type = PacketType::Transform; p.PlayerId = 3; p.SessionId = 55; p.ConnectionId = g_connectionId;
    HandleClientPacket(p, a); assert(!FindPeer(3));

    // Real loopback UDP: host receives a hello and emits a matching welcome.
    NetworkManager::Disconnect(); assert(NetworkManager::Host(0));
    sockaddr_in bound{}; int size = sizeof(bound);
    getsockname(g_socket, reinterpret_cast<sockaddr*>(&bound), &size);
    bound.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const SOCKET client = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    DWORD timeout = 1000; setsockopt(client, SOL_SOCKET, SO_RCVTIMEO,
        reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    auto hello = Hello(456);
    assert(sendto(client, reinterpret_cast<const char*>(&hello), sizeof(hello), 0,
        reinterpret_cast<const sockaddr*>(&bound), sizeof(bound)) == sizeof(hello));
    NetworkManager::Update();
    Packet reply{};
    assert(recv(client, reinterpret_cast<char*>(&reply), sizeof(reply), 0) == sizeof(reply));
    assert(reply.Type == PacketType::Welcome && reply.PlayerId == 2 && reply.ConnectionId == 456 && reply.SessionId);
    closesocket(client); NetworkManager::Shutdown();
    puts("PASS: production UDP handshake, endpoint/session checks, roster recovery, sequencing, visual transfer, archive validation");
}
