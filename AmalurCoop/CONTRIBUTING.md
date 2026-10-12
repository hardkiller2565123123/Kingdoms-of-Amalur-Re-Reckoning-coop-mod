# Contributing

AmalurCoop is an early reverse-engineering and multiplayer research project. Keep changes focused, testable, and easy to review.

## Before submitting code

- Build with Visual Studio 2022 using `Release | Win32`.
- Do not commit compiled DLLs, game files, dumps, save files, logs, or downloaded third-party source trees.
- Do not include copyrighted game assets or executable files.
- Keep executable-specific addresses isolated and clearly named.
- Preserve existing features unless a change explicitly replaces a broken implementation.
- Add defensive pointer validation and useful logging around new engine calls.
- Avoid calling gameplay engine functions from the UDP worker thread; queue work for the game/render thread when necessary.

## Reports and pull requests

Include:

- exact game executable/version information;
- whether the test used one or two computers;
- host/client roles and UDP port;
- relevant `AmalurCoop.log` excerpts;
- a concise description of expected and observed behavior; and
- reproduction steps.

Do not publish personal IP addresses, account information, or private save data in issues.
