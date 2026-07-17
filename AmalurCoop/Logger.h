#pragma once

#include <string>

namespace Logger
{
    enum class Level
    {
        Info,
        Success,
        Warning,
        Error,
        Debug
    };

    bool Initialize(
        const wchar_t* consoleTitle = L"AmalurCoop Debug Console",
        bool enableFileLogging = true);

    void Shutdown();

    void Write(const char* message);
    void Write(const std::string& message);
    void Write(Level level, const char* message);
    void Write(Level level, const std::string& message);
    void WriteFormat(const char* format, ...);
    void WriteFormat(Level level, const char* format, ...);

    void ShowConsole(bool show);
    void ToggleConsole();
    bool IsConsoleVisible();
    bool IsInitialized();
}
