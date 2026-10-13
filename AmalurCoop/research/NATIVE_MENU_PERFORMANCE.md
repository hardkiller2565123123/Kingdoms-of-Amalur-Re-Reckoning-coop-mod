# Native Multiplayer menu and performance candidate - 2026-10-13

## Follow-up after the missing-menu report

The 13:47 live log installed the hooks but never patched a chunk. A read-only inspection of the running game's memory found the actual main_menu override: 56770 bytes, FNV-1a `83744CB7`, 719 root instructions, 307 constants and 67 child functions. The initial_0.pak extraction used previously was the older 56302-byte variant. Both exact fingerprints are now supported, with separate verified offsets; unknown variants remain untouched.

The embedded script now registers `_G.amalur_coop_menu` explicitly. Loadstring inherits the calling module environment, so a bare assignment can put the submenu in main_menu instead of the global module table searched by button_tree. The Lua fixture now executes inside that module environment and checks the global registration.

Native `10D434A` rejects text in a secure VM (`global_State+DC == 2`). The hook permits compilation only for a byte-for-byte match of its own embedded source, temporarily using mode 1 and restoring the previous mode with __finally. No other script load changes mode. Compilation result and protected script errors are logged, as well as the successful ready notification.

Both base and live-override chunks pass structural/bootstrap checks (66 and 67 children retained), and the Lua module-environment fixture passes. The game was read only; this follow-up has not been built, deployed, or run in the game. These checks do not prove successful execution in the native HavokScript VM.

## Native menu

MainMenuMultiplayer hooks the game's HKS load-buffer API `10D5860` (cdecl, five arguments) and registered print callback `AA9470` (cdecl, Lua state). Only the two exact main_menu fingerprints listed above are patched. On an unmatched asset it leaves the original chunk untouched.

The HavokScript v8 chunk keeps all 66 original child functions and original constants. The final root return is replaced by `pcall(loadstring(source))` and a return, using memoized global instructions with two reserved nil constants each. MainMenuScript.h extends the original menu via a native button_tree module child. Native layout/input/navigation opens Multiplayer, with Host and Join callback leaves and a status row.

The print bridge accepts only one exact prefixed string command. String extraction uses `10C01F0`; callback results are pushed through `AA3340`. Host/Join commands are handed to the network worker. Join uses Config's address and port, including auto discovery. Neither button automatically loads a save. The original menu update runs before periodic status refresh.

Expected log: `Native Multiplayer button tree initialized: Multiplayer -> Host / Join`. A hooks-ready message alone does not prove script initialization. Restart after building to intercept the initial main_menu load. The F1 interface remains available for configuration and diagnostics.

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
