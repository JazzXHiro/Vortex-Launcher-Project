# Vortex

A game launcher for Windows. It finds your Steam and local games, pulls artwork
and metadata, tracks playtime, and recommends what to play next.

## Download

**[Download the latest release](../../releases/latest)** — `VortexSetup.exe`,
Windows 10/11 (x64).

Installs to your user folder, so there is no admin password and no UAC prompt.
Qt, the C++ runtime and a full Python environment are bundled — nothing else to
install.

> **Windows SmartScreen will warn you.** The installer is not code-signed, so
> Windows shows *"Windows protected your PC"*. Click **More info → Run anyway**,
> or verify the download against the SHA-256 checksum published with the
> release. See [SETUP.md](SETUP.md#windows-smartscreen-warning).

New install? [SETUP.md](SETUP.md) covers first run, the optional API keys, and
troubleshooting.

## What it does

- **Finds your games.** Steam libraries are detected automatically; point it at
  a folder for anything else.
- **Artwork and metadata.** Covers, hero images and logos from SteamGridDB;
  descriptions, ratings and genres from IGDB.
- **Playtime tracking.** Per-game sessions and totals, recorded as you play.
- **Recommendations.** Ranks your own library by what you actually play — no
  API key required.
- **Discover.** Suggests games you don't own yet, from a ~5,700-title IGDB
  catalog.
- **Controller navigation.** The whole UI is usable from a gamepad.

## Built with

| | |
|---|---|
| UI | Qt 6.11 (QML / Qt Quick), C++17 |
| Engine | C++17, Win32 + WinHTTP |
| Analytics | Python 3.12 — pandas, NumPy, scikit-learn |
| Storage | SQLite |
| Packaging | Inno Setup 6, embeddable CPython |

The launcher and the recommender are separate: a C++/QML front end over an
engine that manages the library, and a Python analytics layer that scores and
ranks it. They meet at flat-file caches and a SQLite database, which is why the
recommender can be re-run, reset or evaluated without touching the app.

## Building from source

Needs Qt 6.11 (msvc2022_64), Visual Studio 2022+ and CMake. From an **x64
Native Tools Command Prompt**:

```bash
cmake --preset Qt-Release && cmake --build out/build/release
```

Development credentials go in `analytics\.env` — copy `analytics\.env.example`
and fill it in. That file is gitignored, and the packaging script refuses to
build an installer containing one.

See [SETUP.md](SETUP.md#for-developers) for the details and
[docs/RELEASING.md](docs/RELEASING.md) for how a release is cut.

## Repository layout

```
source/      C++ engine — library scanning, Steam, IGDB, SteamGridDB, playtime
ui/          Qt Quick front end — QML views and the C++/QML bridge
analytics/   Python recommender — scoring, evaluation, IGDB catalog, SQLite
scripts/     package_release.ps1 — stages the payload, compiles the installer
installer/   vortex.iss — Inno Setup definition
```

## License

Not yet licensed. Until a LICENSE file is added, default copyright applies and
the code is not licensed for reuse.
