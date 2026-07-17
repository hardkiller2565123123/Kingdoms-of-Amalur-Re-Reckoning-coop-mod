#include "ImGuiOverlay.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include "CombatScaler.h"
#include "Config.h"
#include "GameState.h"
#include "HookManager.h"
#include "LobbyManager.h"
#include "Logger.h"
#include "MemoryScanner.h"
#include "MinHook.h"
#include "NetworkManager.h"
#include "PositionTracker.h"
#include "RemotePlayerManager.h"
#include "RuntimeInspector.h"
#include "ResearchLab.h"
#include "DInputProxy.h"

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

#include <algorithm>
#include <cctype>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace
{
    using PresentFn = HRESULT(WINAPI*)(IDXGISwapChain*, UINT, UINT);
    using ResizeBuffersFn = HRESULT(WINAPI*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

    PresentFn g_originalPresent = nullptr;
    ResizeBuffersFn g_originalResizeBuffers = nullptr;
    void* g_presentTarget = nullptr;
    void* g_resizeBuffersTarget = nullptr;

    std::atomic<bool> g_hookInstalled = false;
    std::atomic<bool> g_rendererReady = false;
    std::atomic<bool> g_menuOpen = false;

    HWND g_gameWindow = nullptr;
    WNDPROC g_originalWndProc = nullptr;
    ID3D11Device* g_device = nullptr;
    ID3D11DeviceContext* g_context = nullptr;
    ID3D11RenderTargetView* g_renderTarget = nullptr;
    std::recursive_mutex g_renderMutex;
    int g_cursorShowCalls = 0;
    bool g_f1WasDown = false;

    char g_joinAddress[64] = "127.0.0.1";
    int g_networkPort = 7777;
    float g_scanValue = 100.0f;
    char g_manualPositionAddress[32] = "";
    char g_watchAddress[32] = "";
    char g_runtimeFilter[64] = "";
    int g_selectedRuntimeRecord = -1;
    bool g_showOnlyInstantiatedRecords = false;
    bool g_showOnlySameDefinition = false;
    bool g_enableExperimentalRuntimeActions = false;
    bool g_requireSameDefinitionForQueue = true;
    std::vector<MemoryScanner::ModuleInfo> g_moduleSnapshot;

    std::string HexAddress(uintptr_t value)
    {
        std::ostringstream stream;
        stream << "0x" << std::hex << std::uppercase << value;
        return stream.str();
    }

    bool ParseAddress(const char* text, uintptr_t& value)
    {
        if (!text || !*text)
            return false;

        try
        {
            value = static_cast<uintptr_t>(std::stoull(text, nullptr, 0));
            return true;
        }
        catch (...)
        {
            value = 0;
            return false;
        }
    }

    BOOL CALLBACK FindGameWindowCallback(HWND window, LPARAM parameter)
    {
        DWORD processId = 0;
        GetWindowThreadProcessId(window, &processId);

        if (processId != GetCurrentProcessId() || !IsWindowVisible(window) || GetWindow(window, GW_OWNER))
            return TRUE;

        *reinterpret_cast<HWND*>(parameter) = window;
        return FALSE;
    }

    HWND FindGameWindow()
    {
        HWND window = nullptr;
        EnumWindows(FindGameWindowCallback, reinterpret_cast<LPARAM>(&window));
        return window;
    }

    bool IsInputMessage(UINT message)
    {
        switch (message)
        {
        case WM_MOUSEMOVE:
        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL:
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
        case WM_RBUTTONDBLCLK:
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:
        case WM_MBUTTONDBLCLK:
        case WM_XBUTTONDOWN:
        case WM_XBUTTONUP:
        case WM_XBUTTONDBLCLK:
        case WM_KEYDOWN:
        case WM_KEYUP:
        case WM_SYSKEYDOWN:
        case WM_SYSKEYUP:
        case WM_CHAR:
            return true;
        default:
            return false;
        }
    }

    void ReconcileCursor()
    {
        const Config::Settings settings = Config::Get();
        const bool shouldForce = g_menuOpen.load() && settings.ForceCursorVisible;

        if (shouldForce && g_cursorShowCalls == 0)
        {
            int result = ShowCursor(TRUE);
            g_cursorShowCalls = 1;

            while (result < 0 && g_cursorShowCalls < 32)
            {
                result = ShowCursor(TRUE);
                ++g_cursorShowCalls;
            }

            ClipCursor(nullptr);
        }
        else if (!shouldForce && g_cursorShowCalls > 0)
        {
            while (g_cursorShowCalls-- > 0)
                ShowCursor(FALSE);

            g_cursorShowCalls = 0;
        }
    }

    void SetMenuOpenInternal(bool open)
    {
        g_menuOpen.store(open);
        ReconcileCursor();
        Logger::Write(open ? "ImGui menu opened" : "ImGui menu closed");
    }

    LRESULT CALLBACK HookedWndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (g_rendererReady.load())
            ImGui_ImplWin32_WndProcHandler(window, message, wParam, lParam);

        if (g_menuOpen.load() && Config::Get().CaptureInput && IsInputMessage(message))
            return 1;

        return g_originalWndProc
            ? CallWindowProcW(g_originalWndProc, window, message, wParam, lParam)
            : DefWindowProcW(window, message, wParam, lParam);
    }

    void ApplyStyle()
    {
        ImGui::StyleColorsDark();
        ImGuiStyle& style = ImGui::GetStyle();

        style.WindowPadding = ImVec2(14.0f, 12.0f);
        style.FramePadding = ImVec2(10.0f, 6.0f);
        style.ItemSpacing = ImVec2(10.0f, 8.0f);
        style.ItemInnerSpacing = ImVec2(7.0f, 6.0f);
        style.WindowRounding = 8.0f;
        style.ChildRounding = 6.0f;
        style.FrameRounding = 5.0f;
        style.PopupRounding = 5.0f;
        style.ScrollbarRounding = 6.0f;
        style.GrabRounding = 5.0f;
        style.TabRounding = 5.0f;
        style.WindowBorderSize = 1.0f;
        style.ChildBorderSize = 1.0f;

        ImVec4* colors = style.Colors;
        colors[ImGuiCol_Text] = ImVec4(0.95f, 0.93f, 0.89f, 1.00f);
        colors[ImGuiCol_TextDisabled] = ImVec4(0.55f, 0.52f, 0.47f, 1.00f);
        colors[ImGuiCol_WindowBg] = ImVec4(0.055f, 0.045f, 0.035f, 0.96f);
        colors[ImGuiCol_ChildBg] = ImVec4(0.085f, 0.067f, 0.050f, 0.88f);
        colors[ImGuiCol_PopupBg] = ImVec4(0.065f, 0.052f, 0.040f, 0.98f);
        colors[ImGuiCol_Border] = ImVec4(0.45f, 0.29f, 0.13f, 0.78f);
        colors[ImGuiCol_FrameBg] = ImVec4(0.16f, 0.12f, 0.08f, 0.92f);
        colors[ImGuiCol_FrameBgHovered] = ImVec4(0.28f, 0.18f, 0.09f, 0.95f);
        colors[ImGuiCol_FrameBgActive] = ImVec4(0.37f, 0.23f, 0.10f, 1.00f);
        colors[ImGuiCol_TitleBg] = ImVec4(0.09f, 0.065f, 0.045f, 1.00f);
        colors[ImGuiCol_TitleBgActive] = ImVec4(0.17f, 0.10f, 0.045f, 1.00f);
        colors[ImGuiCol_CheckMark] = ImVec4(1.00f, 0.58f, 0.16f, 1.00f);
        colors[ImGuiCol_SliderGrab] = ImVec4(0.92f, 0.43f, 0.10f, 1.00f);
        colors[ImGuiCol_SliderGrabActive] = ImVec4(1.00f, 0.63f, 0.20f, 1.00f);
        colors[ImGuiCol_Button] = ImVec4(0.46f, 0.22f, 0.07f, 0.86f);
        colors[ImGuiCol_ButtonHovered] = ImVec4(0.70f, 0.34f, 0.08f, 1.00f);
        colors[ImGuiCol_ButtonActive] = ImVec4(0.86f, 0.45f, 0.10f, 1.00f);
        colors[ImGuiCol_Header] = ImVec4(0.42f, 0.20f, 0.07f, 0.78f);
        colors[ImGuiCol_HeaderHovered] = ImVec4(0.68f, 0.33f, 0.08f, 0.95f);
        colors[ImGuiCol_HeaderActive] = ImVec4(0.84f, 0.43f, 0.10f, 1.00f);
        colors[ImGuiCol_Tab] = ImVec4(0.16f, 0.10f, 0.055f, 1.00f);
        colors[ImGuiCol_TabHovered] = ImVec4(0.67f, 0.32f, 0.08f, 1.00f);
        colors[ImGuiCol_TabActive] = ImVec4(0.48f, 0.23f, 0.07f, 1.00f);
        colors[ImGuiCol_Separator] = ImVec4(0.43f, 0.26f, 0.10f, 0.70f);
    }

    void StatusCard(const char* id, const char* title, const std::string& value, bool good)
    {
        ImGui::BeginChild(id, ImVec2(0.0f, 66.0f), true);
        ImGui::TextDisabled("%s", title);
        ImGui::TextColored(
            good ? ImVec4(0.45f, 0.90f, 0.48f, 1.0f) : ImVec4(1.0f, 0.48f, 0.30f, 1.0f),
            "%s",
            value.c_str());
        ImGui::EndChild();
    }

    void RenderDashboard()
    {
        ImGui::TextColored(ImVec4(1.0f, 0.58f, 0.16f, 1.0f), "AMALUR COOP");
        ImGui::SameLine();
        ImGui::TextDisabled("one-DLL DirectInput 8 build");
        ImGui::Separator();

        if (ImGui::BeginTable("##statusCards", 3, ImGuiTableFlags_SizingStretchSame))
        {
            const bool dinputReady = DInputProxy::IsReady();
            const bool hooksReady = HookManager::IsReady();
            const bool rendererReady = g_rendererReady.load();

            ImGui::TableNextColumn();
            StatusCard("##dinput", "DirectInput proxy", dinputReady ? "System dinput8.dll" : "System DLL unavailable", dinputReady);
            ImGui::TableNextColumn();
            StatusCard("##hooks", "Hook system", hooksReady ? "MinHook initialized" : "Hook initialization failed", hooksReady);
            ImGui::TableNextColumn();
            StatusCard("##renderer", "Renderer", rendererReady ? "D3D11 / ImGui ready" : "Waiting for D3D11", rendererReady);
            ImGui::EndTable();
        }

        ImGui::Spacing();
        ImGui::Text("Lobby: %d / %d players", LobbyManager::GetPlayerCount(), LobbyManager::GetMaxPlayers());
        ImGui::Text("Network: %s", NetworkManager::GetStatusText().c_str());
        ImGui::Text("Player runtime: %s", PositionTracker::GetStatusText().c_str());
        ImGui::Text("Boss health preview: 100 -> %.1f", CombatScaler::ScaleBossHealth(100.0f));
        ImGui::Text("Enemy health preview: 100 -> %.1f", CombatScaler::ScaleEnemyHealth(100.0f));

        ImGui::Spacing();
        ImGui::TextDisabled("Press F1 to close or reopen this menu.");
    }

    void RenderLobby()
    {
        Config::Settings settings = Config::Get();
        int maxPlayers = settings.MaxPlayers;
        int playerCount = LobbyManager::GetPlayerCount();

        ImGui::Text("Lobby simulation and scaling inputs");
        ImGui::Separator();

        if (ImGui::SliderInt("Maximum players", &maxPlayers, 1, 16))
        {
            settings.MaxPlayers = maxPlayers;
            Config::Set(settings);
            LobbyManager::SetMaxPlayers(maxPlayers);
        }

        playerCount = std::min(playerCount, LobbyManager::GetMaxPlayers());
        if (ImGui::SliderInt("Current players", &playerCount, 1, LobbyManager::GetMaxPlayers()))
            LobbyManager::SetPlayerCount(playerCount);

        ImGui::Spacing();
        ImGui::Text("Current lobby: %d / %d", LobbyManager::GetPlayerCount(), LobbyManager::GetMaxPlayers());
        ImGui::Text("Boss HP multiplier preview: %.2fx", CombatScaler::ScaleBossHealth(100.0f) / 100.0f);
        ImGui::Text("Enemy HP multiplier preview: %.2fx", CombatScaler::ScaleEnemyHealth(100.0f) / 100.0f);
    }

    void RenderNetwork()
    {
        const NetworkManager::Mode mode = NetworkManager::GetMode();
        const NetworkManager::Stats stats = NetworkManager::GetStats();
        const std::vector<NetworkManager::PeerInfo> peers = NetworkManager::GetPeers();

        const bool online = mode != NetworkManager::Mode::Offline;
        const ImVec4 onlineColor = online
            ? ImVec4(0.35f, 0.90f, 0.48f, 1.0f)
            : ImVec4(0.95f, 0.48f, 0.30f, 1.0f);

        ImGui::TextColored(ImVec4(1.0f, 0.58f, 0.16f, 1.0f), "AMALUR NETWORK");
        ImGui::SameLine();
        ImGui::TextDisabled("direct UDP session layer");
        ImGui::Separator();

        if (ImGui::BeginTable("##networkCards", 4, ImGuiTableFlags_SizingStretchSame))
        {
            ImGui::TableNextColumn();
            StatusCard("##netmode", "Session mode", NetworkManager::GetModeText(), online);

            ImGui::TableNextColumn();
            StatusCard("##netpeers", "Connected peers", std::to_string(stats.ConnectedPeers), stats.ConnectedPeers > 0);

            ImGui::TableNextColumn();
            StatusCard("##netping", "Current ping", stats.CurrentPingMs > 0 ? std::to_string(stats.CurrentPingMs) + " ms" : "--", stats.CurrentPingMs > 0);

            ImGui::TableNextColumn();
            StatusCard("##netport", "UDP port", stats.BoundPort > 0 ? std::to_string(stats.BoundPort) : "Not bound", stats.BoundPort > 0);
            ImGui::EndTable();
        }

        ImGui::Spacing();
        ImGui::TextColored(onlineColor, "%s", NetworkManager::GetStatusText().c_str());
        ImGui::Separator();

        ImGui::TextUnformatted("Create or join a session");
        ImGui::SetNextItemWidth(240.0f);
        ImGui::InputText("Host address", g_joinAddress, sizeof(g_joinAddress));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120.0f);
        ImGui::InputInt("UDP port", &g_networkPort);
        g_networkPort = std::clamp(g_networkPort, 1, 65535);

        if (ImGui::Button("Host session", ImVec2(150.0f, 34.0f)))
        {
            if (NetworkManager::Host(static_cast<unsigned short>(g_networkPort)))
                NetworkManager::TryMapPortUpnp(static_cast<unsigned short>(g_networkPort));
        }

        ImGui::SameLine();
        if (ImGui::Button("Join session", ImVec2(150.0f, 34.0f)))
            NetworkManager::Join(g_joinAddress, static_cast<unsigned short>(g_networkPort));

        ImGui::SameLine();
        if (ImGui::Button("Disconnect", ImVec2(150.0f, 34.0f)))
            NetworkManager::Disconnect();

        ImGui::Spacing();
        ImGui::TextDisabled("Two-instance test: host one game, then join 127.0.0.1 from the second game using the same UDP port.");
        ImGui::TextDisabled("Internet play: UPnP is attempted automatically. If unavailable, forward this UDP port in the router and allow DINPUT8.dll/koa.exe through Windows Firewall.");

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextUnformatted("Port reachability");
        ImGui::Text("UPnP: %s", stats.UpnpStatus.empty() ? "Not requested" : stats.UpnpStatus.c_str());
        if (mode == NetworkManager::Mode::Host && !stats.UpnpMapped)
        {
            if (ImGui::Button("Retry UPnP mapping"))
                NetworkManager::TryMapPortUpnp(static_cast<unsigned short>(g_networkPort));
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextUnformatted("Traffic");
        if (ImGui::BeginTable("##traffic", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchSame))
        {
            ImGui::TableSetupColumn("Sent");
            ImGui::TableSetupColumn("Received");
            ImGui::TableSetupColumn("Bytes out");
            ImGui::TableSetupColumn("Bytes in");
            ImGui::TableSetupColumn("Dropped");
            ImGui::TableHeadersRow();
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%llu", static_cast<unsigned long long>(stats.PacketsSent));
            ImGui::TableNextColumn(); ImGui::Text("%llu", static_cast<unsigned long long>(stats.PacketsReceived));
            ImGui::TableNextColumn(); ImGui::Text("%llu", static_cast<unsigned long long>(stats.BytesSent));
            ImGui::TableNextColumn(); ImGui::Text("%llu", static_cast<unsigned long long>(stats.BytesReceived));
            ImGui::TableNextColumn(); ImGui::Text("%llu", static_cast<unsigned long long>(stats.DroppedPackets));
            ImGui::EndTable();
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Text("Peers (%zu)", peers.size());

        if (ImGui::BeginTable("##peers", 7,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY,
            ImVec2(0.0f, 180.0f)))
        {
            ImGui::TableSetupColumn("ID");
            ImGui::TableSetupColumn("Address");
            ImGui::TableSetupColumn("State");
            ImGui::TableSetupColumn("Ping");
            ImGui::TableSetupColumn("X");
            ImGui::TableSetupColumn("Y");
            ImGui::TableSetupColumn("Z");
            ImGui::TableHeadersRow();

            for (const auto& peer : peers)
            {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::Text("%u", peer.PlayerId);
                ImGui::TableNextColumn(); ImGui::Text("%s:%u", peer.Address.c_str(), peer.Port);
                ImGui::TableNextColumn();
                ImGui::TextColored(peer.Connected ? ImVec4(0.35f, 0.90f, 0.48f, 1.0f) : ImVec4(0.95f, 0.48f, 0.30f, 1.0f),
                    "%s", peer.Connected ? "Connected" : "Timed out");
                ImGui::TableNextColumn(); ImGui::Text("%u ms", peer.PingMs);
                ImGui::TableNextColumn(); ImGui::Text(peer.HasTransform ? "%.2f" : "--", peer.Position.X);
                ImGui::TableNextColumn(); ImGui::Text(peer.HasTransform ? "%.2f" : "--", peer.Position.Y);
                ImGui::TableNextColumn(); ImGui::Text(peer.HasTransform ? "%.2f" : "--", peer.Position.Z);
            }
            ImGui::EndTable();
        }

        ImGui::Spacing();
        if (NetworkManager::HasRemoteTransform())
        {
            const NetworkManager::Vec3 remote = NetworkManager::GetRemoteTransform();
            ImGui::TextColored(ImVec4(0.35f, 0.90f, 0.48f, 1.0f), "Remote transform live");
            ImGui::Text("X %.3f   Y %.3f   Z %.3f   age %llu ms",
                remote.X, remote.Y, remote.Z,
                static_cast<unsigned long long>(NetworkManager::GetRemoteTransformAgeMs()));
        }
        else
        {
            ImGui::TextDisabled("Waiting for a remote player transform...");
        }
    }

    void RenderScaling()
    {
        Config::Settings settings = Config::Get();
        bool changed = false;

        changed |= ImGui::Checkbox("Enable combat scaling", &settings.EnableScaling);
        ImGui::Separator();
        changed |= ImGui::SliderFloat("Boss health / extra player", &settings.BossHealthPerExtraPlayer, 0.0f, 3.0f, "%.2f");
        changed |= ImGui::SliderFloat("Enemy health / extra player", &settings.EnemyHealthPerExtraPlayer, 0.0f, 3.0f, "%.2f");
        changed |= ImGui::SliderFloat("Boss damage / extra player", &settings.BossDamagePerExtraPlayer, 0.0f, 2.0f, "%.2f");
        changed |= ImGui::SliderFloat("Enemy damage / extra player", &settings.EnemyDamagePerExtraPlayer, 0.0f, 2.0f, "%.2f");

        if (changed)
            Config::Set(settings);

        ImGui::Spacing();
        ImGui::Text("At %d players:", LobbyManager::GetPlayerCount());
        ImGui::BulletText("Boss 100 HP -> %.1f HP", CombatScaler::ScaleBossHealth(100.0f));
        ImGui::BulletText("Enemy 100 HP -> %.1f HP", CombatScaler::ScaleEnemyHealth(100.0f));
        ImGui::BulletText("Boss 100 damage -> %.1f", CombatScaler::ScaleBossDamage(100.0f));
        ImGui::BulletText("Enemy 100 damage -> %.1f", CombatScaler::ScaleEnemyDamage(100.0f));
    }

    void RenderPlayer()
    {
        const bool tracked = PositionTracker::HasPosition();
        const PositionTracker::Vec3 position = PositionTracker::GetPosition();

        ImGui::TextUnformatted("Verified local-player runtime inspector");
        ImGui::Separator();

        ImGui::Text("Status: %s", PositionTracker::GetStatusText().c_str());
        ImGui::Text("Confidence: %d%%", PositionTracker::GetConfidence());
        ImGui::TextDisabled("Read-only tracker: no gameplay hook and no float scan.");

        ImGui::Spacing();
        if (ImGui::BeginTable("##playerRuntime", 2,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
        {
            auto row = [](const char* name, uintptr_t value)
            {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(name);
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("0x%08X", static_cast<unsigned int>(value));
            };

            row("Player root", PositionTracker::GetPlayerRoot());
            row("Player manager", PositionTracker::GetPlayerManager());
            row("Player context", PositionTracker::GetPlayerContext());
            row("Object handle", PositionTracker::GetObjectHandle());
            row("Runtime object", PositionTracker::GetRuntimeObject());
            row("Transform component", PositionTracker::GetTransformComponent());
            ImGui::EndTable();
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextUnformatted("Position");

        if (tracked)
        {
            ImGui::Text("X: %.4f", position.X);
            ImGui::Text("Y: %.4f", position.Y);
            ImGui::Text("Z: %.4f", position.Z);

            ImGui::Spacing();
            ImGui::TextDisabled("Verified layout: transform +0x24 / +0x28 / +0x2C");
        }
        else
        {
            ImGui::TextDisabled("Load into gameplay and wait for the local player object.");
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextUnformatted("Visible two-player proxy");
        ImGui::TextWrapped("When a network peer sends transforms, AmalurCoop now queues one inactive game-managed actor record and moves that actor through the verified sub_F5DD50 transform path.");
        ImGui::Text("Status: %s", RemotePlayerManager::GetStatusText().c_str());
        ImGui::Text("Proxy handle: 0x%08X", RemotePlayerManager::GetProxyObjectHandle());

        if (ImGui::Button("Spawn visible test proxy near local player"))
            RemotePlayerManager::SpawnDummyNearLocalPlayer();

        ImGui::SameLine();
        if (ImGui::Button("Stop controlling proxy"))
            RemotePlayerManager::ClearDummy();

        if (RemotePlayerManager::HasDummy())
        {
            const GameState::Vec3 dummy = RemotePlayerManager::GetDummyPosition();
            ImGui::Text("Proxy target: X %.3f  Y %.3f  Z %.3f", dummy.X, dummy.Y, dummy.Z);
        }
    }

    void RenderRuntime()
    {
        const auto records = RuntimeInspector::GetRecords();
        const int localIndex = RuntimeInspector::GetLocalRecordIndex();
        const uint32_t localDefinition = RuntimeInspector::GetLocalDefinitionId();

        ImGui::TextUnformatted("Runtime Record Manager");
        ImGui::Separator();
        ImGui::Text("Status: %s", RuntimeInspector::GetStatusText().c_str());
        ImGui::Text("Manager: 0x%08X", static_cast<unsigned int>(RuntimeInspector::GetManager()));
        ImGui::Text("Manager count: %d", RuntimeInspector::GetManagerRecordCount());
        ImGui::Text("Visible records: %zu", records.size());
        ImGui::Text("Local record: %d", localIndex);
        ImGui::Text("Local definition: 0x%08X", localDefinition);

        if (ImGui::Button("Refresh runtime records"))
            RuntimeInspector::RefreshNow();

        ImGui::SameLine();
        ImGui::Checkbox("Instantiated only", &g_showOnlyInstantiatedRecords);
        ImGui::SameLine();
        ImGui::Checkbox("Same definition only", &g_showOnlySameDefinition);

        ImGui::InputTextWithHint("##runtimeFilter", "Filter index, handle, definition, flags...",
            g_runtimeFilter, sizeof(g_runtimeFilter));

        ImGui::Spacing();

        if (ImGui::BeginTable("##runtimeRecords", 9,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
            ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable |
            ImGuiTableFlags_Sortable,
            ImVec2(0.0f, 300.0f)))
        {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Index", ImGuiTableColumnFlags_WidthFixed, 54.0f);
            ImGui::TableSetupColumn("Record", ImGuiTableColumnFlags_WidthFixed, 86.0f);
            ImGui::TableSetupColumn("Object", ImGuiTableColumnFlags_WidthFixed, 86.0f);
            ImGui::TableSetupColumn("Definition", ImGuiTableColumnFlags_WidthFixed, 86.0f);
            ImGui::TableSetupColumn("Flags", ImGuiTableColumnFlags_WidthFixed, 58.0f);
            ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthFixed, 86.0f);
            ImGui::TableSetupColumn("Live", ImGuiTableColumnFlags_WidthFixed, 42.0f);
            ImGui::TableSetupColumn("Pending", ImGuiTableColumnFlags_WidthFixed, 55.0f);
            ImGui::TableSetupColumn("Local/Same", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();

            for (const auto& record : records)
            {
                if (g_showOnlyInstantiatedRecords && !record.Instantiated)
                    continue;
                if (g_showOnlySameDefinition && !record.SameDefinitionAsLocal)
                    continue;

                if (g_runtimeFilter[0] != '\0')
                {
                    char searchable[256]{};
                    sprintf_s(searchable, "%d %08X %08X %08X %04X %08X",
                        record.Index,
                        static_cast<unsigned int>(record.Address),
                        record.ObjectHandle,
                        record.DefinitionId,
                        record.Flags,
                        record.RuntimeState);
                    std::string haystack = searchable;
                    std::string needle = g_runtimeFilter;
                    std::transform(haystack.begin(), haystack.end(), haystack.begin(), ::tolower);
                    std::transform(needle.begin(), needle.end(), needle.begin(), ::tolower);
                    if (haystack.find(needle) == std::string::npos)
                        continue;
                }

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                const bool selected = g_selectedRuntimeRecord == record.Index;
                char label[32]{};
                sprintf_s(label, "%d##runtimeRecord", record.Index);
                if (ImGui::Selectable(label, selected, ImGuiSelectableFlags_SpanAllColumns))
                    g_selectedRuntimeRecord = record.Index;

                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%08X", static_cast<unsigned int>(record.Address));
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%08X", record.ObjectHandle);
                ImGui::TableSetColumnIndex(3);
                ImGui::Text("%08X", record.DefinitionId);
                ImGui::TableSetColumnIndex(4);
                ImGui::Text("%04X", record.Flags);
                ImGui::TableSetColumnIndex(5);
                ImGui::Text("%08X", record.RuntimeState);
                ImGui::TableSetColumnIndex(6);
                ImGui::TextUnformatted(record.Instantiated ? "Yes" : "No");
                ImGui::TableSetColumnIndex(7);
                ImGui::TextUnformatted(record.Pending ? "Yes" : "No");
                ImGui::TableSetColumnIndex(8);
                if (record.IsLocalPlayer)
                    ImGui::TextColored(ImVec4(0.35f, 0.90f, 0.48f, 1.0f), "LOCAL");
                else if (record.SameDefinitionAsLocal)
                    ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.20f, 1.0f), "Same definition");
                else
                    ImGui::TextDisabled("-");
            }

            ImGui::EndTable();
        }

        const RuntimeInspector::Record* selectedRecord = nullptr;
        for (const auto& record : records)
        {
            if (record.Index == g_selectedRuntimeRecord)
            {
                selectedRecord = &record;
                break;
            }
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextUnformatted("Selected Record");

        if (!selectedRecord)
        {
            ImGui::TextDisabled("Select a record from the table.");
            return;
        }

        if (ImGui::BeginTable("##selectedRuntimeRecord", 2,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
        {
            auto row = [](const char* name, unsigned int value)
            {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(name);
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("0x%08X", value);
            };

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted("Index");
            ImGui::TableSetColumnIndex(1); ImGui::Text("%d", selectedRecord->Index);
            row("Record address", static_cast<unsigned int>(selectedRecord->Address));
            row("Persistent handle +0x18", selectedRecord->PersistentHandle);
            row("Object handle +0x50", selectedRecord->ObjectHandle);
            row("Runtime state +0x54", selectedRecord->RuntimeState);
            row("Flags +0x68", selectedRecord->Flags);
            row("Spawn flags +0x6A", selectedRecord->SpawnFlags);
            row("Definition reference +0x6C", static_cast<unsigned int>(selectedRecord->DefinitionReference));
            row("Resolved definition", selectedRecord->DefinitionId);
            row("Runtime definition raw +0xEC", selectedRecord->RuntimeDefinitionRaw);
            row("Runtime object", static_cast<unsigned int>(selectedRecord->RuntimeObject));
            ImGui::EndTable();
        }

        ImGui::Text("Instantiated: %s", selectedRecord->Instantiated ? "yes" : "no");
        ImGui::Text("Pending: %s", selectedRecord->Pending ? "yes" : "no");
        ImGui::Text("Local player: %s", selectedRecord->IsLocalPlayer ? "yes" : "no");
        ImGui::Text("Same definition as local: %s", selectedRecord->SameDefinitionAsLocal ? "yes" : "no");

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextUnformatted("Experimental Existing-Record Queue");
        ImGui::TextWrapped("This calls the verified sub_EA5240 streaming request on an existing inactive record. It does not clone memory or call the low-level object constructor directly. This is still experimental and may crash if the selected record is not a valid spawn candidate.");
        ImGui::Checkbox("Enable experimental runtime actions", &g_enableExperimentalRuntimeActions);
        ImGui::Checkbox("Require same definition as local player", &g_requireSameDefinitionForQueue);

        const bool canQueue = g_enableExperimentalRuntimeActions &&
            !selectedRecord->IsLocalPlayer &&
            !selectedRecord->Instantiated &&
            selectedRecord->ObjectHandle == 0 &&
            !selectedRecord->Pending &&
            (selectedRecord->Flags & 0x3u) == 0 &&
            (!g_requireSameDefinitionForQueue || selectedRecord->SameDefinitionAsLocal);

        if (!canQueue)
            ImGui::BeginDisabled();
        if (ImGui::Button("Queue selected existing record"))
            RuntimeInspector::QueueExistingRecord(selectedRecord->Index, g_requireSameDefinitionForQueue);
        if (!canQueue)
            ImGui::EndDisabled();

        ImGui::TextWrapped("Last action: %s", RuntimeInspector::GetLastActionText().c_str());
    }

    void RenderResearchLab()
    {
        static char filter[96]{};
        static int selected = -1;
        static bool autoScroll = true;

        const auto functions = ResearchLab::GetFunctions();
        const auto traces = ResearchLab::GetTraceEntries(512);

        ImGui::TextColored(ImVec4(1.0f, 0.58f, 0.16f, 1.0f), "GAMEPLAY RESEARCH LAB");
        ImGui::SameLine();
        ImGui::TextDisabled("verified addresses and opt-in trace hooks");
        ImGui::Separator();
        ImGui::TextWrapped("All gameplay detours start disabled. Enable only one or two traces at a time while loaded into gameplay. The unsafe-call gate is intentionally separate and no guessed low-level spawn/action call is executed automatically.");
        ImGui::Text("Status: %s", ResearchLab::GetStatusText().c_str());
        ImGui::Text("Game base: 0x%08X", static_cast<unsigned int>(reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr))));

        bool unsafe = ResearchLab::IsUnsafeGateEnabled();
        if (ImGui::Checkbox("Enable unsafe testing gate", &unsafe))
            ResearchLab::SetUnsafeGateEnabled(unsafe);
        ImGui::SameLine();
        if (ImGui::Button("Disable every trace"))
            ResearchLab::DisableAllTraces();
        ImGui::SameLine();
        if (ImGui::Button("Clear trace log"))
            ResearchLab::ClearTrace();

        ImGui::Spacing();
        ImGui::TextUnformatted("Confirmed trace presets");
        if (ImGui::Button("Action lifecycle"))
        {
            ResearchLab::EnableTrace("sub_FE65B0");
            ResearchLab::EnableTrace("sub_FE0860");
            ResearchLab::EnableTrace("sub_FE0FE0");
            ResearchLab::EnableTrace("sub_FE1EB0");
            ResearchLab::EnableTrace("sub_FE1D80");
        }
        ImGui::SameLine();
        if (ImGui::Button("Spawned actors"))
        {
            ResearchLab::EnableTrace("sub_F86E20");
            ResearchLab::EnableTrace("sub_FE8D80");
            ResearchLab::EnableTrace("sub_EF6470");
        }
        ImGui::SameLine();
        if (ImGui::Button("Modifiers/callbacks"))
        {
            ResearchLab::EnableTrace("sub_E033D0");
            ResearchLab::EnableTrace("sub_FC7F20");
            ResearchLab::EnableTrace("sub_FC7E10");
            ResearchLab::EnableTrace("sub_FBEF30");
        }
        if (ImGui::Button("Affected actors"))
        {
            ResearchLab::EnableTrace("sub_FE5AB0");
            ResearchLab::EnableTrace("sub_FE6950");
        }
        ImGui::SameLine();
        if (ImGui::Button("Transform/resource"))
        {
            ResearchLab::EnableTrace("sub_F5DD50");
            ResearchLab::EnableTrace("sub_F6CA70");
            ResearchLab::EnableTrace("sub_FD72B0");
        }
        ImGui::SameLine();
        if (ImGui::Button("Event dispatch"))
        {
            ResearchLab::EnableTrace("sub_E4EC50");
            ResearchLab::EnableTrace("sub_FC8790");
        }

        ImGui::Spacing();
        ImGui::TextUnformatted("NPC lifecycle controls");
        ImGui::TextWrapped("Existing-record spawning is available in Runtime Records. Direct NPC delete/kill is intentionally not exposed yet: the death pipeline address is confirmed, but its complete calling contract and required actor/component arguments are not. Calling it from a guessed button could delete the player, corrupt the actor manager, or crash the game.");

        ImGui::InputTextWithHint("##researchFilter", "Filter actor, action, transform, address...", filter, sizeof(filter));

        if (ImGui::BeginTable("##researchFunctions", 7,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY,
            ImVec2(0.0f, 280.0f)))
        {
            ImGui::TableSetupColumn("Function", ImGuiTableColumnFlags_WidthFixed, 105.0f);
            ImGui::TableSetupColumn("Category", ImGuiTableColumnFlags_WidthFixed, 75.0f);
            ImGui::TableSetupColumn("IDA", ImGuiTableColumnFlags_WidthFixed, 85.0f);
            ImGui::TableSetupColumn("Runtime", ImGuiTableColumnFlags_WidthFixed, 85.0f);
            ImGui::TableSetupColumn("Trace", ImGuiTableColumnFlags_WidthFixed, 58.0f);
            ImGui::TableSetupColumn("Calls", ImGuiTableColumnFlags_WidthFixed, 55.0f);
            ImGui::TableSetupColumn("Meaning", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();

            for (size_t i = 0; i < functions.size(); ++i)
            {
                const auto& f = functions[i];
                std::string searchable = std::string(f.Name) + " " + f.Category + " " + f.Description;
                std::string needle = filter;
                std::transform(searchable.begin(), searchable.end(), searchable.begin(), ::tolower);
                std::transform(needle.begin(), needle.end(), needle.begin(), ::tolower);
                if (!needle.empty() && searchable.find(needle) == std::string::npos)
                    continue;

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                char label[96]{};
                sprintf_s(label, "%s##research%zu", f.Name, i);
                if (ImGui::Selectable(label, selected == static_cast<int>(i), ImGuiSelectableFlags_SpanAllColumns))
                    selected = static_cast<int>(i);
                ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(f.Category);
                ImGui::TableSetColumnIndex(2); ImGui::Text("%08X", static_cast<unsigned int>(f.IdaAddress));
                ImGui::TableSetColumnIndex(3); ImGui::Text("%08X", static_cast<unsigned int>(ResearchLab::RuntimeAddress(f.IdaAddress)));
                ImGui::TableSetColumnIndex(4);
                if (f.Traceable)
                {
                    bool enabled = f.Hooked;
                    ImGui::PushID(static_cast<int>(i));
                    if (ImGui::Checkbox("##trace", &enabled))
                    {
                        if (enabled) ResearchLab::EnableTrace(f.Name);
                        else ResearchLab::DisableTrace(f.Name);
                    }
                    ImGui::PopID();
                }
                else ImGui::TextDisabled("View");
                ImGui::TableSetColumnIndex(5); ImGui::Text("%llu", static_cast<unsigned long long>(f.Calls));
                ImGui::TableSetColumnIndex(6); ImGui::TextUnformatted(f.Description);
            }
            ImGui::EndTable();
        }

        if (selected >= 0 && selected < static_cast<int>(functions.size()))
        {
            const auto& f = functions[selected];
            ImGui::Text("Selected: %s | %s", f.Name, f.Description);
            ImGui::Text("Runtime address: 0x%08X", static_cast<unsigned int>(ResearchLab::RuntimeAddress(f.IdaAddress)));
            ImGui::TextDisabled("Traceable means a typed logging detour was added. View-only entries are documented but are not hooked because their complete calling contract is not yet safe enough.");
        }

        ImGui::Separator();
        ImGui::Checkbox("Auto-scroll trace", &autoScroll);
        ImGui::SameLine();
        ImGui::Text("Entries: %zu", traces.size());
        ImGui::BeginChild("##researchTrace", ImVec2(0.0f, 0.0f), true, ImGuiWindowFlags_HorizontalScrollbar);
        for (const auto& entry : traces)
            ImGui::Text("#%llu T%u +%llu  %-12s %s",
                static_cast<unsigned long long>(entry.Sequence), entry.ThreadId,
                static_cast<unsigned long long>(entry.Tick), entry.Function.c_str(), entry.Details.c_str());
        if (autoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 10.0f)
            ImGui::SetScrollHereY(1.0f);
        ImGui::EndChild();
    }

    void RenderScanner()
    {
        ImGui::Text("Float memory scanner");
        ImGui::Separator();
        ImGui::InputFloat("Target value", &g_scanValue, 0.1f, 1.0f, "%.3f");

        if (ImGui::Button("First scan"))
            MemoryScanner::FirstFloatScan(g_scanValue);

        ImGui::SameLine();
        if (ImGui::Button("Next scan"))
            MemoryScanner::NextFloatScan(g_scanValue);

        ImGui::SameLine();
        if (ImGui::Button("Reset"))
            MemoryScanner::ResetFloatScan();

        ImGui::Text("Status: %s", MemoryScanner::GetScanStatus().c_str());
        ImGui::Text("Results: %zu", MemoryScanner::GetFloatScanCount());

        const auto results = MemoryScanner::GetFloatScanResults(100);
        ImGui::BeginChild("##scanResults", ImVec2(0.0f, 0.0f), true, ImGuiWindowFlags_HorizontalScrollbar);
        for (const auto& result : results)
            ImGui::Text("%s = %.6f", HexAddress(result.Address).c_str(), result.Value);
        ImGui::EndChild();
    }

    void RenderDebug()
    {
        ImGui::TextUnformatted("Debug tools");
        ImGui::Separator();

        ImGui::TextDisabled("The separate interactive CMD console was removed for stability.\nThe always-on logger remains available in AmalurCoop.log.");

        ImGui::Spacing();
        ImGui::Separator();

        ImGui::TextUnformatted("Memory watch");
        ImGui::InputText(
            "Watch address",
            g_watchAddress,
            sizeof(g_watchAddress));

        uintptr_t watchAddress = 0;

        if (ParseAddress(g_watchAddress, watchAddress))
        {
            float floatValue = 0.0f;
            int intValue = 0;

            const bool hasFloat =
                MemoryScanner::ReadFloat(
                    watchAddress,
                    floatValue);

            const bool hasInt =
                MemoryScanner::ReadInt(
                    watchAddress,
                    intValue);

            ImGui::Text(
                "Address: %s",
                HexAddress(watchAddress).c_str());

            if (hasFloat)
            {
                ImGui::Text(
                    "Float value: %.6f",
                    floatValue);
            }
            else
            {
                ImGui::TextDisabled(
                    "Float value: unreadable");
            }

            if (hasInt)
            {
                ImGui::Text(
                    "Integer value: %d",
                    intValue);
            }
            else
            {
                ImGui::TextDisabled(
                    "Integer value: unreadable");
            }
        }
        else if (g_watchAddress[0] != '\0')
        {
            ImGui::TextColored(
                ImVec4(1.0f, 0.48f, 0.30f, 1.0f),
                "Enter a valid address such as 0x12345678");
        }
        else
        {
            ImGui::TextDisabled(
                "Enter an address to inspect its float and integer values.");
        }

        ImGui::Spacing();
        ImGui::Separator();

        ImGui::TextUnformatted("Loaded modules");

        if (ImGui::Button("Refresh module list"))
        {
            g_moduleSnapshot =
                MemoryScanner::GetLoadedModules();

            Logger::Write(
                "Loaded-module list refreshed from ImGui");
        }

        ImGui::SameLine();

        ImGui::TextDisabled(
            "%zu module(s)",
            g_moduleSnapshot.size());

        ImGui::BeginChild(
            "##modules",
            ImVec2(0.0f, 0.0f),
            true,
            ImGuiWindowFlags_HorizontalScrollbar);

        if (g_moduleSnapshot.empty())
        {
            ImGui::TextDisabled(
                "Press Refresh module list to enumerate loaded modules.");
        }
        else
        {
            for (const auto& module : g_moduleSnapshot)
            {
                ImGui::Text(
                    "%-28s  %s  size 0x%zX",
                    module.Name.c_str(),
                    HexAddress(module.Base).c_str(),
                    module.Size);
            }
        }

        ImGui::EndChild();
    }

    void RenderSettings()
    {
        Config::Settings settings = Config::Get();
        bool changed = false;

        ImGui::Text("Interface and startup behavior");
        ImGui::Separator();
        changed |= ImGui::Checkbox("Open menu on startup", &settings.MenuStartsOpen);
        changed |= ImGui::Checkbox("Block gameplay input while menu is open", &settings.CaptureInput);
        changed |= ImGui::Checkbox("Force mouse cursor visible", &settings.ForceCursorVisible);
        changed |= ImGui::Checkbox("Run automatic memory scan at startup", &settings.AutoScanOnStartup);
        changed |= ImGui::Checkbox("Open debug console at startup", &settings.DebugConsoleOnStartup);
        changed |= ImGui::SliderFloat("Menu scale", &settings.MenuScale, 0.75f, 1.75f, "%.2fx");

        if (changed)
        {
            Config::Set(settings);
            ReconcileCursor();
        }

        ImGui::Spacing();
        if (ImGui::Button("Save settings"))
            Config::Save();

        ImGui::SameLine();
        if (ImGui::Button("Reset defaults"))
        {
            Config::ResetToDefaults();
            LobbyManager::SetMaxPlayers(Config::Get().MaxPlayers);
            ReconcileCursor();
        }

        ImGui::Spacing();
        ImGui::TextDisabled("F1 is always reserved for toggling this menu. Disable input blocking to keep walking while it is open.");
    }

    void RenderMenu()
    {
        ImGuiIO& io = ImGui::GetIO();
        io.FontGlobalScale = Config::Get().MenuScale;

        ImGui::SetNextWindowSize(ImVec2(900.0f, 620.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));

        bool open = g_menuOpen.load();
        if (!ImGui::Begin("AmalurCoop // F1", &open, ImGuiWindowFlags_NoCollapse))
        {
            ImGui::End();
            if (open != g_menuOpen.load())
                SetMenuOpenInternal(open);
            return;
        }

        if (ImGui::BeginTabBar("##mainTabs", ImGuiTabBarFlags_FittingPolicyScroll))
        {
            if (ImGui::BeginTabItem("Dashboard")) { RenderDashboard(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Lobby")) { RenderLobby(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Network")) { RenderNetwork(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Scaling")) { RenderScaling(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Player")) { RenderPlayer(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Runtime")) { RenderRuntime(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Research Lab")) { RenderResearchLab(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Scanner")) { RenderScanner(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Debug")) { RenderDebug(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Settings")) { RenderSettings(); ImGui::EndTabItem(); }
            ImGui::EndTabBar();
        }

        ImGui::End();

        if (open != g_menuOpen.load())
            SetMenuOpenInternal(open);
    }

    void ReleaseRenderTarget()
    {
        if (g_renderTarget)
        {
            g_renderTarget->Release();
            g_renderTarget = nullptr;
        }
    }

    bool CreateRenderTarget(IDXGISwapChain* swapChain)
    {
        ReleaseRenderTarget();

        ID3D11Texture2D* backBuffer = nullptr;
        const HRESULT hr = swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer));
        if (FAILED(hr) || !backBuffer)
            return false;

        const HRESULT viewHr = g_device->CreateRenderTargetView(backBuffer, nullptr, &g_renderTarget);
        backBuffer->Release();
        return SUCCEEDED(viewHr) && g_renderTarget;
    }

    bool InitializeRenderer(IDXGISwapChain* swapChain)
    {
        if (g_rendererReady.load())
            return true;

        if (!swapChain)
            return false;

        if (FAILED(swapChain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&g_device))) || !g_device)
            return false;

        g_device->GetImmediateContext(&g_context);
        if (!g_context)
        {
            g_device->Release();
            g_device = nullptr;
            return false;
        }

        DXGI_SWAP_CHAIN_DESC desc{};
        if (FAILED(swapChain->GetDesc(&desc)))
            return false;

        g_gameWindow = desc.OutputWindow;
        if (!IsWindow(g_gameWindow))
            g_gameWindow = FindGameWindow();
        if (!IsWindow(g_gameWindow))
            return false;

        if (!CreateRenderTarget(swapChain))
            return false;

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        ApplyStyle();

        if (!ImGui_ImplWin32_Init(g_gameWindow))
        {
            ImGui::DestroyContext();
            ReleaseRenderTarget();
            return false;
        }

        if (!ImGui_ImplDX11_Init(g_device, g_context))
        {
            ImGui_ImplWin32_Shutdown();
            ImGui::DestroyContext();
            ReleaseRenderTarget();
            return false;
        }

        SetLastError(0);
        g_originalWndProc = reinterpret_cast<WNDPROC>(
            SetWindowLongPtrW(g_gameWindow, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(HookedWndProc)));

        if (!g_originalWndProc && GetLastError() != 0)
        {
            ImGui_ImplDX11_Shutdown();
            ImGui_ImplWin32_Shutdown();
            ImGui::DestroyContext();
            ReleaseRenderTarget();
            return false;
        }

        g_rendererReady.store(true);
        g_menuOpen.store(Config::Get().MenuStartsOpen);
        ReconcileCursor();
        Logger::Write("Direct3D 11 ImGui renderer initialized");
        return true;
    }

    HRESULT WINAPI HookedPresent(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags)
    {
        std::lock_guard<std::recursive_mutex> lock(g_renderMutex);

        const bool f1Down = (GetAsyncKeyState(VK_F1) & 0x8000) != 0;
        if (f1Down && !g_f1WasDown)
            SetMenuOpenInternal(!g_menuOpen.load());
        g_f1WasDown = f1Down;

        if (!g_rendererReady.load())
            InitializeRenderer(swapChain);

        // Actor creation touches world/graphics systems and must run from the
        // game's render thread rather than the background network thread.
        RemotePlayerManager::PumpGameThread();

        if (g_rendererReady.load() && g_renderTarget)
        {
            ReconcileCursor();
            ImGui_ImplDX11_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();

            if (g_menuOpen.load())
                RenderMenu();

            ImGui::Render();
            g_context->OMSetRenderTargets(1, &g_renderTarget, nullptr);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        }

        return g_originalPresent ? g_originalPresent(swapChain, syncInterval, flags) : S_OK;
    }

    HRESULT WINAPI HookedResizeBuffers(
        IDXGISwapChain* swapChain,
        UINT bufferCount,
        UINT width,
        UINT height,
        DXGI_FORMAT newFormat,
        UINT swapChainFlags)
    {
        std::lock_guard<std::recursive_mutex> lock(g_renderMutex);
        ReleaseRenderTarget();

        const HRESULT result = g_originalResizeBuffers
            ? g_originalResizeBuffers(swapChain, bufferCount, width, height, newFormat, swapChainFlags)
            : E_FAIL;

        if (SUCCEEDED(result) && g_rendererReady.load())
            CreateRenderTarget(swapChain);

        return result;
    }

    bool GetD3D11Methods(void*& present, void*& resizeBuffers)
    {
        const wchar_t* className = L"AmalurCoopD3D11Probe";
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = className;
        RegisterClassExW(&wc);

        HWND window = CreateWindowExW(0, className, L"", WS_OVERLAPPEDWINDOW,
            0, 0, 100, 100, nullptr, nullptr, wc.hInstance, nullptr);
        if (!window)
        {
            UnregisterClassW(className, wc.hInstance);
            return false;
        }

        DXGI_SWAP_CHAIN_DESC desc{};
        desc.BufferCount = 1;
        desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.OutputWindow = window;
        desc.SampleDesc.Count = 1;
        desc.Windowed = TRUE;
        desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

        IDXGISwapChain* swapChain = nullptr;
        ID3D11Device* device = nullptr;
        ID3D11DeviceContext* context = nullptr;
        D3D_FEATURE_LEVEL featureLevel{};
        const D3D_FEATURE_LEVEL levels[] = {
            D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1,
            D3D_FEATURE_LEVEL_10_0
        };

        HRESULT hr = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
            levels, static_cast<UINT>(_countof(levels)), D3D11_SDK_VERSION,
            &desc, &swapChain, &device, &featureLevel, &context);

        if (FAILED(hr))
        {
            hr = D3D11CreateDeviceAndSwapChain(
                nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
                levels, static_cast<UINT>(_countof(levels)), D3D11_SDK_VERSION,
                &desc, &swapChain, &device, &featureLevel, &context);
        }

        if (SUCCEEDED(hr) && swapChain)
        {
            void** table = *reinterpret_cast<void***>(swapChain);
            present = table[8];
            resizeBuffers = table[13];
        }

        if (context) context->Release();
        if (device) device->Release();
        if (swapChain) swapChain->Release();
        DestroyWindow(window);
        UnregisterClassW(className, wc.hInstance);
        return present && resizeBuffers;
    }

}

namespace ImGuiOverlay
{
    bool Initialize()
    {
        if (g_hookInstalled.load())
            return true;

        if (!HookManager::IsReady())
        {
            Logger::Write("ImGui hook skipped: MinHook is not ready");
            return false;
        }

        if (!GetD3D11Methods(g_presentTarget, g_resizeBuffersTarget))
        {
            Logger::Write("ImGui hook failed: unable to resolve D3D11 methods");
            return false;
        }

        if (MH_CreateHook(g_presentTarget, reinterpret_cast<void*>(&HookedPresent),
            reinterpret_cast<void**>(&g_originalPresent)) != MH_OK)
        {
            Logger::Write("ImGui hook failed: Present MH_CreateHook");
            return false;
        }

        if (MH_CreateHook(g_resizeBuffersTarget, reinterpret_cast<void*>(&HookedResizeBuffers),
            reinterpret_cast<void**>(&g_originalResizeBuffers)) != MH_OK)
        {
            MH_RemoveHook(g_presentTarget);
            Logger::Write("ImGui hook failed: ResizeBuffers MH_CreateHook");
            return false;
        }

        if (MH_EnableHook(g_presentTarget) != MH_OK || MH_EnableHook(g_resizeBuffersTarget) != MH_OK)
        {
            MH_DisableHook(g_presentTarget);
            MH_DisableHook(g_resizeBuffersTarget);
            MH_RemoveHook(g_resizeBuffersTarget);
            MH_RemoveHook(g_presentTarget);
            Logger::Write("ImGui hook failed: unable to enable D3D11 hooks");
            return false;
        }

        g_hookInstalled.store(true);
        Logger::Write("Direct3D 11 Present/ResizeBuffers hooks installed");
        return true;
    }

    void Shutdown()
    {
        if (g_presentTarget) MH_DisableHook(g_presentTarget);
        if (g_resizeBuffersTarget) MH_DisableHook(g_resizeBuffersTarget);

        std::lock_guard<std::recursive_mutex> lock(g_renderMutex);
        if (g_rendererReady.load())
        {
            SetMenuOpenInternal(false);
            if (g_gameWindow && g_originalWndProc)
                SetWindowLongPtrW(g_gameWindow, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_originalWndProc));

            ImGui_ImplDX11_Shutdown();
            ImGui_ImplWin32_Shutdown();
            ImGui::DestroyContext();
        }

        ReleaseRenderTarget();
        if (g_context) { g_context->Release(); g_context = nullptr; }
        if (g_device) { g_device->Release(); g_device = nullptr; }

        if (g_presentTarget) MH_RemoveHook(g_presentTarget);
        if (g_resizeBuffersTarget) MH_RemoveHook(g_resizeBuffersTarget);

        g_gameWindow = nullptr;
        g_originalWndProc = nullptr;
        g_originalPresent = nullptr;
        g_originalResizeBuffers = nullptr;
        g_presentTarget = nullptr;
        g_resizeBuffersTarget = nullptr;
        g_rendererReady.store(false);
        g_hookInstalled.store(false);
        g_f1WasDown = false;
        Logger::Write("D3D11 ImGui overlay shutdown");
    }

    bool IsHookInstalled() { return g_hookInstalled.load(); }
    bool IsRendererReady() { return g_rendererReady.load(); }
    bool IsMenuOpen() { return g_menuOpen.load(); }
    void SetMenuOpen(bool open) { SetMenuOpenInternal(open); }
}
