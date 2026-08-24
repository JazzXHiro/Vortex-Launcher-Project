# Vortex

A game launcher for Windows. It finds your Steam and local games, pulls artwork
and metadata, tracks playtime, and recommends what to play next.

## Install

Run **`VortexSetup.exe`** and click through it.

That's the whole thing. No admin password, no Python, no database to set up —
everything Vortex needs is inside the installer. It installs to your user
folder (`%LOCALAPPDATA%\Programs\Vortex`), so Windows won't prompt you for
permission.

The first time it opens, a short wizard offers two optional API keys. **You can
skip all of it** — your Steam library, playtime tracking and recommendations
work without them.

## Windows SmartScreen warning

The first time you run `VortexSetup.exe`, Windows may show a blue
**"Windows protected your PC"** box and hide the Run button.

That is expected. The installer is not code-signed — a signing certificate is a
paid, per-year expense — so SmartScreen has no publisher reputation to check it
against. Click **More info**, then **Run anyway**.

If you would rather verify the download than trust the prompt, every release
publishes a SHA-256 checksum. Compare it in PowerShell:

```powershell
Get-FileHash .\VortexSetup.exe -Algorithm SHA256
```

The hash it prints should match the one on the release page.

Vortex requires 64-bit Windows 10 or 11.

## What the optional keys add

Both are free, and you can add them later from Settings.

| | What it needs |
|---|---|
| Steam library, launching games, playtime | nothing |
| Local (non-Steam) games | add the folder in Settings |
| Recommendations from your own library | nothing |
| Cover art, hero images, logos | a SteamGridDB key |
| Descriptions, ratings, genres | IGDB credentials |
| **Discover** — games you don't own yet | IGDB credentials, then one catalog download |

**SteamGridDB** — sign in at
[steamgriddb.com/profile/preferences/api](https://www.steamgriddb.com/profile/preferences/api)
and copy the key.

**IGDB** — authenticates through Twitch. Register an app at
[dev.twitch.tv/console/apps](https://dev.twitch.tv/console/apps), set the OAuth
redirect URL to `http://localhost` (it isn't used), then copy the Client ID and
generate a Client Secret. *The secret is shown only once.*

After entering the IGDB keys, the wizard offers to download the Discover
catalog — about 5,700 games. It takes a few minutes and runs in the background;
you can keep using Vortex while it works.

## Adding your non-Steam games

Settings → Directories → add a folder. Vortex scans it for game executables.
Steam games are found automatically and don't need to be added.

## Troubleshooting

**Recommendations say "unavailable".**
The bundled Python lives in `python\` inside the install folder. If it went
missing, reinstall. Nothing else on your machine affects it.

**No artwork.**
Check the SteamGridDB key in Settings. Artwork downloads lazily — covers when a
game first appears, hero images and logos when you open its detail page.

**Discover is empty.**
The catalog download hasn't run. Settings → Recommendations → download catalog.

**Games have no descriptions or ratings.**
Check the IGDB credentials in Settings. If you didn't copy the client secret
when you created the Twitch app, generate a new one — it's only shown once.

**Starting over.**
Uninstall and choose **Yes** when asked about deleting your data, then
reinstall. Choosing **No** keeps your playtime history and keys for the next
install.

---

## For developers

Building from source needs Qt 6.11 (msvc2022_64), Visual Studio 2022+ and
CMake. From an **x64 Native Tools Command Prompt**:

```bash
cmake --preset Qt-Release && cmake --build out/build/release
```

To produce the installer:

```bash
powershell -ExecutionPolicy Bypass -File scripts\package_release.ps1
```

That stages a payload, bundles an embeddable Python with the analytics
dependencies, refuses to continue if any credential survived the pruning, and
compiles `dist\VortexSetup.exe` with [Inno Setup 6](https://jrsoftware.org/isdl.php).
Pass `-SkipInstaller` to stop after staging, or `-ReusePython` to skip
rebuilding the Python bundle when only the app changed.

Analytics data lives in `analytics\vortex.sqlite3` (SQLite — Vortex used
PostgreSQL until v3 and moved so the app could ship self-contained). To rebuild
it from the flat-file caches:

```bash
cd analytics && python reset_db.py --yes
```

Credentials for development go in `analytics\.env`; copy `.env.example` and
fill it in. That file is gitignored and the packaging script refuses to build
an installer that contains one.
