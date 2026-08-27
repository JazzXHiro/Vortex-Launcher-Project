<#
.SYNOPSIS
    Stage a clean, secret-free payload from a Release build and compile the installer.

.DESCRIPTION
    Produces dist\VortexSetup.exe -- a per-user installer that leaves the
    recipient with a working application: no Python install, no database
    server, no command line.

    The Release build directory is not shippable as-is. It carries debug
    symbols, CMake/Ninja scaffolding, and -- because every Vortex data file
    resolves against the executable's own directory (see app_paths.h) -- the
    developer's *personal* runtime state: their Steam library snapshot,
    playtime log, wishlist, downloaded artwork, and analytics\.env with live
    IGDB, SteamGridDB and Postgres credentials in it.

    Stages:
      1. validate the build (Release, Qt deployed)
      2. copy and prune
      3. add the app-local VC++ runtime
      4. build the embeddable Python tree with the analytics dependencies
      5. refuse to continue if anything sensitive survived
      6. compile the installer

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\package_release.ps1

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\package_release.ps1 -SkipInstaller
#>
[CmdletBinding()]
param(
    [string] $BuildDir,
    [string] $OutDir,
    # Stage the payload but stop before compiling the installer.
    [switch] $SkipInstaller,
    # Reuse an already-built payload\python tree. The Python bundle is the slow
    # part of packaging (a few hundred MB of wheels); this skips rebuilding it
    # when only the app changed.
    [switch] $ReusePython,
    # Package a Debug build anyway. The result will not start on a machine
    # without Visual Studio -- see the ucrtbased.dll check below.
    [switch] $AllowDebugBuild
)

$ErrorActionPreference = 'Stop'

$RepoRoot = Split-Path -Parent $PSScriptRoot
if (-not $BuildDir) { $BuildDir = Join-Path $RepoRoot 'out\build\release' }
if (-not $OutDir)   { $OutDir   = Join-Path $RepoRoot 'dist' }

$StageDir  = Join-Path $OutDir 'payload'
$CacheDir  = Join-Path $OutDir '.cache'
$IssPath   = Join-Path $RepoRoot 'installer\vortex.iss'

# Pinned rather than "latest": the embeddable build and the wheels have to
# agree on an ABI tag, and a silent minor bump is exactly the kind of thing
# that produces an installer that only fails on someone else's machine.
$PythonVersion = '3.12.8'
$PythonZipUrl  = "https://www.python.org/ftp/python/$PythonVersion/python-$PythonVersion-embed-amd64.zip"
$GetPipUrl     = 'https://bootstrap.pypa.io/get-pip.py'

function Write-Step($message) { Write-Host "==> $message" -ForegroundColor Cyan }
function Write-Note($message) { Write-Host "    $message" -ForegroundColor DarkGray }


# ---------------------------------------------------------------------------
# 1. Validate the build we are about to package
# ---------------------------------------------------------------------------
Write-Step "Checking build output in $BuildDir"

if (-not (Test-Path $BuildDir)) {
    throw "Build directory not found: $BuildDir`nConfigure and build Release first, from an x64 Native Tools prompt:`n    cmake --preset Qt-Release`n    cmake --build out/build/release"
}

$LauncherExe = Join-Path $BuildDir 'VortexLauncher.exe'
if (-not (Test-Path $LauncherExe)) {
    throw "VortexLauncher.exe not found in $BuildDir -- the build did not complete."
}

# A Debug build imports ucrtbased.dll, which ships only with the Windows SDK
# and is not redistributable. Catching it here beats the recipient seeing
# "ucrtbased.dll was not found" and concluding the app is broken.
$exeBytes = [System.IO.File]::ReadAllBytes($LauncherExe)
$exeText  = [System.Text.Encoding]::ASCII.GetString($exeBytes)
$isDebugBuild = $exeText -match 'ucrtbased\.dll'
Remove-Variable exeBytes, exeText

