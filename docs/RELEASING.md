# Releasing Vortex

How a Windows release is built and published. For everyday build instructions
see [SETUP.md](../SETUP.md#for-developers).

## Prerequisites

| Tool | Notes |
|---|---|
| Visual Studio 2022+ | With the C++ x64 toolset. All commands run from an **x64 Native Tools Command Prompt** — the presets use Ninja + MSVC and will not find a compiler otherwise. |
| Qt 6.11.0 msvc2022_64 | `CMakeUserPresets.json` locates it via `QTDIR`. That file is user-specific and gitignored; Qt Creator regenerates it, or copy the pattern from `CMakePresets.json`. |
| CMake + Ninja | CMake 3.16 or newer. |
| [Inno Setup 6](https://jrsoftware.org/isdl.php) | The script probes `%LocalAppData%\Programs\Inno Setup 6`, then both Program Files locations. Without it, staging still runs and the compile step is skipped with a warning. |

The packaging script downloads an embeddable CPython and the analytics wheels,
so the first run needs a network connection.

## 1. Bump the version

`AppVersion` in [`installer/vortex.iss`](../installer/vortex.iss).

**Never change `AppId`.** That GUID is how Windows recognises an upgrade of an
existing install rather than a second, parallel one. Changing it strands every
existing user with two copies of Vortex.

## 2. Build

```bash
cmake --preset Qt-Release
cmake --build out/build/release
```

Build Debug too if you are testing locally — the project convention is to keep
both current:

```bash
cmake --preset Qt-Debug
cmake --build out/build/debug
```

## 3. Package

```bash
powershell -ExecutionPolicy Bypass -File scripts\package_release.ps1
```

This stages a pruned payload from the Release build, adds the app-local VC++
runtime, builds an embeddable Python 3.12.8 with the analytics dependencies,
scans the result for credentials, and compiles `dist\VortexSetup.exe`.

| Flag | Use |
|---|---|
| `-ReusePython` | Reuse the existing `dist\payload\python` tree. The wheel bundling is by far the slowest step; skip it when only app code changed. |
| `-SkipInstaller` | Stage the payload but stop before ISCC. Useful for inspecting what would ship. |
| `-AllowDebugBuild` | Package a Debug build. The result links the debug CRT and **will not start** on a machine without Visual Studio — for local diagnosis only, never for release. |
| `-BuildDir` / `-OutDir` | Override the default `out\build\release` and `dist`. |

### The credential gate

Packaging **aborts** if an `.env` survives pruning, or if any staged file
contains something shaped like a credential assignment. This exists because
every Vortex data file resolves against the executable's directory (see
`source/app_paths.h`), so a Release build directory accumulates the developer's
own `.env`, database, playtime log and artwork.

If the gate fires, fix what put the file there. Never work around it — it is
the last check between a live IGDB secret and a public download.

## 4. Verify before publishing

Installing on your development machine proves nothing about the bundled
runtimes, because Qt and the C++ runtime are already on it. Test on a clean
Windows 10/11 x64 machine or VM with **no** Qt, Visual Studio or Python:

- The installer runs without an admin prompt and lands in
  `%LocalAppData%\Programs\Vortex`.
- The app starts, finds the Steam library, and the first-run wizard can be
  skipped entirely.
- Recommendations work — this exercises the bundled Python.
- The install folder contains **no** `.env` carrying your credentials, and
  `local_game_dirs.txt` is the blank template rather than your own paths.
- Uninstalling prompts about deleting user data and defaults to **No**.

## 5. Publish

```bash
sha256sum dist/VortexSetup.exe
```

Tag the commit, create the GitHub Release, and attach `VortexSetup.exe`.

The installer is a release asset, never a committed file — `dist/` is
gitignored, and at ~96 MB the binary is close to GitHub's 100 MB blob limit
while sitting well inside the 2 GB limit for release assets.

Include in the release notes:

- The SHA-256, so users can verify the download.
- A line about the SmartScreen warning and the *More info → Run anyway* path.
  The build is unsigned, and this is the most common reason an install fails
  for someone.

## Note on end-user documentation

`package_release.ps1` copies `SETUP.md` into the payload as `README.md`, so it
ships inside the install folder. Edits to end-user setup instructions belong in
`SETUP.md`, not in this file or the repository `README.md`.
