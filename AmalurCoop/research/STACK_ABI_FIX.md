# Two-instance crash fix

Fixed the `F3DBD0` world-transform callback invocation in CharacterVisualRuntime.cpp. The executable ends this callback with `RET 4`: it consumes one 32-bit stack argument. The previous mod code declared/called it with no stack arguments, shifting ESP by four bytes. This is a concrete ABI defect consistent with the screenshot's Run-Time Check Failure #0 and the host log stopping immediately after proxy creation.

The corrected call uses NativeVisualCalls::UpdateWorldTransform and passes the required unused argument as zero. This fixes the signature rather than disabling runtime checks. Added visual-apply start logs and native exception stage diagnostics.

Validation:
- Executed the actual executable callback in Unicorn: the old call reproduces the four-byte ESP imbalance; the corrected call preserves ESP.
- Expanded the previous archive emulation to assert stack balance and cover the post-load callback. The earlier test covered serialization/deserialization but omitted this callback, so it missed the defect.
- Compiled the production wrapper with /RTC1 and /Od, then ran 1,000 invocations against a native RET 4 fixture; stack, ECX and argument checks pass.
- Production network/UDP and archive tests pass.
- Debug Win32 and Release Win32 builds pass. Existing linker export warnings remain.
- Audited neighboring native call argument counts for spawn, positioning, rotation rebuild, archive load/save and post-load finalization.

Installed the corrected Release Win32 DLL in the original game directory while no koa.exe process was running. The previous installed DLL was backed up; exact paths and new hash are in stack-fix-manifest.json. The previous installed hash differed from the release artifact from the preceding turn, so installation was verified by hash.

The identified stack mismatch is fixed and this build is ready for a two-instance retest. Live gameplay has not been run here, so this is not a claim that all other crashes or character-sync defects are eliminated. Restart both instances so both load the corrected DLL. If a second instance uses a separate game folder, it needs the same corrected DINPUT8.dll. Load the same area, host on one and join 127.0.0.1 on the other. Each instance writes AmalurCoop-<process ID>.log beside its executable.
