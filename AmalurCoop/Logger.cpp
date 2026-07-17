#include "Logger.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>

namespace
{
    std::mutex g_mutex;
    bool g_initialized = false;
    bool g_ownsConsole = false;
    bool g_fileEnabled = false;
    HWND g_consoleWindow = nullptr;
    HANDLE g_consoleOutput = INVALID_HANDLE_VALUE;
    HANDLE g_logFile = INVALID_HANDLE_VALUE;
    WORD g_defaultAttributes = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;

    const char* LevelName(Logger::Level level)
    {
        switch (level)
        {
        case Logger::Level::Info: return "INFO";
        case Logger::Level::Success: return "SUCCESS";
        case Logger::Level::Warning: return "WARNING";
        case Logger::Level::Error: return "ERROR";
        case Logger::Level::Debug: return "DEBUG";
        default: return "LOG";
        }
    }

    WORD LevelColor(Logger::Level level)
    {
        switch (level)
        {
        case Logger::Level::Success: return FOREGROUND_GREEN | FOREGROUND_INTENSITY;
        case Logger::Level::Warning: return FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY;
        case Logger::Level::Error: return FOREGROUND_RED | FOREGROUND_INTENSITY;
        case Logger::Level::Debug: return FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY;
        default: return g_defaultAttributes;
        }
    }

    std::wstring LogPath()
    {
        wchar_t path[MAX_PATH]{};
        const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
        if (!length || length >= MAX_PATH)
            return L"AmalurCoop.log";

        std::wstring result(path, length);
        const auto slash = result.find_last_of(L"\\/");
        if (slash == std::wstring::npos)
            result.clear();
        else
            result.resize(slash + 1);
        result += L"AmalurCoop.log";
        return result;
    }

    void ConfigureConsole(HWND window)
    {
        if (!window)
            return;

        // Prevent closing the console window from terminating the host game.
        if (HMENU menu = GetSystemMenu(window, FALSE))
        {
            DeleteMenu(menu, SC_CLOSE, MF_BYCOMMAND);
            DrawMenuBar(window);
        }

        HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
        if (input && input != INVALID_HANDLE_VALUE)
        {
            DWORD mode = 0;
            if (GetConsoleMode(input, &mode))
            {
                // Quick Edit can pause the entire process while text is selected.
                mode |= ENABLE_EXTENDED_FLAGS;
                mode &= ~ENABLE_QUICK_EDIT_MODE;
                SetConsoleMode(input, mode);
            }
        }
    }

    void RawWrite(HANDLE handle, const char* text, DWORD length)
    {
        if (!handle || handle == INVALID_HANDLE_VALUE || !text || !length)
            return;

        DWORD written = 0;
        if (GetFileType(handle) == FILE_TYPE_CHAR &&
            WriteConsoleA(handle, text, length, &written, nullptr))
        {
            return;
        }

        WriteFile(handle, text, length, &written, nullptr);
    }

    void WriteInternal(Logger::Level level, const char* message)
    {
        if (!message)
            return;

        std::lock_guard<std::mutex> lock(g_mutex);

        SYSTEMTIME time{};
        GetLocalTime(&time);

        char prefix[128]{};
        _snprintf_s(
            prefix,
            _countof(prefix),
            _TRUNCATE,
            "[%02u:%02u:%02u.%03u] [%s] [T%lu] ",
            static_cast<unsigned>(time.wHour),
            static_cast<unsigned>(time.wMinute),
            static_cast<unsigned>(time.wSecond),
            static_cast<unsigned>(time.wMilliseconds),
            LevelName(level),
            static_cast<unsigned long>(GetCurrentThreadId()));

        std::string line(prefix);
        line += message;
        if (line.empty() || line.back() != '\n')
            line += "\r\n";

        OutputDebugStringA(line.c_str());

        if (g_consoleOutput != INVALID_HANDLE_VALUE)
        {
            SetConsoleTextAttribute(g_consoleOutput, LevelColor(level));
            RawWrite(g_consoleOutput, line.c_str(), static_cast<DWORD>(line.size()));
            SetConsoleTextAttribute(g_consoleOutput, g_defaultAttributes);
        }

        if (g_fileEnabled && g_logFile != INVALID_HANDLE_VALUE)
        {
            RawWrite(g_logFile, line.c_str(), static_cast<DWORD>(line.size()));
            FlushFileBuffers(g_logFile);
        }
    }

