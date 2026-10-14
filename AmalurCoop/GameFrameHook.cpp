#include "GameFrameHook.h"
#include "WindowFocusPolicy.h"
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include "Config.h"
#include "EngineRuntime.h"
#include <cstring>
#include "HookManager.h"
#include "Logger.h"
#include "MinHook.h"
#include "NetworkManager.h"
#include "PositionTracker.h"
#include "RemotePlayerManager.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
namespace
{
    using PresentFn = HRESULT(WINAPI*)(IDXGISwapChain*, UINT, UINT);
    PresentFn g_originalPresent = nullptr;
    using RenderTransformFn = void(__thiscall*)(void*, const void*);
    RenderTransformFn g_originalRenderTransform = nullptr;
    void* g_renderTransformTarget = nullptr;
    thread_local bool g_correctingPose = false;

    void __fastcall HookedRenderTransform(void* rendering, void*, const void* matrix)
    {
        if (!g_correctingPose)
        {
            g_correctingPose = true;
            const auto authoritative = RemotePlayerManager::PrepareRenderMatrix(reinterpret_cast<uintptr_t>(rendering));
            g_correctingPose = false;
            if (authoritative) matrix = reinterpret_cast<const void*>(authoritative);
        }
        g_originalRenderTransform(rendering, matrix);
    }

    void* g_presentTarget = nullptr;
    std::atomic<bool> g_hookInstalled = false;
    HWND g_gameWindow = nullptr;
    WNDPROC g_originalWndProc = nullptr;
    std::recursive_mutex g_renderMutex;