if ($isDebugBuild) {
    if (-not $AllowDebugBuild) {
        throw "$LauncherExe is a DEBUG build (imports ucrtbased.dll).`nIt cannot run on a machine without Visual Studio installed.`nBuild Release instead, or pass -AllowDebugBuild if you really mean it."
    }
    Write-Warning 'Packaging a Debug build. This will NOT start on a clean machine.'
}

$qtCore = @(Get-ChildItem -Path $BuildDir -Filter 'Qt6Core*.dll' -ErrorAction SilentlyContinue)
if ($qtCore.Count -eq 0) {
    throw "No Qt6Core DLL beside the executable -- windeployqt did not run.`nReconfigure with -DVORTEX_DEPLOY_QT=ON and make sure windeployqt is findable."
}
Write-Note "Found $($qtCore[0].Name)"


# ---------------------------------------------------------------------------
# 2. Copy the build output to a clean staging folder
# ---------------------------------------------------------------------------
Write-Step "Staging into $StageDir"

$pythonBackup = $null
if ($ReusePython -and (Test-Path (Join-Path $StageDir 'python\python.exe'))) {
    $pythonBackup = Join-Path $OutDir '.python-reuse'
    if (Test-Path $pythonBackup) { Remove-Item -Recurse -Force $pythonBackup }
    Move-Item (Join-Path $StageDir 'python') $pythonBackup
    Write-Note 'preserved existing python/ for reuse'
}

if (Test-Path $StageDir) { Remove-Item -Recurse -Force $StageDir }
New-Item -ItemType Directory -Force -Path $StageDir | Out-Null
Copy-Item -Path (Join-Path $BuildDir '*') -Destination $StageDir -Recurse -Force


# ---------------------------------------------------------------------------
# 3. What gets pruned
# ---------------------------------------------------------------------------
$ArtifactDirs = @(
    'CMakeFiles', '.cmake', '.qt', '.rcc', 'meta_types', 'qmltypes',
    'Testing', 'VortexLauncher_autogen', 'VortexCLI_autogen', 'qmltooling'
)
$ArtifactFiles = @(
    '*.pdb', '*.ilk', '*.exp', '*.lib', '*.obj', '*.lnk',
    'CMakeCache.txt', '.ninja_deps', '.ninja_log', 'build.ninja',
    'VSInheritEnvironments.txt', 'cmake_install.cmake',
    'vortexlauncher_qmltyperegistrations.cpp'
)

# The developer's own runtime state. Every one of these is written by the
# launcher beside the exe. Shipping them hands over a snapshot of the
# developer's library and playtime, and gives the recipient a first launch
# pre-populated with games they do not own.
$PersonalDirs  = @('Images', 'CandidateImages')
$PersonalFiles = @(
    'installed_games.txt', 'wishlist.json', 'favorite_snapshots.json',
    # The played-games ledger: every game the developer has ever launched,
    # with hours and artwork paths. Same class of data as wishlist.json.
    'played_games.json',
    'playtime_sessions.log', 'playtime_stats.txt',
    'preferences.json', 'settings.json',
    'igdb_cache.txt', 'exe_cache.txt', 'game_metadata.txt',
    'local_game_dirs.txt',
    # The diagnostic log. Names every game the developer owns and every folder
    # they scanned, so it is personal data as much as any cache; the recipient
    # gets a fresh one on first launch.
    'vortex.log'
)

# Secrets and generated state under analytics\.
# .env holds a live IGDB client secret, SteamGridDB API key and (historically)
# a Postgres password. .igdb_token.json is a cached OAuth bearer token. The
# database and the model files are rebuilt on the recipient's machine --
# and recommendations.json describes the developer's taste.
$AnalyticsSecretFiles = @(
    'analytics\.env',
    'analytics\.igdb_token.json',
    'analytics\recommendations.json',
    'analytics\recommendations_meta.json',
    'analytics\model\model_manifest.json',
    'analytics\vortex.sqlite3',
    'analytics\vortex.sqlite3-wal',
    'analytics\vortex.sqlite3-shm'
)
$AnalyticsSecretGlobs = @(
    @{ Dir = 'analytics\model'; Filter = '*.pkl' }
)