    void WriteFormatted(Logger::Level level, const char* format, va_list args)
    {
        char buffer[4096]{};
        _vsnprintf_s(buffer, _countof(buffer), _TRUNCATE, format, args);
        WriteInternal(level, buffer);
    }
}

namespace Logger
{
    bool Initialize(const wchar_t* title, bool enableFileLogging)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_initialized)
            return true;

        if (!GetConsoleWindow())
        {
            if (AllocConsole())
                g_ownsConsole = true;
        }

        g_consoleWindow = GetConsoleWindow();
        if (g_consoleWindow)
        {
            if (title && *title)
                SetConsoleTitleW(title);

            SetConsoleOutputCP(CP_UTF8);
            SetConsoleCP(CP_UTF8);
            ConfigureConsole(g_consoleWindow);

            g_consoleOutput = GetStdHandle(STD_OUTPUT_HANDLE);
            CONSOLE_SCREEN_BUFFER_INFO info{};
            if (g_consoleOutput != INVALID_HANDLE_VALUE &&
                GetConsoleScreenBufferInfo(g_consoleOutput, &info))
            {
                g_defaultAttributes = info.wAttributes;
            }
        }

        g_fileEnabled = enableFileLogging;
        if (g_fileEnabled)
        {
            const std::wstring path = LogPath();
            g_logFile = CreateFileW(
                path.c_str(),
                GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE,
                nullptr,
                CREATE_ALWAYS,
                FILE_ATTRIBUTE_NORMAL,
                nullptr);

            if (g_logFile == INVALID_HANDLE_VALUE)
                g_fileEnabled = false;
        }

        g_initialized = g_consoleWindow != nullptr || g_fileEnabled;
        return g_initialized;
    }

    void Shutdown()
    {
        if (!g_initialized)
            return;

        Write(Level::Debug, "Logger shutting down");
        std::lock_guard<std::mutex> lock(g_mutex);

        if (g_logFile != INVALID_HANDLE_VALUE)
        {
            FlushFileBuffers(g_logFile);
            CloseHandle(g_logFile);
            g_logFile = INVALID_HANDLE_VALUE;
        }

        // Intentionally do not call FreeConsole. Detaching a live console while
        // other game/runtime threads may still hold its handles is unsafe.
        if (g_consoleWindow)
            ShowWindow(g_consoleWindow, SW_HIDE);

        g_consoleOutput = INVALID_HANDLE_VALUE;
        g_fileEnabled = false;
        g_initialized = false;
    }

    void Write(const char* message) { WriteInternal(Level::Info, message); }
    void Write(const std::string& message) { WriteInternal(Level::Info, message.c_str()); }
    void Write(Level level, const char* message) { WriteInternal(level, message); }
    void Write(Level level, const std::string& message) { WriteInternal(level, message.c_str()); }

    void WriteFormat(const char* format, ...)
    {
        va_list args;
        va_start(args, format);
        WriteFormatted(Level::Info, format, args);
        va_end(args);
    }

    void WriteFormat(Level level, const char* format, ...)
    {
        va_list args;
        va_start(args, format);
        WriteFormatted(level, format, args);
        va_end(args);
    }

    void ShowConsole(bool show)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_consoleWindow)
            ShowWindow(g_consoleWindow, show ? SW_SHOW : SW_HIDE);
    }

    void ToggleConsole() { ShowConsole(!IsConsoleVisible()); }

    bool IsConsoleVisible()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        return g_consoleWindow && IsWindowVisible(g_consoleWindow);
    }

    bool IsInitialized()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        return g_initialized;
    }
}
