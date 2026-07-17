#pragma once

namespace ImGuiOverlay
{
    bool Initialize();
    void Shutdown();

    bool IsHookInstalled();
    bool IsRendererReady();
    bool IsMenuOpen();
    void SetMenuOpen(bool open);
}
