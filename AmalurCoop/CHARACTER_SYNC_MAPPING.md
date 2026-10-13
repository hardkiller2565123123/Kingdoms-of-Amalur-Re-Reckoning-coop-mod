> Current candidate: source-only protocol v7 changes; no build or deployment performed. Build/test and installed-DLL records below are historical, not validation of this candidate.

> Crash-fix update: F3DBD0 consumes one unused 32-bit stack argument (RET 4). Its old zero-argument call was wrong and is now corrected. Native stack regression and /RTC1 wrapper tests pass. See research/STACK_ABI_FIX.md; the installation/hash information below describes the earlier candidate.

# Character synchronization mapping and build report

Source executable: `koa.exe`, 32-bit image base `0x00400000`.
SHA-256: `16a400f6e8fc10dbe446a9e57717de9fbe975ef17c004f4ca405e2e4e09cb314`.
Addresses below are preferred-image VAs; code relocates them against the running image base.

## Status and scope

The full 43-entry component factory is mapped by its branch table and native RTTI. Rendering-tree serialization, material customization, transform writes, render-controller lookup, and motion-request replay have been traced for this executable. This is not a complete map of every executable function, gameplay system, or every character asset. The build is a research candidate; two live instances have not been run in this session.

The requested outcome�both characters visibly animated with their own complete clothing�still requires live confirmation. Native archive emulation establishes the archive ABI and independent render-handle creation, not GPU output or complete engine integration.

## Corrections to older research

Component 7 is PartRendering, 8 is PartStats, 9 is PartInventory, 15 is PartPhysics, and 42 is PartMotion. Copying 9/15/42 did not reproduce the other player's clothing. That copying path has been removed. `F91770` is not an appearance finalizer; the previous speculative hooks on it and the Script change path are removed from appearance synchronization. `F4DF90` is Inventory serialization, not Stats.

## Implemented character path

1. Resolve the exact local runtime actor; do not select an arbitrary registry entry when lookup fails.
2. On the render frame callback, serialize C7 using `F11950`. Rendering uses `+9C` as a render-actor handle, resolved in manager `19FDF54`, table `+C4`, count `+C8`.
3. Validate the recursive rendering archive, retain stable model/animation asset keys, canonicalize world transforms, and remove transient motion-controller queues. Limits: 64 KiB, 64 nodes, depth 16, 32 children per node, 128 overrides. The native writer has a fixed buffer with backpatch padding; overfull output is rejected.
4. Append three texture keys and ten packed ARGB colors from C8. No process pointers or sender texture handles are sent.
5. Transfer revisioned snapshots in 960-byte chunks over protocol 7. The host validates, caches, acknowledges and relays completed snapshots; unacknowledged revisions retry. New peers receive cached appearances.
6. Spawn a destination-owned player-style proxy from the local verified template. Sender actor-definition/setup handles are diagnostic metadata only; they are not used as local engine identities.
7. Preflight model, override, animation and texture identities. Registered model/texture resources are loaded through `A71E80(...,1)` and fallback placeholders rejected. Asset loading may cause a one-time frame stall; missing/unregistered assets fail visibly in diagnostics instead of claiming success.
8. Apply C7 through `F1DA50`, then native post-load `F757C0` and transform update `F3DBD0`. The receiver creates its own render tree. Texture replacements retain/release receiver resource handles through manager vtable `+A4/+A8`, then reapply the Stats material listener. No gameplay Stats archive is imported.
9. Replicate position and normalized quaternion separately. C6 position is `+24`, rotation `+34`; `F3BCF0` rebuilds derived transform data after rotation. Repeated movement does not change the normalized visual snapshot.
10. Resolve the current remote motion controller from its C7 render handle plus actor offset `+44`. Capture/replay the shared `D1D5D0` clip enqueue path (five stack arguments, `RET 14h`), including selected variants, weights and up to eight tracks. Stable category-5 catalog, category-7 rig and category-8 clip keys replace sender handles. Receiver catalogs must belong to its model. Latest-state retries retain per-track animation serials; caches are invalidated after appearance replacement. See `research/ANIMATION_REQUEST_MAPPING.md`.