Write-Step 'Removing build artifacts, personal data and secrets'

foreach ($name in ($ArtifactDirs + $PersonalDirs)) {
    $path = Join-Path $StageDir $name
    if (Test-Path $path) {
        Remove-Item -Recurse -Force $path
        Write-Note "removed dir  $name"
    }
}

foreach ($pattern in ($ArtifactFiles + $PersonalFiles)) {
    Get-ChildItem -Path $StageDir -Filter $pattern -File -Force -ErrorAction SilentlyContinue |
        ForEach-Object {
            Remove-Item -Force $_.FullName
            Write-Note "removed file $($_.Name)"
        }
}

foreach ($relative in $AnalyticsSecretFiles) {
    $path = Join-Path $StageDir $relative
    if (Test-Path $path) {
        Remove-Item -Force $path
        Write-Note "removed      $relative"
    }
}

foreach ($glob in $AnalyticsSecretGlobs) {
    $dir = Join-Path $StageDir $glob.Dir
    if (Test-Path $dir) {
        Get-ChildItem -Path $dir -Filter $glob.Filter -File -Force -ErrorAction SilentlyContinue |
            ForEach-Object {
                Remove-Item -Force $_.FullName
                Write-Note "removed      $($glob.Dir)\$($_.Name)"
            }
    }
}

Get-ChildItem -Path $StageDir -Directory -Recurse -Force -Filter '__pycache__' -ErrorAction SilentlyContinue |
    ForEach-Object {
        Remove-Item -Recurse -Force $_.FullName
        Write-Note "removed dir  $($_.FullName.Substring($StageDir.Length + 1))"
    }


# ---------------------------------------------------------------------------
# 4. App-local VC++ runtime
# ---------------------------------------------------------------------------
# The exe imports MSVCP140.dll and VCRUNTIME140*.dll. windeployqt's
# --compiler-runtime did not supply them here, and the redistributable
# installer needs administrator rights that a per-user install does not have --
# so deploy them beside the executable, which Microsoft permits and which
# needs no elevation.
Write-Step 'Adding the VC++ runtime'

$vcRedistRoot = Get-ChildItem 'C:\Program Files\Microsoft Visual Studio\*\*\VC\Redist\MSVC\*\x64\Microsoft.VC*.CRT' -Directory -ErrorAction SilentlyContinue |
    Sort-Object FullName -Descending | Select-Object -First 1

if ($vcRedistRoot) {
    # Only what the binaries actually import, plus the two the CRT pulls in
    # transitively. Copying the whole folder would add OpenMP and C++/CLI
    # support libraries nothing here references.
    $needed = @('msvcp140.dll', 'msvcp140_1.dll', 'msvcp140_2.dll',
                'msvcp140_atomic_wait.dll', 'msvcp140_codecvt_ids.dll',
                'vcruntime140.dll', 'vcruntime140_1.dll', 'concrt140.dll')
    foreach ($dll in $needed) {
        $src = Join-Path $vcRedistRoot.FullName $dll
        if (Test-Path $src) {
            Copy-Item $src (Join-Path $StageDir $dll) -Force
            Write-Note "copied       $dll"
        }
    }
} else {
    Write-Warning 'VC++ redist folder not found. The installer will rely on the recipient already having the runtime.'
}


