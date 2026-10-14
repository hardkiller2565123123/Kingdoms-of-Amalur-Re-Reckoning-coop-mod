# Native Multiplayer menu and performance candidate - 2026-10-13

## Latest session flow

Multiplayer is inserted before the options_win module, independent of Continue availability. Host/Join select the original Load Game node (735424) through root launch (657596), highlight and go_down_a_level. Native button_tree handles callbacks, save-device checks, asynchronous window load, selection and Back navigation. The Lua fixture checks insertion order and both save-picker actions.

Network initialization always remains Offline; Config ignores the legacy AutoConnect field on load. Successful explicit Join increments an atomic generation. The render-thread remote manager waits for the local world gate and a fresh host transform, then moves the local PartLocation through F5DD50, offset 100 units beside the host. It records completion once per Join and does not continuously pull the player back. This does not load another area's assets or synchronize world identity: both saves must be in the same loaded area. No cross-area teleport is claimed.

These changes remain source-only. Added C++ assertions for offline startup with legacy AutoConnect=true and explicit Join generation have not been built or run. Menu fixtures and native request flag emulation pass.

## Follow-up after the missing-menu report

The 13:47 live log installed the hooks but never patched a chunk. A read-only inspection of the running game's memory found the actual main_menu override: 56770 bytes, FNV-1a `83744CB7`, 719 root instructions, 307 constants and 67 child functions. The initial_0.pak extraction used previously was the older 56302-byte variant. Both exact fingerprints are now supported, with separate verified offsets; unknown variants remain untouched.

The embedded script now registers `_G.amalur_coop_menu` explicitly. Loadstring inherits the calling module environment, so a bare assignment can put the submenu in main_menu instead of the global module table searched by button_tree. The Lua fixture now executes inside that module environment and checks the global registration.

Native `10D434A` rejects text in a secure VM (`global_State+DC == 2`). The hook permits compilation only for a byte-for-byte match of its own embedded source, temporarily using mode 1 and restoring the previous mode with __finally. No other script load changes mode. Compilation result and protected script errors are logged, as well as the successful ready notification.

Both base and live-override chunks pass structural/bootstrap checks (66 and 67 children retained), and the Lua module-environment fixture passes. The game was read only; this follow-up has not been built, deployed, or run in the game. These checks do not prove successful execution in the native HavokScript VM.

## Native menu

MainMenuMultiplayer hooks the game's HKS load-buffer API `10D5860` (cdecl, five arguments) and registered print callback `AA9470` (cdecl, Lua state). Only the two exact main_menu fingerprints listed above are patched. On an unmatched asset it leaves the original chunk untouched.

The HavokScript v8 chunk keeps all 66 original child functions and original constants. The final root return is replaced by `pcall(loadstring(source))` and a return, using memoized global instructions with two reserved nil constants each. MainMenuScript.h extends the original menu via a native button_tree module child. Native layout/input/navigation opens Multiplayer, with Host and Join callback leaves and a status row.

The print bridge accepts only one exact prefixed string command. String extraction uses `10C01F0`; callback results are pushed through `AA3340`. Host/Join commands are handed to the network worker. Join uses Config's address and port, including auto discovery. Neither button automatically loads a save. The original menu update runs before periodic status refresh.

Expected log: `Native Multiplayer button tree initialized: Multiplayer -> Host / Join`. A hooks-ready message alone does not prove script initialization. Restart after building to intercept the initial main_menu load. Configuration uses the INI; diagnostics use per-process logs. The F1 overlay has been removed.

