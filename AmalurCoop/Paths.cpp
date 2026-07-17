#include "Paths.h"
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

namespace Paths
{
    std::string GetGameFolder()
    {
        char path[MAX_PATH]{};
        GetModuleFileNameA(nullptr, path, MAX_PATH);

        std::string fullPath = path;
        size_t slash = fullPath.find_last_of("\\/");

        if (slash == std::string::npos)
            return ".";

        return fullPath.substr(0, slash);
    }

    std::string GetModFolder()
    {
        return GetGameFolder() + "\\AmalurCoop";
    }

    std::string GetLogsFolder()
    {
        return GetModFolder() + "\\logs";
    }

    std::string GetConfigPath()
    {
        return GetModFolder() + "\\config.ini";
    }
}