Native access is moved out of the network worker into the frame callback. This serializes this mod's capture, proxy updates and apply operations; it is not proof that all engine rendering/streaming work is single-threaded.

## Native archive layout

Each chunk header is little-endian `uint32 tag, uint8 version, uint32 payloadSize`.

- Rendering: `00712123`, version 0; one flags byte, override count, 8-byte override records, then ActorReference.
- ActorReference: `00F86910`, version 0; presence byte, model key, Actor chunk when present.
- Actor: `008DE50F`, version 3; two 45-byte world transforms, five scalar words, animation key, optional native controller chunk, child count, recursive ActorReferences.
- Controller: `017D60C9`, version 1. Removed from outgoing appearance data; rejected in incoming normalized visual data. The native reader leaves its cursor unchanged if this chunk is absent.
- Network-only material trailer: `MATS` (4 bytes), three uint32 texture keys, ten uint32 ARGB colors. It is excluded from the native reader's buffer length.

Transforms are normalized to the `AC38E0` identity representation: zero bytes except float 1 at each transform's +0C. `D2DAC0` establishes their world-transform role; they are not skeleton bone poses.

## Material map

C8 PartStats vtable `17562E4`, size `B4`. Native save `F40380`, load `F767E0`.
Texture handle offsets: `2C`, `34`, `38`; resource category 3; manager `19F4D3C`.
Packed ARGB offsets, in wire order: `30`, `5C`, `3C`, `48`, `40`, `44`, `4C`, `50`, `54`, `58`.
`A6FE10` maps the category/index descriptor to a stable resource key. `A70B80` resolves that key in the destination manager.
`F757C0 -> F60150 -> DE5670 -> DDA730` is the customization/material binding path. `B37520` decodes the packed ARGB values; they are not pointers.

## Session/server changes

The host remains inside DINPUT8.dll, not a separate server executable. Protocol 7 is 1148 bytes per packet; both instances require the same DLL. Host-session and per-connection random nonces, endpoint checks, authoritative recurring rosters, sequence wrap handling, finite transform checks, stale movement timeout, bounded receive queues, and visual fragment validation are implemented. Session reset during native capture discards the old capture. These nonce checks are session isolation, not cryptographic authentication or encryption.

## Historical validation (before the current source candidate)

- Release Win32 DLL compiles successfully (existing macro/export warnings remain).
- `tests/run_tests.cmd` passes against production NetworkManager.cpp with game services stubbed: real UDP loopback hello/welcome, endpoint/session isolation, forged player IDs, roster removal/reordering, transform sequencing/wrap, non-finite positions, motion deduplication, visual chunk reordering/duplicates, malformed/truncated archives, motion stripping and material trailer validation.
- `research/emulate_archive.py` executes the executable's actual native serializer and deserializer with Unicorn. Resource allocation/lookup and selected engine callbacks are mocked. It verifies archive layout, reader/writer calling conventions, acceptance of the omitted controller chunk, and independent destination render handle. It does not execute real model allocation, material rendering, or game simulation.
- Native visual entry points have byte-signature guards. The other pre-existing mod hooks remain executable-version-specific.

## Still unverified or incomplete

- Live appearance, skin/hair/armor compatibility, animation transitions/blending and rendering-thread integration.
- Missing assets/mod differences and dynamically registered asset keys across distinct saves.
- Live armor swaps may rebuild the render tree and interrupt an active animation; no animation playhead/late-join timeline synchronization is implemented.
- Combat outcomes, health/damage, equipment gameplay effects, inventories, quests, AI, interactions and world authority are not fully replicated.
- Safe native actor destruction on disconnect is not mapped/implemented. Stopped proxies can remain game-managed until the world unloads. Reconnect and area-transition behavior need further work.
- The build does not manufacture extra engine Player_Manager contexts. It uses the verified actor-spawn stage and destination-owned proxy records.

## Two-instance validation when testing the candidate

