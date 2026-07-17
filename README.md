# AmalurCoop

Open-source experimental multiplayer prototype for **Kingdoms of Amalur: Re-Reckoning**.

## Current status

The prototype currently supports:

- Host/client UDP sessions
- Two connected game instances
- Local player transform replication
- Direct creation of a visible remote proxy actor
- Remote proxy position updates in the same loaded area
- F1 ImGui diagnostics and session controls

The visible proxy is still an early research actor. Its appearance, materials, animation, equipment, AI suppression, combat, quests, and world transitions are not complete.

## Repository layout

```text
AmalurCoop.slnx
AmalurCoop/       Main DINPUT8 proxy and multiplayer prototype
steam_api/        Auxiliary Steam API proxy project
```

## Build

1. Open `AmalurCoop.slnx` in Visual Studio 2022.
2. Select **Win32** for the game-compatible build.
3. Build the `AmalurCoop` project in Release mode.
4. Copy the resulting proxy DLL and required dependencies to the game directory for testing.

This project does not include game files. You must own the game.

## Scope

This project is intended for offline/private cooperative research and modding. It does not support cheating in official online services.

## License

MIT. See [LICENSE](LICENSE).
