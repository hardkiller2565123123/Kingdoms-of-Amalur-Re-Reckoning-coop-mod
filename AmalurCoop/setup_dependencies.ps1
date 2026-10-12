param(
    [string]$ImGuiVersion = "v1.90.9",
    [string]$MinHookVersion = "v1.3.3"
)

$ErrorActionPreference = "Stop"
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$ProjectDir = Split-Path -Parent $MyInvocation.MyCommand.Path

function Download-File([string]$Uri, [string]$OutputPath)
{
    $Parent = Split-Path -Parent $OutputPath
    New-Item -ItemType Directory -Path $Parent -Force | Out-Null

    if (Test-Path $OutputPath)
    {
        if ((Get-Item $OutputPath).Length -gt 0)
        {
            return
        }
    }

    Write-Host "Downloading $Uri"
    Invoke-WebRequest -UseBasicParsing -Headers @{ "User-Agent" = "AmalurCoop-Build" } -Uri $Uri -OutFile $OutputPath

    if (!(Test-Path $OutputPath) -or (Get-Item $OutputPath).Length -eq 0)
    {
        throw "Download failed or returned an empty file: $Uri"
    }
}

# Dear ImGui: compiled directly into DINPUT8.dll.
$ImGuiDestination = Join-Path $ProjectDir "third_party\imgui"
$ImGuiBase = "https://raw.githubusercontent.com/ocornut/imgui/$ImGuiVersion"
$ImGuiFiles = @(
    "imconfig.h",
    "imgui.h",
    "imgui_internal.h",
    "imgui.cpp",
    "imgui_draw.cpp",
    "imgui_tables.cpp",
    "imgui_widgets.cpp",
    "imstb_rectpack.h",
    "imstb_textedit.h",
    "imstb_truetype.h",
    "LICENSE.txt"
)
$ImGuiBackendFiles = @(
    "imgui_impl_win32.h",
    "imgui_impl_win32.cpp",
    "imgui_impl_dx11.h",
    "imgui_impl_dx11.cpp"
)

foreach ($File in $ImGuiFiles)
{
    Download-File "$ImGuiBase/$File" (Join-Path $ImGuiDestination $File)
}
foreach ($File in $ImGuiBackendFiles)
{
    Download-File "$ImGuiBase/backends/$File" (Join-Path $ImGuiDestination "backends\$File")
}

# MinHook: compile the x86 implementation into this project instead of linking
# the old import library that expected a separate MinHook.x86.dll.
$MinHookDestination = Join-Path $ProjectDir "third_party\minhook"
$MinHookBase = "https://raw.githubusercontent.com/TsudaKageyu/minhook/$MinHookVersion"
$MinHookFiles = @(
    "include/MinHook.h",
    "src/buffer.c",
    "src/buffer.h",
    "src/hook.c",
    "src/trampoline.c",
    "src/trampoline.h",
    "src/hde/hde32.c",
    "src/hde/hde32.h",
    "src/hde/pstdint.h",
    "src/hde/table32.h",
    "LICENSE.txt"
)

foreach ($File in $MinHookFiles)
{
    Download-File "$MinHookBase/$File" (Join-Path $MinHookDestination ($File -replace '/', '\'))
}

Write-Host "Dependencies ready: Dear ImGui $ImGuiVersion and MinHook $MinHookVersion"