    LRESULT CALLBACK HookedWndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (WindowFocusPolicy::IsAutomaticDeactivation(message, wParam))
        {
            ClipCursor(nullptr);
            return DefWindowProcW(window, message, wParam, lParam);
        }
        return g_originalWndProc ? CallWindowProcW(g_originalWndProc, window, message, wParam, lParam)
            : DefWindowProcW(window, message, wParam, lParam);
    }

    void AttachWindow(IDXGISwapChain* swapChain)
    {
        if (g_gameWindow && IsWindow(g_gameWindow)) return;
        DXGI_SWAP_CHAIN_DESC desc{};
        if (FAILED(swapChain->GetDesc(&desc)) || !IsWindow(desc.OutputWindow)) return;
        SetLastError(0);
        auto original = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(desc.OutputWindow,
            GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(HookedWndProc)));
        if (!original && GetLastError() != 0) return;
        g_originalWndProc = original;
        g_gameWindow = desc.OutputWindow;
    }

    HRESULT WINAPI HookedPresent(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags)
    {
        if (flags & DXGI_PRESENT_TEST)
            return g_originalPresent ? g_originalPresent(swapChain, syncInterval, flags) : S_OK;
        std::lock_guard<std::recursive_mutex> lock(g_renderMutex);
        AttachWindow(swapChain);
        using FrameClock = std::chrono::steady_clock;
        static auto lastFrame = FrameClock::now();
        const auto settings = Config::Get();
        const bool foreground = GetForegroundWindow() == g_gameWindow;
        const int fpsLimit = foreground ? settings.ForegroundFPS : settings.BackgroundFPS;
        if (fpsLimit > 0)
        {
            const auto remaining = std::chrono::duration<double>(1.0 / fpsLimit) - (FrameClock::now() - lastFrame);
            const auto waitMs = std::chrono::duration_cast<std::chrono::milliseconds>(remaining).count();
            if (waitMs > 0) Sleep(static_cast<DWORD>(waitMs));
        }
        lastFrame = FrameClock::now();
        const auto workStart = lastFrame;

        // Actor creation touches world/graphics systems and must run from the
        // game's render thread rather than the background network thread.
        const auto beforeTracker = FrameClock::now();
        PositionTracker::Update();
        const auto afterTracker = FrameClock::now();
        NetworkManager::CaptureLocalSnapshot();
        const auto afterSnapshot = FrameClock::now();
        RemotePlayerManager::Update();
        const auto afterRemote = FrameClock::now();
        RemotePlayerManager::PumpGameThread();
        const auto afterPump = FrameClock::now();

        if (!foreground) ClipCursor(nullptr);

        // Exclude our intentional frame cap and the driver's Present wait.
        static double totalWorkMs = 0, peakWorkMs = 0;
        static double trackerMs = 0, snapshotMs = 0, remoteMs = 0, pumpMs = 0;
        trackerMs += std::chrono::duration<double, std::milli>(afterTracker - beforeTracker).count();
        snapshotMs += std::chrono::duration<double, std::milli>(afterSnapshot - afterTracker).count();
        remoteMs += std::chrono::duration<double, std::milli>(afterRemote - afterSnapshot).count();
        pumpMs += std::chrono::duration<double, std::milli>(afterPump - afterRemote).count();
        static unsigned measuredFrames = 0;
        static auto lastReport = FrameClock::now();
        const auto workEnd = FrameClock::now();
        const double workMs = std::chrono::duration<double, std::milli>(workEnd - workStart).count();
        totalWorkMs += workMs; peakWorkMs = (std::max)(peakWorkMs, workMs); ++measuredFrames;
        if (workEnd - lastReport >= std::chrono::seconds(5))
        {
            Logger::WriteFormat(Logger::Level::Debug,
                "Mod frame work avg=%.3fms peak=%.3fms frames=%u focus=%s cap=%d",
                totalWorkMs / measuredFrames, peakWorkMs, measuredFrames, foreground ? "foreground" : "background", fpsLimit);
            Logger::WriteFormat(Logger::Level::Debug,
                "Mod stage averages tracker=%.3fms snapshot=%.3fms remote=%.3fms spawn/visual/motion=%.3fms",
                trackerMs / measuredFrames, snapshotMs / measuredFrames, remoteMs / measuredFrames, pumpMs / measuredFrames);
            trackerMs = snapshotMs = remoteMs = pumpMs = 0;
            totalWorkMs = peakWorkMs = 0; measuredFrames = 0; lastReport = workEnd;
        }
        return g_originalPresent ? g_originalPresent(swapChain, syncInterval, flags) : S_OK;
    }

    bool GetD3D11Methods(void*& present)
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
        }

        if (context) context->Release();
        if (device) device->Release();
        if (swapChain) swapChain->Release();
        DestroyWindow(window);
        UnregisterClassW(className, wc.hInstance);
        return present != nullptr;
    }

}
namespace GameFrameHook
{
    bool Initialize()
    {
        if (g_hookInstalled.load()) return true;
        void* target = nullptr;
        if (!HookManager::IsReady() || !GetD3D11Methods(target)) return false;
        if (MH_CreateHook(target, reinterpret_cast<void*>(&HookedPresent),
            reinterpret_cast<void**>(&g_originalPresent)) != MH_OK) return false;
        if (MH_EnableHook(target) != MH_OK)
        {
            MH_RemoveHook(target);
            g_originalPresent = nullptr;
            return false;
        }
        g_presentTarget = target;
        const auto renderTarget = EngineRuntime::RuntimeAddress(0x00F116B0);
        const unsigned char expected[]{ 0x81, 0xEC, 0x84, 0, 0, 0 };
        unsigned char actual[sizeof(expected)]{};
        if (EngineRuntime::ReadBytes(renderTarget, actual, sizeof(actual)) &&
            !std::memcmp(actual, expected, sizeof(actual)) &&
            MH_CreateHook(reinterpret_cast<void*>(renderTarget), reinterpret_cast<void*>(&HookedRenderTransform),
                reinterpret_cast<void**>(&g_originalRenderTransform)) == MH_OK)
        {
            if (MH_EnableHook(reinterpret_cast<void*>(renderTarget)) == MH_OK)
            {
                g_renderTransformTarget = reinterpret_cast<void*>(renderTarget);
                Logger::Write("Native remote pose correction installed before render transform publication");
            }
            else MH_RemoveHook(reinterpret_cast<void*>(renderTarget));
        }
        if (!g_renderTransformTarget)
            Logger::Write(Logger::Level::Error, "Native remote pose correction unavailable; Present fallback only");
        g_hookInstalled.store(true);
        Logger::Write("D3D11 game frame hook installed");
        return true;
    }
    void Shutdown()
    {
        if (g_renderTransformTarget) MH_DisableHook(g_renderTransformTarget);
        if (g_presentTarget) MH_DisableHook(g_presentTarget);
        std::lock_guard<std::recursive_mutex> lock(g_renderMutex);
        if (IsWindow(g_gameWindow) && g_originalWndProc &&
            GetWindowLongPtrW(g_gameWindow, GWLP_WNDPROC) == reinterpret_cast<LONG_PTR>(HookedWndProc))
            SetWindowLongPtrW(g_gameWindow, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_originalWndProc));
        if (g_renderTransformTarget) MH_RemoveHook(g_renderTransformTarget);
        g_renderTransformTarget = nullptr;
        g_originalRenderTransform = nullptr;
        if (g_presentTarget) MH_RemoveHook(g_presentTarget);
        g_presentTarget = nullptr;
        g_originalPresent = nullptr;
        g_gameWindow = nullptr;
        g_originalWndProc = nullptr;
        g_hookInstalled.store(false);
    }
}
