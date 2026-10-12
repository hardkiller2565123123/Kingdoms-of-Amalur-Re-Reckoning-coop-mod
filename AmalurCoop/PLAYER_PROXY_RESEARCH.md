> Superseded appearance mapping: see [CHARACTER_SYNC_MAPPING.md](CHARACTER_SYNC_MAPPING.md). C7 is Rendering, C8 Stats, C9 Inventory, C15 Physics, C42 Motion. Historical C9/C15/C42 appearance-copy claims below are not current implementation.

# Player Proxy Research

This note records the verified player-proxy path used by the current AmalurCoop build.

## Why FC6220 was removed

`sub_FC6220` creates a valid actor and copies transform information, but its game callers use it for limited actor/entity duplication and stack-style splitting. It is not the complete player-avatar creation path. That matches the old proxy behavior: the actor could move, but visual/player state was incomplete.

## Verified player creation path

The game creates the primary player actor through this chain:

```text
sub_FBC830
  -> sub_DD15C0
    -> sub_FC6170
      -> sub_F86E20
        -> sub_FD66F0
          -> sub_FD5ED0
```

The `DD15C0` player path passes **two distinct handles** by address into `FC6170`: a persistent actor handle resolved through `playerManager -> B4C080 -> runtimeRoot+0x1754/A70B80`, and the player actor-definition handle from the live actor at `+0xEC`. It also passes two definition-owned descriptors. Those descriptors are resolved from the definition record at `[definition + 0x33C] + 0x04` and `+0x14`.

AmalurCoop now mirrors that spawn portion without binding the new actor into the local player's `+0x1EC` slot.

The verified `FC6170` argument order used by the proxy spawner is:

```text
ECX = runtime root
1    = out actor handle*
2    = persistent actor handle*
3    = actor definition handle*
4    = player descriptor A
5    = player descriptor B
6-9  = zero for this player path
```

## Remote player post-setup and Player ownership

After `FC6170`, the real `DD15C0` player path performs this order:

```text
Component 6 -> F6A660
Component 7 -> vtable +0x34 init
Player context -> vtable +0x3C(new actor handle)
Player context -> vtable +0x20(1)
resolve playerContext +0x1EC through the object registry
resolved actor runtime -> F7A270(playerContext +0x2BC)
```

The raw assembly settles an earlier ambiguity: `sub_F7A270` receives the **spawned actor runtime object in ECX**. It is not called with the Player context. The one stack argument is the Player setup value read from `playerContext + 0x2BC`.

`PlayerContextRuntime` therefore does two separate things:

- if `Player_Manager` already exposes an unused non-local Player context, bind the remote actor through vtable `+0x3C`, activate that context through `+0x20`, then apply `F7A270`;
- if no safe context exists, do **not** fabricate or overwrite a Player object. The proxy still receives the verified actor-runtime `F7A270` stage.

This keeps Player ownership conservative while reproducing more of the real player initialization path.

## Animation/controller path

Component 7 `+0x9C` is passed by the game to `sub_C8AD60`. That routine resolves the actor-state entry from the manager table at `global_19FDF54 + 0xC4`. The motion controller is embedded at actor-state entry `+0x44`.

Motion replication uses the verified actor-state handle directly. The older full actor-state/runtime-object fallback scan is disabled so the `D1D840` hot path stays bounded and does not reintroduce the periodic freeze. `sub_D1D840` remains the replay entry; its six stack arguments are unchanged.

## Current appearance limitation

The proxy is created from the local player's definition/profile data on each machine, but runtime testing still shows a bright purple/magenta actor. That means the player-style actor exists, while at least one visual/material finalization step from the real player path is still missing.

The complete dump set supplied earlier covers the executable `.text` range used by this build, including the player spawn, registration, component, rendering, and animation paths. No F5 pseudocode or repeat export of these functions is required.

Recovered/verified from that dump set:

- `sub_FC6170` is the player-style spawn wrapper used with the runtime registry in `ECX`.
- `sub_F86E20` performs core actor construction/reinitialization, invokes the actor-class virtual initializer, initializes Component 6, calls `FD66F0`, and finishes with `F323A0`.
- `sub_F6A660` is a Component 6 post-spawn transform/update path and consults Component 15 before falling back to `F5DD50`.
- `sub_F5DFA0` is part of that Component 6 update path; it is not a standalone material finalizer.
- `sub_F7A270` is called on the resolved spawned actor runtime object after the Player bind/activate stage; its one stack argument is the Player setup value from `+0x2BC`.
- `F7A270` reads actor-runtime fields at `+0x30/+0x38`, resolves another managed object through `FF7010`, obtains Component 1, and forwards it to `F77200`. The current proxy replays this verified actor-runtime stage.
- Network protocol v4 sends only compact player snapshot data (definition handle, setup value, component flags, revision). Received definition handles are validated through the local definition manager before use.
- The game contains `Equipment_Manager`, `NY_Cusomtization_Manager`, `NY_Dye_Manager`, `NY_Tint_Manager`, `NY_Rig_Swap_Manager`, `NY_Creature_Appearance_Manager`, and RTTI for `DS_Component_Equipment`, but the native equipment/customization serialization signatures are not proven yet, so they are not called blindly.

The next useful evidence is an in-game host/client test of protocol v4 plus the new F1 diagnostics: engine Player slot, Player-context bind state, `F7A270` result, Component 7 actor-state handle/entry, and controller state. Component 15 remains labeled as a candidate visual-related component because the dump proves it participates in Component 6 update behavior, not that it is the complete appearance system.

True per-peer equipment/customization still needs its own proven native serialization/apply path; that work is intentionally separate from the now-corrected Player ownership and actor-state initialization path.
