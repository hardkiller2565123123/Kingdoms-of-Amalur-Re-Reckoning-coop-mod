# Changelog

## 0.2.5-player-context-snapshot — 2026-09-26

- Bumped the UDP protocol to v4 and added `PeerJoined` plus `PlayerSnapshot` packets.
- Clients now create Player 1 (the host) immediately after `Welcome`; the host immediately sends its latest transform/snapshot and the current peer roster to a joining client.
- Existing clients receive a roster event as soon as a new peer joins instead of waiting for later transform traffic.
- Added compact per-player snapshot state: definition handle, player setup value, component-presence flags, and revision. Raw process pointers are never serialized.
- Added stable per-peer engine Player-slot assignment independent of network player IDs.
- Added `PlayerContextRuntime`: it binds/activates a remote actor only when the game's `Player_Manager` already provides a safe non-local Player context and refuses to overwrite an occupied context.
- Corrected the earlier `F7A270` interpretation from the raw `DD15C0` assembly: `ECX` is the spawned actor runtime object after `playerContext+0x1EC` is resolved, while the one stack argument is `playerContext+0x2BC`. Remote proxies now replay that verified actor-runtime stage.
- Preserved the motion-freeze hotfix: no full actor-state/controller scans were reintroduced into the `D1D840` path.
- Added F1 diagnostics for snapshot revision, engine Player slot, Player-context binding, and `F7A270` state.
- Equipment/customization serialization remains disabled until the native equipment/appearance structures and signatures are proven.

## 0.2.4-motion-freeze-hotfix — 2026-09-26

- Removed every full actor-state/controller scan from the `sub_D1D840` motion hook path.
- Removed the unsafe `Local actor-state motion controller inferred` fallback that could adopt an unrelated controller and scan thousands of actor-state entries on the game thread.
- Local controller discovery now happens outside the hook through the verified Component 7 `+0x9C` handle path and is cached for constant-time pointer comparison.
- Moved motion network sends out of the hook into a bounded preallocated queue so the hook no longer waits on the network mutex or allocates memory.
- Limited incoming/outgoing motion processing per frame to prevent packet bursts from stalling rendering.
- Remote controller resolution now uses only Component 7 `+0x9C`; full-table fallback scanning is disabled.

## 0.2.3-full-dump-alignment — 2026-09-26

- Rechecked the project against the previously supplied complete IDA dump instead of treating the latest small paste as the only source.
- Removed speculative actor-state activation calls to `CCC6C0`, `CCC7A0`, and `F57E10`; those were not proven as the player-proxy activation sequence. The verified engine registration path through `FC6170 -> F86E20 -> FD66F0 -> FD5ED0` remains intact.
- Renamed the runtime diagnostic from actor-state "activation" to actor-state "resolved" so the UI reports only what is actually verified.
- Updated research notes: no repeat IDA/F5 export is currently required because the earlier dump set already covers the relevant executable code.

## 0.2.2-assembly-fix — 2026-09-26

> Historical note: the `F7A270` interpretation in this section was superseded by 0.2.5 after the exact `DD15C0` resolve-and-call sequence was recovered.

- Corrected the proxy post-spawn path from the supplied raw IDA assembly.
- Removed the invalid `F7A270(proxyRuntimeObject, setupValue)` call. `F7A270` expects a player-context-owned object layout after the real local-player binding vtable calls, not a generic actor runtime pointer.
- Kept the verified `FC6170` spawn, Component 6 `F6A660`, Component 7 vtable `+0x34` initialization, and actor-state diagnostics.
- Reclassified Component 15 as a candidate visual component until the remaining constructor/finalizer assembly proves its role.
- Documented the exact remaining raw-disassembly targets; F5 pseudocode is not required.

## 0.2.1-cleanup — 2026-09-26

- Removed duplicate legacy project copies, Visual Studio cache files, user files, and old build output.
- Removed the obsolete `AmalurCoopServer.exe` project and moved hosting into `NetworkManager` so the F1 **Host session** button hosts directly inside the game.
- Kept automatic LAN discovery and direct IP joining on the same UDP protocol.
- Added host-side player assignment, packet relay, timeout handling, transforms, motion events, and ping handling.
- Rate-limited failed automatic proxy spawning to prevent repeated actor creation from hammering the game.
- Cached remote motion controllers and throttled the expensive fallback actor-state scan.
- Changed the runtime-record inspector to refresh only when requested instead of rescanning every second.
- Reduced the solution to the active Win32 `DINPUT8` project and removed stale hard-coded build paths.
- Updated project files for Visual Studio 2026 / MSVC v145.

The bright purple/magenta proxy is still an appearance-initialization research issue, not considered solved by this cleanup.

## 0.2.0-research — 2026-09-25

- Replaced remote-player creation through `sub_FC6220` with the verified player-style `sub_FC6170 -> sub_F86E20` spawn path.
- Added player definition/descriptor resolution from the live local player.
- Mirrored the player path's separate persistent actor handle (`B4C080 -> A70B80`) instead of passing zero into `FC6170`.
- Added remote-safe player post-spawn setup through Component 6, Component 7, `sub_F6A660`, and `sub_F7A270`.
- Added actor-state activation using Component 7 `+0x9C` and the verified actor-state manager path.
- Changed motion-controller resolution to use the verified actor-state table index first, with the older memory scan retained as a fallback.
- Added controller diagnostics for queue `+0x14` and values at `+0xA8`, `+0xAC`, and `+0xB4`.
- Added Component 7/15, actor-state, definition, and player-setup diagnostics to the F1 interface.
- Added `EngineRuntime` and `PlayerProxySpawner` modules and documented the verified proxy-spawn research.
- Expanded the Research Lab catalog with the newly identified player, actor-state, and motion functions.
- Restored the active D3D11 ImGui/Present game-thread pump in the Win32 project so queued proxy spawns and motion replay actually execute on the render thread.

The new path intentionally does not bind a remote actor into the real local player object. Per-peer equipment/customization replication is still future work.

## 0.1.0-prototype — 2026-07-17

- Added a Win32 `DINPUT8.dll` proxy loader.
- Added a Direct3D 11 ImGui interface toggled with F1.
- Added UDP host/join session support and transform exchange.
- Added verified local-player actor and Component 6 transform tracking.
- Added direct remote proxy actor creation from the local actor handle.
- Added network-driven proxy position updates through the game's transform setter.
- Added runtime, scanner, research, networking, and debugging panels.
- Published the first GitHub-ready open-source source package.

Known issue: the directly created proxy can render with bright magenta placeholder materials until appearance initialization is fully understood.