# ---------------------------------------------------------------------------
# The ._pth file, and why it is rewritten rather than appended to
# ---------------------------------------------------------------------------
# The embeddable distribution ships with site-packages disabled: its ._pth has
# "import site" commented out and does not list Lib\site-packages. Without that
# edit pip installs successfully and then every import of numpy fails at
# runtime -- the classic silent failure of this approach.
#
# A ._pth file also REPLACES sys.path entirely, including the rule that normally
# puts a script's own directory on it. Without the analytics hop every analytics
# script dies on "No module named 'db'", and only ever on the recipient's
# machine: a developer running the same script with a normal Python install has
# the script directory added for them.
#
# The entry is rewritten, not appended. A shipped v1.1.0 build carried
# "..<BEL>nalytics" in its ._pth -- a literal 0x07 byte where an escaped
# backslash belonged -- which put a nonexistent directory on sys.path and killed
# Sync, Recommendations, Catalog and Explain on every install. Appending the
# correct line would have left the corrupt one sitting beside it.
#
# The hop uses a forward slash deliberately: Windows accepts it, and it leaves
# no backslash escape for a future editor or generator to eat the same way.
function Repair-PythonPth {
    param([Parameter(Mandatory)] [string] $PythonDir)

    $pthFile = Get-ChildItem -Path $PythonDir -Filter 'python*._pth' | Select-Object -First 1
    if (-not $pthFile) { throw "No python*._pth in the embeddable distribution -- layout changed." }

    $analyticsEntry = '../analytics'
    $sitePackages   = 'Lib' + [char]0x5C + 'site-packages'

    $pth = @(@(Get-Content $pthFile.FullName) |
        ForEach-Object { if ($_ -match '^\s*#\s*import\s+site\s*$') { 'import site' } else { $_ } } |
        Where-Object { $_ -notmatch 'nalytics\s*$' })

    if ($pth -notcontains $sitePackages) { $pth += $sitePackages }
    if ($pth -notcontains 'import site') { $pth += 'import site' }
    $pth += $analyticsEntry

    Set-Content -Path $pthFile.FullName -Value $pth -Encoding ascii

    # Assert what the old code merely assumed. A control byte is precisely the
    # defect described above; an analytics hop that resolves nowhere is the
    # symptom it produced on the recipient's machine.
    $written = @(Get-Content $pthFile.FullName)

    foreach ($line in $written) {
        if ($line -match '[\x00-\x08\x0B\x0C\x0E-\x1F]') {
            throw "$($pthFile.Name) contains a control character in '$line'.`nThat is a corrupted escape sequence -- refusing to ship a broken sys.path."
        }
    }

    $resolved = Join-Path $PythonDir $analyticsEntry
    if (-not (Test-Path $resolved)) {
        throw "$($pthFile.Name) puts '$analyticsEntry' on sys.path, but that resolves to '$resolved', which does not exist.`nThe analytics scripts would fail with ModuleNotFoundError on every install."
    }

    Write-Note "patched $($pthFile.Name): site-packages + analytics on sys.path"
}


# ---------------------------------------------------------------------------
# 5. Bundled Python
# ---------------------------------------------------------------------------
$PythonDir = Join-Path $StageDir 'python'