Format research used the primary-source [hksc](https://github.com/Jake-NotTheMuss/hksc) and [DSLuaDecompiler](https://github.com/katalash/DSLuaDecompiler) repositories, then checked the shipped v8 bytes and game loader disassembly. No game assets are redistributed or modified on disk.

## Performance changes

- Cache up to 32 memory-region permission queries per thread for 16ms; every actual read remains guarded by SEH and validates address bounds. PositionTracker shares these reads.
- Queue animation sends for the network worker. Render-side peer/stat/status reads avoid waiting for its mutex; visual reads and incoming motion retry when busy.
- Bound each worker receive pass to 32 packets / a 2ms elapsed budget checked between packets; send at most eight queued motion packets and eight visual chunks per update. This is not a hard bound on an individual socket or packet-processing call.
- Capture player snapshots at 30Hz; capture appearance at most once a second while peers are connected. Resolve Join DNS and discovery adapter enumeration outside the network mutex.
- Log separate tracker/snapshot/remote/spawn-visual-motion timings every five seconds. These measure mod work, excluding the frame limiter and driver Present.

These target stalls observed in prior logs. There is no measured post-change FPS result, and a single native asset load may still stall a frame.

## Checks and reproduction

Source-only checks passed: Lua 5.1 fixture for original menu preservation, submenu callbacks, status updates and reload idempotency; structural HKS reader preserving all 66 child functions and checking bootstrap instructions; native queue ABI emulation; git diff whitespace checks.

The Lua fixture mocks button_tree/print. It does not execute the game's HavokScript VM or render the actual menu. The bytecode fixture models the C++ patch, rather than compiling/calling it. No C++ build, deployment or game launch was performed.

With Python packages `unicorn`, `pefile`, and `lupa` available:

```powershell
python research/verify_queue_abi.py 'D:/SteamLibrary/steamapps/common/Kingdoms of Amalur Re-Reckoning/koa.exe'
python research/verify_native_menu.py '<path-to-extracted-13493.lua_bxml>'
```

After the user builds Release Win32 for both instances: restart, Host/Join from Multiplayer, load the same area, then check movement/attack/dodge/weapon animations and both foreground/background FPS. Keep both per-process AmalurCoop logs; animation capture/replay counters and stage timings distinguish missing events from native replay failures and frame stalls.

### Frame hook and spawn recovery

ImGui is removed from the project and dependency installer. `GameFrameHook` retains the Present pump without a render target, device context, UI frame, ResizeBuffers detour or keyboard polling. Focus deactivation filtering and foreground/background caps remain.

Remote actor identities are checked once per second. A missing registry identity must remain absent for five seconds before clearing it and scheduling a replacement. Existing actors with changed/reactivated components are rebound and invalidate visual/motion caches. Failed spawns retain any allocated nonlocal handle; retries cannot allocate over an existing identity. Pre-allocation failures retry every five seconds initially, then every thirty seconds instead of permanently giving up. A failed position call preserves the identity for recovery. These source paths have not been exercised in live gameplay.


## Disappearing peers during gameplay

The latest logs show successful proxy creation and archive apply, without repeated recovery events during the stable world. A successful apply was insufficient: F11950 serializes C7+F1 (its bit 0 hides the whole character in F1DA50/F757C0), and D1B300 serializes Actor+1D0 (CCC300 forces opacity zero for E2004). These sender-local states were imported into the receiver and could change visual revisions during gameplay.

Normalization now removes C7+F1 bit 0 and Actor+1D0 view/update mask EA143. The root also clears explicit-hidden bit 4; attached models retain that bit for weapon visibility. Other actor scalars, stable catalog keys, material data and child trees remain. Both capture and receiver normalization apply this rule, as introduced in the protocol-v7 candidate. Stationary proxies also publish their C6 transform to the rendering tree each frame, since an appearance/animation update can dirty rendering without network movement.

Added C++ normalization regressions, but did not compile them per the user's source-only instruction. Native opacity/catalog fixtures pass. No DLL built, deployed, injected or launched. Live Tab/focus behavior, movement, sword swings and frame rate still require the user's two-instance test.


## October 14: appearance churn and transform publication

The two process logs preceding the 00:20 recording show proxies created and animation requests replayed on both sides, but repeated appearance revisions replace controllers every few seconds. Capture now compares a separate normalized appearance key: owner identity and runtime catalog are zeroed, transient actor flags excluded, while model keys, child structure, attachment-hidden flags, overrides and materials remain significant. The actual transported archive retains its catalog/owner fields. This avoids replacing animation controllers solely because animation state changes.

Remote positions use a 60 ms exponential smoothing time constant with normalized shortest-arc quaternion interpolation. First samples, gaps over 250 ms and displacement over 1500 game units snap. F116B0 now receives a remote pose correction immediately before actor/spatial transform publication, preventing ordinary actor updates from winning over the previous Present update. The hook verifies its prologue, confines correction to tracked nonlocal actors on the Present thread, validates active components and avoids waiting on the proxy lock. Native calls execute outside that lock. Stale poses expire; unsupported hook installation is logged and retains the Present fallback.

The Unicorn fixture executes F116B0's normal path and verifies RET 4 and identical matrix arguments to actor and spatial publication. Appearance-key and protocol C++ regressions were added but not built. Native fixtures and source review cannot prove that both instances render every animation: the user must build protocol v8 for both instances and test movement, turns, attacks, visibility and focus switches.
