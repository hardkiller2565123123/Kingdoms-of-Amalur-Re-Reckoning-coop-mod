# Changelog

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
