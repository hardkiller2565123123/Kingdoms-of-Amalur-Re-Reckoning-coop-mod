# AmalurCoop

<p align="center">
  <img src="https://raw.githubusercontent.com/hardkiller2565123123/images/main/Screenshot%20%281644%29.png" alt="AmalurCoop Multiplayer Screenshot" width="100%">
</p>

<p align="center">
  <strong>Open-source experimental multiplayer prototype for <em>Kingdoms of Amalur: Re-Reckoning</em>.</strong>
</p>

---

## Current Status

AmalurCoop is an early multiplayer research project focused on bringing cooperative play to **Kingdoms of Amalur: Re-Reckoning** while keeping the original gameplay experience intact.

### Currently Working

- ✅ Host/client UDP networking
- ✅ Two connected game instances
- ✅ Local player transform replication
- ✅ Visible remote player proxy
- ✅ Real-time position synchronization
- ✅ F1 ImGui debug and networking interface
- ✅ Basic connection diagnostics

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

## Repository Layout

```text
AmalurCoop.slnx
│
├── AmalurCoop/
│   Main DINPUT8 proxy and multiplayer prototype
│
└── steam_api/
    Steam API proxy project
```

---

## Building

1. Open **AmalurCoop.slnx** in Visual Studio 2022.
2. Select the **Win32** platform.
3. Build the **Release** configuration.
4. Copy the generated DLL and required files into the game directory.
5. Launch the game and press **F1** to open the multiplayer debug menu.

---

## Project Goals

The goal of AmalurCoop is to create a stable private cooperative experience while preserving the original game.

Planned features include:

- Full player synchronization
- Proper player models
- Animation replication
- Equipment synchronization
- Shared combat
- Enemy synchronization
- Quest progression
- Inventory sharing
- World event synchronization
- Area transitions
- Improved networking
- Better debugging tools
- LAN and Internet support

---

## Requirements

- Visual Studio 2022
- A legitimate copy of **Kingdoms of Amalur: Re-Reckoning**

This repository does **not** include any game assets or copyrighted files.

---

## Scope

This project is intended for:

- Multiplayer research
- Reverse engineering
- Modding
- Preservation
- Private cooperative play

It does **not** support cheating in official online services. Every player must own a legitimate copy of the game.

---

## Contributing

Contributions are welcome.

Whether it's reverse engineering, networking, synchronization, bug fixes, or documentation, any help is appreciated as the project continues to grow.

---

## License

Licensed under the **MIT License**.

See the **LICENSE** file for details.
