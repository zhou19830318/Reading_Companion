<#
  Builds the self-contained Windows installer for the Voice Relay.

  Prerequisites on the build machine:
    - Python 3.11 or 3.12 on PATH (deps are installed by this script)
    - Inno Setup 6 (winget/choco: `choco install innosetup`), or set ISCC_PATH
    - Internet access the first time (WinSW download is cached afterwards)

  Usage:
    ./build.ps1 -Version v1.5.0
    ./build.ps1 -SkipInstaller          # only stage the payload for inspection

  Output:
    build/windows/out/voice-relay-setup-<version>.exe
#>
param(
    [string]$Version = "dev",
    [switch]$SkipInstaller
)
$ErrorActionPreference = "Stop"

$ScriptDir = $PSScriptRoot
$RelayRoot = (Resolve-Path (Join-Path $ScriptDir "..\..")).Path
$RepoRoot = Split-Path $RelayRoot -Parent
$BuildRoot = Join-Path $RepoRoot "build\windows"
$StageDir = Join-Path $BuildRoot "stage"
$DistDir = Join-Path $BuildRoot "pyinstaller"
$WorkDir = Join-Path $BuildRoot "work"
$CacheDir = Join-Path $BuildRoot "cache"
$OutDir = Join-Path $BuildRoot "out"

if (!(Test-Path (Join-Path $RelayRoot "dict-data\chars.json"))) {
    throw "dict-data/chars.json is missing; see relay/README.md 'Dictionary data'"
}

Write-Host "== installing python deps"
python -m pip install --disable-pip-version-check -q -r (Join-Path $RelayRoot "requirements.txt") "pyinstaller>=6,<7"

Write-Host "== pyinstaller (onedir)"
Push-Location $RelayRoot
try {
    python -m PyInstaller --clean --noconfirm --distpath $DistDir --workpath $WorkDir (Join-Path $ScriptDir "relay.spec")
    if ($LASTEXITCODE -ne 0) { throw "PyInstaller failed with exit code $LASTEXITCODE" }
} finally {
    Pop-Location
}

Write-Host "== staging payload"
Remove-Item -Recurse -Force $StageDir -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path (Join-Path $StageDir "voice-relay") | Out-Null
Copy-Item -Recurse -Force (Join-Path $DistDir "voice-relay\*") (Join-Path $StageDir "voice-relay")
Copy-Item -Recurse -Force (Join-Path $RelayRoot "dict-data") (Join-Path $StageDir "voice-relay\dict-data")

$winsw = Join-Path $CacheDir "WinSW-x64.exe"
if (!(Test-Path $winsw)) {
    New-Item -ItemType Directory -Force -Path $CacheDir | Out-Null
    Write-Host "== downloading WinSW 2.12.0"
    Invoke-WebRequest -UseBasicParsing -Uri "https://github.com/winsw/winsw/releases/download/v2.12.0/WinSW-x64.exe" -OutFile $winsw
}
Copy-Item -Force $winsw (Join-Path $StageDir "voice-relay-service.exe")
Copy-Item -Force (Join-Path $ScriptDir "relay-service.xml") (Join-Path $StageDir "voice-relay-service.xml")
Copy-Item -Force (Join-Path $ScriptDir "write-env.ps1") (Join-Path $StageDir "write-env.ps1")

if ($SkipInstaller) {
    Write-Host "== stage ready: $StageDir"
    exit 0
}

$iscc = $env:ISCC_PATH
if (!$iscc -or !(Test-Path $iscc)) {
    $candidates = @(
        "C:\Program Files (x86)\Inno Setup 6\ISCC.exe",
        "C:\Program Files\Inno Setup 6\ISCC.exe"
    )
    $iscc = $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
}
if (!$iscc) {
    throw "ISCC.exe not found; install Inno Setup 6 (choco install innosetup) or set ISCC_PATH"
}

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
Write-Host "== building installer with Inno Setup"
& $iscc "/DAppVersion=$Version" "/DStageDir=$StageDir" "/DOutDir=$OutDir" (Join-Path $ScriptDir "installer.iss")
if ($LASTEXITCODE -ne 0) { throw "ISCC failed with exit code $LASTEXITCODE" }

$installer = Get-ChildItem (Join-Path $OutDir "*.exe") | Sort-Object LastWriteTime -Descending | Select-Object -First 1
Write-Host "== installer: $($installer.FullName) ($([math]::Round($installer.Length / 1MB, 1)) MB)"