if ($pythonBackup) {
    Move-Item $pythonBackup $PythonDir
    Write-Step 'Reusing existing Python bundle (-ReusePython)'
    # A reused tree keeps whatever ._pth it was built with, which is how a
    # corrupt analytics hop survived into a shipped installer. Repair it here
    # too -- the rewrite is idempotent and costs nothing.
    Repair-PythonPth -PythonDir $PythonDir
} else {
    Write-Step "Building the Python $PythonVersion bundle"
    New-Item -ItemType Directory -Force -Path $CacheDir | Out-Null

    $zipPath    = Join-Path $CacheDir "python-$PythonVersion-embed-amd64.zip"
    $getPipPath = Join-Path $CacheDir 'get-pip.py'

    if (-not (Test-Path $zipPath)) {
        Write-Note "downloading $PythonZipUrl"
        Invoke-WebRequest -Uri $PythonZipUrl -OutFile $zipPath -UseBasicParsing
    } else {
        Write-Note 'using cached embeddable zip'
    }
    if (-not (Test-Path $getPipPath)) {
        Write-Note "downloading $GetPipUrl"
        Invoke-WebRequest -Uri $GetPipUrl -OutFile $getPipPath -UseBasicParsing
    }

    New-Item -ItemType Directory -Force -Path $PythonDir | Out-Null
    Expand-Archive -Path $zipPath -DestinationPath $PythonDir -Force

    Repair-PythonPth -PythonDir $PythonDir

    $pythonExe = Join-Path $PythonDir 'python.exe'
    Write-Note 'bootstrapping pip'
    & $pythonExe $getPipPath --no-warn-script-location 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "get-pip.py failed with exit code $LASTEXITCODE" }

    $requirements = Join-Path $RepoRoot 'analytics\requirements.txt'
    Write-Note "installing $requirements (this is the slow part)"
    & $pythonExe -m pip install --no-warn-script-location --no-compile -r $requirements
    if ($LASTEXITCODE -ne 0) { throw "pip install failed with exit code $LASTEXITCODE" }

    # Trim what the recipient will never execute. pip and setuptools are only
    # needed to build this tree; the test suites inside numpy/pandas/sklearn
    # are a large fraction of their footprint.
    Write-Note 'trimming the bundle'
    $sitePackages = Join-Path $PythonDir 'Lib\site-packages'
    foreach ($junk in @('pip', 'pip-*.dist-info', 'setuptools', 'setuptools-*.dist-info',
                        '_distutils_hack', 'pkg_resources', 'wheel', 'wheel-*.dist-info')) {
        Get-ChildItem -Path $sitePackages -Filter $junk -Force -ErrorAction SilentlyContinue |
            ForEach-Object { Remove-Item -Recurse -Force $_.FullName }
    }
    Get-ChildItem -Path $sitePackages -Directory -Recurse -Force -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -in @('tests', 'test', '__pycache__') } |
        ForEach-Object { Remove-Item -Recurse -Force $_.FullName -ErrorAction SilentlyContinue }
}

# Prove the bundle works before it goes anywhere near an installer. A tree
# that cannot import sklearn is the single most likely packaging defect, and
# it would otherwise surface only on the recipient's machine.
Write-Step 'Verifying the Python bundle'
$check = & (Join-Path $PythonDir 'python.exe') -c "import sklearn, pandas, numpy, sqlite3, joblib, requests, dotenv, db, config, model; print(sklearn.__version__)" 2>&1
if ($LASTEXITCODE -ne 0) {
    throw "The bundled interpreter cannot import the analytics dependencies:`n$check"
}
Write-Note "sklearn $check -- imports OK"


# ---------------------------------------------------------------------------
# 6. First-run files
# ---------------------------------------------------------------------------
Write-Step 'Adding first-run files'

# A blank template. The developer's copy points at D:\Games, which will not
# exist on the recipient's machine.
$gameDirsTemplate = @(
    '# Local Game Directories',
    '# One folder per line. The launcher scans each for game executables.',
    '# Steam games are found automatically and do not belong here.',
    '#',
    '# Example:',
    '# C:\Games'
)
Set-Content -Path (Join-Path $StageDir 'local_game_dirs.txt') -Value $gameDirsTemplate -Encoding utf8
Write-Note 'wrote        local_game_dirs.txt (blank template)'

$readmeSource = Join-Path $RepoRoot 'SETUP.md'
if (Test-Path $readmeSource) {
    Copy-Item $readmeSource (Join-Path $StageDir 'README.md') -Force
    Write-Note 'copied       README.md'
}

$envExample = Join-Path $StageDir 'analytics\.env.example'
if (-not (Test-Path $envExample)) {
    throw "analytics\.env.example is missing from the payload. The first-run wizard writes .env from it, so the package is unusable without it."
}


# ---------------------------------------------------------------------------
# 7. Refuse to ship if anything sensitive survived
# ---------------------------------------------------------------------------
Write-Step 'Scanning for leaked secrets'

$leaks = @(Get-ChildItem -Path $StageDir -Recurse -File -Force -ErrorAction SilentlyContinue |
    Where-Object {
        $_.Name -eq '.env' -or
        $_.Extension -eq '.pdb' -or
        $_.Name -like '*token*' -or
        $_.Name -eq 'recommendations.json' -or
        $_.Name -like 'vortex.sqlite3*' -or
        # .pkl anywhere except inside the bundled interpreter, where joblib and
        # sklearn legitimately ship sample data.
        ($_.Extension -eq '.pkl' -and $_.FullName -notlike "$PythonDir*")
    })

