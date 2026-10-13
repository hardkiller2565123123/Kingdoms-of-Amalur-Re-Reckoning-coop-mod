# Native clip queue mapping - 2026-10-13

Mapped executable: `koa.exe`, SHA-256 `16a400f6e8fc10dbe446a9e57717de9fbe975ef17c004f4ca405e2e4e09cb314`. Addresses are preferred-image VAs, image base `0x400000`.

## Why the previous hook missed movement

The previous test log had a valid controller but zero captured local requests at `D3EC40`. That function handles explicit requests; graph transitions also enqueue clips through `D1D5D0`. Its callers include `D1DECD`, `D1E25B`, `D27699`, `D3EEC1` and `D3EFBE`. The current hook captures the shared queue after native variant selection. The earlier `verify_animation_abi.py` remains historical evidence for D3EC40, not the current hook.

## ABI and identities

`D1D5D0` is void thiscall: ECX = rig asset, stack arguments = request pointer, state integer, transition integer, queue pointer, optional output pointer. It pops `0x14` bytes. The hook calls the original once, then reads the selected queue node while the caller's descriptor remains alive.

Native request size is `0x70`: owner +00; category-8 handles +04/+08; hash +0C; previous hash +10; four variants +14; four weights +24; actor handle +34; start offset +38; speed +3C; six blend floats +40 and enabled byte +58; callbacks +5C/+60; catalog handle +64; catalog index +68; flag +6C. Callback pointers are rebuilt as zero, never transmitted.

Controller owner is +A8, actor identity +B4, rig handle +A4. Track array is +60, count +64, stride `0x5C`, each queue at track+04. Capture accepts only queues in the local controller's array, with matching owner/actor identity, and wire track indices 0..7.

Queue node size is `0x6C`: rig hash index +00, catalog index +04, selected variants +08, weights +18, speed +28, borrowed catalog +2C, retained optional resources +30/+34, state +38, transition +3C, start +40, blend +44, callbacks +60/+64, flag +68. D1D5D0 retains the optional resources itself; replay must not add a second owning reference to its stack descriptor.

Replay resolves stable keys in the receiving process. Catalogs must already belong to the receiver's model (owner+1B4 or model+18C array/count+190). This avoids rejecting a valid catalog merely because the proxy's graph active-mask differs. Hash and variant bounds are checked against that catalog. Missing tracks are initialized through D3EBF0; the selected queue is cleared with CE6910 and populated through D1D5D0. Matching appearance/rig resources are still required.

## Protocol and recovery

Follow-up after the live animation report: the log showed thousands of local requests and successful replay calls, including roughly 60 requests/second while the sender position was stationary. Every captured queue insertion previously received a new serial, even if it represented identical clip state. Outgoing capture now preserves the prior state/serial for identical payloads. When only speed, weights or blend values change, replay updates a single matching current node in place instead of clearing its queue. Reuse requires matching catalog/index, variants, optional resources, start offset and flag; transitions to different clips still enqueue normally. CE6910 disassembly confirms read-index +14 and node storage at queue+04. Live visual improvement from this follow-up is unverified; intentional restarts of an otherwise identical clip are not separately signaled.

Protocol v7 stores an 84-byte AnimationRequest inside the existing MotionEvent visual area. Packet size remains 1148 bytes. Payload includes rig/catalog/optional resource keys, blend values, selected variants and weights, track and start offset. No sender pointer or resource handle is replayed locally. Both instances require the updated source build.

Latest state is cached and resent per track every second. Animation serial is independent of UDP sequence; a repeated state does not restart an already applied clip. Appearance replacement invalidates receiver controller/serial caches. Network sends are queued for the worker, not performed inside the render callback or native hook.

## Source-only validation and limits

`verify_queue_abi.py` executes the real executable's D1D5D0 instructions under Unicorn, mocking allocation, resource lookup, reference callbacks and variant selection. Checks pass for five-argument stack cleanup, queue insertion, descriptor values, selected variants/weights and reference retention. This is ABI evidence, not a real animation rendering test.

NetworkTests.cpp includes protocol payload, invalid track/weight/catalog validation, retry serial and outgoing queue/session-reset cases. These C++ tests were not built or run for this candidate. No DLL was built or deployed and no game was launched.

Live walking, running, attacks, dodge, weapon changes and blended transitions still require two-instance validation. Exact playhead synchronization, all scripted/facial animation paths, track-removal events and gameplay combat authority are not complete. This is not a full map of the executable.
