# AmalurCoop

<p align="center">
  <img src="https://www.dsogaming.com/wp-content/uploads/2020/09/KOARR_Gorhart.jpg" alt="Kingdoms of Amalur: Re-Reckoning" width="100%">
</p>

<p align="center">
  <strong>Open-source experimental multiplayer prototype for <em>Kingdoms of Amalur: Re-Reckoning</em>.</strong>
</p>

## Current Status

AmalurCoop is an early multiplayer research project focused on bringing cooperative play to Kingdoms of Amalur: Re-Reckoning while keeping the original gameplay experience intact.

**Development has resumed! New updates are in progress.**

## Currently Working

- ✅ Host/client UDP networking
- ✅ In-game multiplayer menu with Host and Join options
- ✅ Save and character selection when hosting or joining
- ✅ Basic remote player spawning and position synchronization
- ✅ Basic character appearance, clothing, and equipment replication
- ✅ Partial animation synchronization
- ✅ Basic connection diagnostics

Remote player visibility and animation playback are still inconsistent. This is an experimental prototype, and bugs are expected.

## Work In Progress

The following systems are still under development:

- Reliable remote player visibility
- Character appearance and equipment accuracy
- Materials and shaders
- Walking, running, and combat animation synchronization
- Smooth movement and animation transitions
- Weapon replication
- Combat synchronization
- Health and damage replication
- Enemy synchronization
- Quest synchronization
- Inventory synchronization
- Dialogue synchronization
- World transitions
- Death and respawning
- Performance and stable multiplayer sessions

## Building and Playing

1. Open `AmalurCoop.slnx` in Visual Studio 2022.
2. Select the Win32 platform.
3. Build the Release configuration.
4. Copy the generated DLL and required files into the game directory.
5. Launch the game and select Multiplayer from the main menu.
6. Choose Host or Join, then select your save.

Both players must use the same mod version. The old F1 ImGui interface has been removed.

## Requirements

- Visual Studio 2022
- A legitimate copy of Kingdoms of Amalur: Re-Reckoning

This repository does not include any game assets or copyrighted files.