if ($leaks.Count -gt 0) {
    $leaks | ForEach-Object { Write-Host "    LEAK: $($_.FullName)" -ForegroundColor Red }
    throw "Refusing to package: $($leaks.Count) sensitive file(s) survived pruning."
}

# Belt and braces: look for an assignment that actually has a value on the
# right-hand side. .env.example and the README are allowed to name the keys,
# and config.py reads them through os.getenv, so those are exempt. The bundled
# interpreter is skipped -- it is thousands of third-party files and none of
# them came from this repo.
$textExtensions = @('.txt', '.json', '.py', '.md', '.log', '.sql', '.ini', '.cfg')
$suspects = @()

Get-ChildItem -Path $StageDir -Recurse -File -Force -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -notlike "$PythonDir*" } |
    Where-Object { $textExtensions -contains $_.Extension } |
    Where-Object { $_.Name -ne '.env.example' -and $_.Name -ne 'README.md' } |
    ForEach-Object {
        $file = $_
        Select-String -Path $file.FullName -Pattern 'IGDB_CLIENT_SECRET|STEAMGRIDDB_API_KEY|IGDB_CLIENT_ID|DB_PASSWORD' -ErrorAction SilentlyContinue |
            ForEach-Object {
                $line = $_.Line.Trim()
                if ($line -match '=\s*\S' -and $line -notmatch 'os\.getenv|getenv|^#|^//|^\*') {
                    $suspects += "$($file.FullName): $line"
                }
            }
    }

if ($suspects.Count -gt 0) {
    $suspects | ForEach-Object { Write-Host "    SUSPECT: $_" -ForegroundColor Yellow }
    throw 'Refusing to package: found what looks like a credential assignment. Review the lines above.'
}

Write-Note 'clean'


# ---------------------------------------------------------------------------
# 8. Report and compile the installer
# ---------------------------------------------------------------------------
$files = @(Get-ChildItem -Path $StageDir -Recurse -File -Force)
$size  = ($files | Measure-Object -Property Length -Sum).Sum
Write-Step ("Payload: {0} files, {1:N1} MB" -f $files.Count, ($size / 1MB))
Write-Note $StageDir

if ($SkipInstaller) {
    Write-Host "`nStopped before the installer (-SkipInstaller). Payload is at $StageDir" -ForegroundColor Green
    return
}

# LocalAppData first: Inno Setup's own installer offers a per-user mode
# (/CURRENTUSER) that needs no administrator rights, which is how it gets
# installed on a machine where the developer cannot elevate.
$iscc = @(
    "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe",
    "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
    "$env:ProgramFiles\Inno Setup 6\ISCC.exe"
) | Where-Object { Test-Path $_ } | Select-Object -First 1

if (-not $iscc) {
    Write-Warning 'Inno Setup 6 not found, so the installer was not compiled.'
    Write-Host    '  Install it from https://jrsoftware.org/isdl.php and re-run,'
    Write-Host    '  or compile manually:'
    Write-Host    "      ISCC.exe /DPayloadDir=`"$StageDir`" `"$IssPath`""
    Write-Host "`nThe payload itself is complete and verified at $StageDir" -ForegroundColor Green
    return
}

Write-Step 'Compiling the installer'
& $iscc "/DPayloadDir=$StageDir" $IssPath
if ($LASTEXITCODE -ne 0) { throw "ISCC failed with exit code $LASTEXITCODE" }

$setup = Join-Path $OutDir 'VortexSetup.exe'
if (Test-Path $setup) {
    $setupSize = (Get-Item $setup).Length
    Write-Host ("`nDone. {0} ({1:N1} MB)" -f $setup, ($setupSize / 1MB)) -ForegroundColor Green
    Write-Host 'Send that one file. It installs per-user with no admin prompt.' -ForegroundColor Green
}
