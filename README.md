# AmalurCoop

<p align="center">
  <img src="https://www.dsogaming.com/wp-content/uploads/2020/09/KOARR_Gorhart.jpg" alt="AmalurCoop Multiplayer Screenshot" width="100%">
</p>

<p align="center">
  <strong>Open-source experimental multiplayer prototype for <em>Kingdoms of Amalur: Re-Reckoning</em>.</strong>
</p>

---

## Current Status

AmalurCoop is an early multiplayer research project focused on bringing cooperative play to **Kingdoms of Amalur: Re-Reckoning** while keeping the original gameplay experience intact.

### Currently Working

- ✅ Host/client UDP networking
- ✅ Local player transform replication
- ✅ Visible remote player proxy
- ✅ Real-time position synchronization
- ✅ F1 ImGui debug and networking interface
- ✅ Basic connection diagnostics
- ✅ Both player walking around. (no animation)

The screenshot above shows two connected game instances with a synchronized remote player prototype.

---

## Work In Progress

The current proxy actor is only a placeholder. The following systems are still under development:

- Character appearance
- Materials and shaders
- Animation synchronization
- Equipment replication
- Weapon replication
- Combat synchronization
- Health and damage replication
- Enemy synchronization
- Quest synchronization
- Inventory synchronization
- Dialogue synchronization
- World transitions
- Death and respawning
- Stable multiplayer sessions

---

## Building

1. Open **AmalurCoop.slnx** in Visual Studio 2022.
2. Select the **Win32** platform.
3. Build the **Release** configuration.
4. Copy the generated DLL and required files into the game directory.
5. Launch the game and press **F1** to open the multiplayer debug menu.

---

## Requirements

- Visual Studio 2022
- A legitimate copy of **Kingdoms of Amalur: Re-Reckoning**

This repository does **not** include any game assets or copyrighted files.

---