Load both instances into the same area with distinguishable clothing. In F1, host on one and join `127.0.0.1:7777` on the other. Confirm distinct nonlocal actor/render handles and `Peer visuals ... applied=yes` in each process's diagnostics. Check position, facing, idle/walk/run, weapons/attacks, visible armor, skin/hair colors, then a clothing change. `applied=yes` reports completed native calls; visual correctness must still be observed. Each instance writes its own `AmalurCoop-<process ID>.log` beside the executable, so the two instances do not overwrite each other's logs. Avoid treating successful connection alone as character-sync success.

## Complete component factory

Factory `FA5D80`, jump table `FA61B8`; table entries are confirmed against RTTI.

| ID | Native type | Size | Vtable |
|---:|---|---:|---|
| 0 | PartCombat | 0xe0 | 0x1756ea4 |
| 1 | PartTarget | 0x100 | 0x1756464 |
| 2 | PartPathing | 0xd8 | 0x1756b6c |
| 3 | PartSteering | 0x68 | 0x1756264 |
| 4 | PartAnimation | 0x1b8 | 0x1756164 |
| 5 | PartAmmo | 0x228 | 0x17571d4 |
| 6 | PartLocation | 0xc4 | 0x1755cdc |
| 7 | PartRendering | 0xf4 | 0x17560e4 |
| 8 | PartStats | 0xb4 | 0x17562e4 |
| 9 | PartInventory | 0xc8 | 0x1756da4 |
| 10 | PartItem | 0xa0 | 0x1756e24 |
| 11 | PartWeapon | 0x218 | 0x175745c |
| 12 | PartArmor | 0x148 | 0x17563e4 |
| 13 | PartBlocking | 0x2c | 0x17564e4 |
| 14 | PartBehavior | 0xdc | 0x1756d24 |
| 15 | PartPhysics | 0x350 | 0x175759c |
| 16 | PartSenseMemory | 0x18c | 0x17572dc |
| 17 | PartLineOfSight | 0x80 | 0x17567e4 |
| 18 | PartTalent | 0xdc | 0x1757384 |
| 19 | PartFastTravelPoint | 0x30 | 0x17565e4 |
| 20 | PartBuff | 0xec | 0x17576e4 |
| 21 | PartRead | 0x40 | 0x1756664 |
| 22 | PartCustomInteraction | 0x28 | 0x17566e4 |
| 23 | PartHarvest | 0x44 | 0x1756764 |
| 24 | PartDetection | 0x74 | 0x1756064 |
| 25 | PartAttributes | 0x54 | 0x1756364 |
| 26 | PartSkills | 0x34 | 0x1755f64 |
| 27 | PartServices | 0x38 | 0x1756864 |
| 28 | PartFormula | 0x3c | 0x1756964 |
| 29 | PartActivity | 0xf8 | 0x1757124 |
| 30 | PartUsableItem | 0x30 | 0x17569e4 |
| 31 | PartSocketable | 0x58 | 0x1756f24 |
| 32 | PartSockets | 0x40 | 0x17568e4 |
| 33 | PartLockable | 0x38 | 0x1756a64 |
| 34 | PartInteraction | 0x5c | 0x1755fe4 |
| 35 | PartScript | 0x6c | 0x1756564 |
| 36 | PartTransport | 0x2c | 0x1756ae4 |
| 37 | PartSchedule | 0x3c | 0x17574dc |
| 38 | PartSense | 0x84 | 0x1757254 |
| 39 | PartComponent | 0x54 | 0x1756fa4 |
| 40 | PartCharacterCustomizationItem | 0x30 | 0x1756bec |
| 41 | PartAttachToNodeAlias | 0x30 | 0x1756c6c |
| 42 | PartMotion | 0x6c | 0x17561e4 |

## Historical installed candidate

The Release Win32 DLL was installed beside koa.exe after confirming the game was stopped. The prior DLL is backed up as DINPUT8.before-character-sync-20261011-223616.dll.bak. DLL SHA-256: 8aa81e3a4ef3c63564a5f2cdd6f5d10ad694bae89a45133970102c695d2dca30. See build-manifest.json for the exact paths. No game EXE or save was modified, and no game instance was launched.

