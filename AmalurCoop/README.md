# AmalurCoop

AmalurCoop is an experimental open-source cooperative multiplayer prototype for the tested 32-bit PC build of **Kingdoms of Amalur: Re-Reckoning**.

The current rendering/material mapping and validation limits are documented in [CHARACTER_SYNC_MAPPING.md](CHARACTER_SYNC_MAPPING.md).

The project currently supports:

- native main-menu Multiplayer -> Host / Join controls, plus the F1 diagnostic interface;
- direct UDP sessions between game instances;
- protocol-v7 player roster and snapshot replication so the host and joining players are announced immediately;
- live player position and rotation replication;
- revisioned peer rendering-tree and material snapshots with chunk recovery;
- player-style remote proxy creation through the verified `FC6170 -> F86E20` path;
- the verified `F7A270` player actor-state stage after spawn;
- safe binding to an existing non-local engine `Player` context when the game's `Player_Manager` exposes one;
- remote position updates and experimental native motion-event replay; and
- an in-game F1 research and diagnostics interface.

The remote player is still a research prototype. Appearance/material initialization, equipment, combat, quests, AI behavior, interactions, and world transitions are not complete.

## Project status

**Proof of concept — not a finished multiplayer mod.**

The current build focuses on keeping the multiplayer path stable enough for reverse-engineering. Automatic proxy spawning is rate-limited, animation replication keeps the no-scan hot path from the motion-freeze fix, and the runtime-record inspector only refreshes when requested from the F1 interface.

## Requirements

- A legally owned PC copy of Kingdoms of Amalur: Re-Reckoning
- Windows
- Visual Studio 2026 with Desktop development with C++
- MSVC v145 toolset
- Windows 10/11 SDK

The game-compatible build is **Win32/x86**. Do not build this project as x64.

## Building

1. Open `AmalurCoop.sln`.
2. Select `Release | Win32`.
3. Build the `DINPUT8` project.
4. Copy `build/Release/DINPUT8.dll` beside the game executable.

Dear ImGui and MinHook source are included under `third_party/`. If those files are ever removed, `setup_dependencies.ps1` can restore the expected versions.

## First proxy test

Load a save, press **F1**, and use **Spawn player-style test proxy near local player**.

Check the proxy diagnostics for:

- object handle;
- persistent actor handle;
- definition handle;
- Component 6 / transform;
- Component 7 / rendering;
- Component 8 / material customization and Component 15 / physics;
- network snapshot revision;
- assigned engine Player slot and Player-context binding state;
- verified `F7A270` additional player-state result;
- actor-state handle and entry; and
- motion-controller diagnostics.

A manual local test proxy does not receive a peer appearance archive. For peer clothing/material validation use a network peer and inspect its visual revision and apply status. Purple/magenta rendering indicates a material/resource problem; it does not establish that the appearance path succeeded.

## Two-instance test

1. Build the same updated Release Win32 source for both instances, then restart both games.
2. In the first game's main menu, select **Multiplayer -> Host**.
3. In the second, select **Multiplayer -> Join**. This uses the configured address/port; `auto` enables LAN discovery. For an explicit two-instance address, configure `127.0.0.1` using the INI/F1 settings.
4. Return through the normal menu and load both saves into the same area. Hosting/joining does not automatically load a save.
5. Protocol v7 announces the host and peer roster. The host runs inside DINPUT8.dll; no separate server executable is needed.

This source candidate has not been built, deployed, or tested in live gameplay. See [research/NATIVE_MENU_PERFORMANCE.md](research/NATIVE_MENU_PERFORMANCE.md) for the source-only checks and diagnostic logs.

For separate computers, join the host's LAN/VPN address. For internet testing, manually forward the selected UDP port and allow the game/DINPUT8 through the firewall. The default port is `7777`.

## Known limitations

- Rendering and material archive paths are mapped and implemented, but live two-instance visual correctness has not been verified.
- Sender runtime handles are diagnostic metadata only. Peer appearance uses stable asset keys and receiver-owned native objects.
- Mesh trees, texture keys and packed customization colors are transferred. Equipment gameplay effects and full animation state are not synchronized.
- If the game does not expose a spare non-local `Player` context, AmalurCoop does not fabricate one; it still applies the verified actor-runtime `F7A270` stage.
- Motion capture/replay uses the shared native clip queue `D1D5D0`, stable rig/catalog/clip keys, selected variants, blend weights, and up to eight tracks. Retries retain animation serials to avoid restarting a playing clip. Both instances must use protocol v7. Live animation behavior remains unverified.
- Both players should remain in the same loaded area.
- The code contains version-specific reverse-engineered addresses for the tested executable.
- Spawned research actors are still game-managed; stopping proxy control does not yet destroy them safely.
- Back up save data before testing.

## Scope

This project is intended for private cooperative experimentation, reverse-engineering research, accessibility, preservation, and modding of legitimately owned copies.

It does not include game assets, executable files, copyrighted game data, authentication bypasses, or piracy support. Do not use it to interfere with official online services or other players.

## License

AmalurCoop source code is released under the MIT License. Third-party projects retain their own licenses. See `LICENSE` and `THIRD_PARTY_NOTICES.md`.
