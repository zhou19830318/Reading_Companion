<#
  Generates the Windows relay env file (same key=value format install.sh
  writes on Linux). Called by the Inno Setup installer; idempotent so
  upgrades never rotate the device token.

  Usage:
    powershell -NoProfile -ExecutionPolicy Bypass -File write-env.ps1 `
      -EnvFile "C:\ProgramData\ReadingCompanion\voice-relay.env" `
      -DictDataDir "C:\Program Files\ReadingCompanionVoiceRelay\voice-relay\dict-data" `
      -T2IAssetDir "C:\ProgramData\ReadingCompanion\run\t2i" `
      [-DashScopeKey sk-...] [-DeviceId reading-companion-001] [-Port 8080]
#>
param(
    [Parameter(Mandatory = $true)][string]$EnvFile,
    [Parameter(Mandatory = $true)][string]$DictDataDir,
    [Parameter(Mandatory = $true)][string]$T2IAssetDir,
    [string]$DashScopeKey = "",
    [string]$DeviceId = "reading-companion-001",
    [int]$Port = 8080,
    [string]$Version = "windows"
)

$ErrorActionPreference = "Stop"

if (Test-Path -LiteralPath $EnvFile) {
    Write-Host "keeping existing $EnvFile (device token preserved)"
    exit 0
}

function New-HexToken([int]$Bytes) {
    $buffer = New-Object byte[] $Bytes
    [System.Security.Cryptography.RandomNumberGenerator]::Create().GetBytes($buffer)
    return -join ($buffer | ForEach-Object { $_.ToString("x2") })
}

New-Item -ItemType Directory -Force -Path (Split-Path -Parent $EnvFile) | Out-Null
New-Item -ItemType Directory -Force -Path $T2IAssetDir | Out-Null

$token = New-HexToken 24
$admin = New-HexToken 24

if ($DashScopeKey -ne "") {
    $providers = @"
ASR_PROVIDER=paraformer
ASR_MODEL=paraformer-realtime-v2
LLM_PROVIDER=dashscope-openai-compatible
LLM_MODEL=qwen-flash
LLM_DISABLE_THINKING=true
DASHSCOPE_API_KEY=$DashScopeKey
DASHSCOPE_REGION=cn-beijing
"@
} else {
    $providers = @"
ASR_PROVIDER=mock
LLM_PROVIDER=mock
# Mock mode: canned replies only. Add DASHSCOPE_API_KEY (and the provider
# lines above) then restart the service to switch to real speech.
"@
}

$content = @"
# Managed by the Reading Companion Windows installer.
# Secrets ONLY live here. Edit, then restart the service:
#   voice-relay-service.exe restart
RELAY_ENV=production
RELAY_BIND=0.0.0.0
RELAY_PORT=$Port
RELAY_LOG_LEVEL=INFO
RELAY_VERSION=$Version
RELAY_DEVICE_TOKEN=$token
RELAY_ADMIN_TOKEN=$admin
$providers
MAX_AUDIO_SECONDS=30
MAX_LLM_OUTPUT_TOKENS=256
MAX_REPLY_CHARS=500
SESSION_IDLE_TIMEOUT_SECONDS=60
DICT_SOURCE=local
DICT_DATA_DIR=$DictDataDir
T2I_PROVIDER=mock
T2I_ASSET_DIR=$T2IAssetDir
MAX_CONCURRENT_SESSIONS=4
MAX_WS_CONNECTIONS=32
MAX_SESSIONS_PER_DEVICE=1
"@

# File.WriteAllText writes UTF-8 without a BOM; entry.py accepts a BOM too.
[System.IO.File]::WriteAllText($EnvFile, $content)
Write-Host "created $EnvFile (device token generated)"
