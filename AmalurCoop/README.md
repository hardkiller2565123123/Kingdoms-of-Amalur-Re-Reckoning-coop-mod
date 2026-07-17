# AmalurCoop

AmalurCoop is an experimental open-source cooperative multiplayer prototype for the original 32-bit PC release of **Kingdoms of Amalur: Reckoning**.

The project currently proves that two running game clients can:

- host and join a lightweight UDP session;
- exchange live player transforms;
- create a visible remote humanoid proxy actor;
- update that proxy from the other player's position data; and
- use an in-game F1 debug and research interface.

The remote actor is still an early prototype. It may appear with placeholder magenta materials, does not yet reproduce the other player's appearance, and does not yet synchronize animation, equipment, combat, quests, AI, or world transitions.

## Project status

**Proof of concept — not a finished multiplayer mod.**

Current milestone: two connected clients can see a network-driven proxy actor in the same loaded area.

Next research targets include:

- correct actor appearance and material initialization;
- safe proxy AI/controller suppression;
- facing and orientation replication;
- locomotion and animation state replication;
- equipment and weapon replication;
- spawn/despawn handling across world transitions; and
- synchronized combat and interaction systems.

## Requirements

- A legally owned PC copy of Kingdoms of Amalur: Reckoning
- Windows
- Visual Studio 2022 with the Desktop development with C++ workload
- Windows 10/11 SDK
- PowerShell and an internet connection for the dependency setup script

The project targets **Win32/x86**. Do not build it as x64.

## Building

1. Clone or download this repository.
2. Run `setup_dependencies.ps1`, or simply build once and allow the project to run it automatically.
3. Open `AmalurCoop.sln` in Visual Studio 2022.
4. Select `Release | Win32`.
5. Build the `DINPUT8` project.
6. Copy `build/Release/DINPUT8.dll` beside the game executable.

Dear ImGui and MinHook source files are downloaded into `third_party/` and compiled directly into the DLL. They are intentionally not committed to this repository.

## Basic two-instance test

1. Start both game instances and load both characters into the same area.
2. Press **F1** in the first instance and host a session.
3. Press **F1** in the second instance and join `127.0.0.1` using the same UDP port.
4. After transform packets begin flowing, each client should create a visible proxy actor for the other player.

For testing across separate computers, use the host's reachable LAN/VPN address and allow the selected UDP port through the firewall. The default port is `7777`.

## Known limitations

- The proxy currently uses an incomplete direct clone path and can render bright magenta because appearance/material initialization is not yet replicated.
- The proxy is position-driven and may slide, remain in an idle pose, or be affected by local game systems.
- Both players should remain in the same loaded area for the current prototype.
- The code contains version-specific reverse-engineered addresses for the tested executable.
- Crashes, save corruption, broken quests, or other unintended behavior are possible. Back up save data before testing.

## Scope and rules

This project is intended for private cooperative experimentation, reverse-engineering research, accessibility, preservation, and modding of legitimately owned game copies.

It does not include game assets, executable files, copyrighted game data, authentication bypasses, or piracy support. Do not use it to interfere with official online services or other players.

## Contributing

Useful contributions include executable-version validation, safer actor creation, appearance initialization research, animation mapping, packet validation, crash fixes, and clear reproduction logs. See `CONTRIBUTING.md` before opening a pull request.

## License

AmalurCoop source code is released under the MIT License. Third-party projects retain their own licenses. See `LICENSE` and `THIRD_PARTY_NOTICES.md`.
