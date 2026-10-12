#include "PlayerLifecycleSync.h"
#include "CharacterVisualRuntime.h"
#include "EngineRuntime.h"
#include "Logger.h"
#include "MotionReplication.h"
#include "NetworkManager.h"
#include "RemotePlayerManager.h"
#include <Windows.h>
#include <atomic>
#include <map>
#include <mutex>

namespace
{
    struct State
    {
        std::uint32_t Applied = 0;
        std::uint32_t Attempted = 0;
        std::uint32_t Handle = 0;
        unsigned Retries = 0;
        ULONGLONG LastAttempt = 0;
    };
    std::atomic<bool> g_ready{ false };
    std::mutex g_mutex;
    std::map<std::uint32_t, State> g_states;
    std::string g_status = "Peer visual synchronization stopped";
}
namespace PlayerLifecycleSync
{
    bool Initialize()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_states.clear(); g_ready = true;
        g_status = "Waiting for peer rendering archives";
        return true;
    }
    void Shutdown()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_ready = false; g_states.clear(); g_status = "Peer visual synchronization stopped";
    }
    void PumpGameThread()
    {
        if (!g_ready) return;
        for (const auto id : RemotePlayerManager::GetActiveProxyPlayerIds())
        {
            const auto diagnostics = RemotePlayerManager::GetProxyDiagnostics(id);
            if (!diagnostics.Connected || !diagnostics.RuntimeObject || diagnostics.Manual) continue;
            std::uint32_t revision = 0;
            std::vector<std::uint8_t> bytes;
            if (!NetworkManager::GetVisualState(id, revision, bytes)) continue;
            const auto now = GetTickCount64();
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                auto& state = g_states[id];
                if (state.Handle != diagnostics.ObjectHandle)
                {
                    state = {}; state.Handle = diagnostics.ObjectHandle;
                }
                if (state.Applied == revision) continue;
                if (state.Attempted != revision)
                {
                    state.Attempted = revision; state.Retries = 0;
                }
                if (state.Retries >= 3 || now - state.LastAttempt < 1000) continue;
                state.LastAttempt = now; ++state.Retries;
            }
            const bool applied = CharacterVisualRuntime::Apply(diagnostics.ObjectHandle,
                diagnostics.RuntimeObject, bytes);
            RemotePlayerManager::RefreshProxyRuntimeState(id);
            MotionReplication::InvalidateRemoteController(id);
            RemotePlayerManager::SetVisualSyncResult(id, revision, applied);
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                if (applied) g_states[id].Applied = revision;
                g_status = "Peer " + std::to_string(id) + (applied ? " rendering archive applied" :
                    " rendering archive pending (asset resolution or native apply failed)");
            }
            Logger::WriteFormat(applied ? Logger::Level::Success : Logger::Level::Warning,
                "Peer visuals player=%u revision=%u bytes=%u applied=%s", id, revision,
                static_cast<unsigned>(bytes.size()), applied ? "yes" : "no");
            break;
        }
    }
    void NotifyProxySpawn(std::uint32_t id)
    {
        std::lock_guard<std::mutex> lock(g_mutex); g_states.erase(id);
    }
    void NotifyProxyRemoved(std::uint32_t id)
    {
        std::lock_guard<std::mutex> lock(g_mutex); g_states.erase(id);
    }
    void RequestFullRefresh()
    {
        std::lock_guard<std::mutex> lock(g_mutex); g_states.clear();
    }
    bool IsReady() { return g_ready; }
    std::string GetStatusText()
    {
        std::lock_guard<std::mutex> lock(g_mutex); return g_status;
    }
}
