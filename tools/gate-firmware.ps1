param(
    [string]$BuildDir = "",
    [string]$LogDir = "build/logs"
)
# Maintained firmware gate for Windows (ESP-IDF 5.5 refuses to run under MSYS,
# so tools/validate.sh --firmware cannot execute inside Git Bash).
#
#   powershell -File tools/gate-firmware.ps1
#
# Steps mirror validate.sh --firmware: clean build -> merge-bin -> verify ->
# archive. Each run gets its own directory under build/firmware-gate/; nothing
# is ever deleted, so previous evidence stays on disk.
$ErrorActionPreference = "Continue"   # export.ps1 writes progress to stderr
$PSDefaultParameterValues['Out-File:Encoding'] = 'utf8'
$repo = (Get-Location).Path
New-Item -ItemType Directory -Force -Path $LogDir | Out-Null

$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$runRoot = Join-Path $repo "build/firmware-gate"
New-Item -ItemType Directory -Force -Path $runRoot | Out-Null
if (-not $BuildDir) {
    $BuildDir = "build/firmware-gate/run-$stamp"
}
if (Test-Path $BuildDir) {
    Write-Output "BUILD_DIR_EXISTS: $BuildDir"
    exit 1
}
New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null
$runLog = Join-Path $LogDir "firmware-gate-$stamp"
New-Item -ItemType Directory -Force -Path $runLog | Out-Null

. 'C:\Espressif\frameworks\esp-idf-v5.5.3\export.ps1' *> $null
$env:SDKCONFIG_DEFAULTS = "$repo\sdkconfig.defaults"
$env:IDF_CCACHE_ENABLE = '1'

$log = Join-Path $runLog "build.log"
& idf.py -B $BuildDir -D "SDKCONFIG=$repo\$BuildDir\sdkconfig" build *>&1 | Out-File -FilePath $log
Get-Content $log -Tail 6
if ($LASTEXITCODE -ne 0) { Write-Output "BUILD_FAIL log=$log"; exit 1 }

$mergeLog = Join-Path $runLog "merge.log"
& idf.py -B $BuildDir merge-bin -o "$repo\$BuildDir\FoloToy-AI-Passport-full.bin" *>&1 |
    Out-File -FilePath $mergeLog
Get-Content $mergeLog -Tail 6
if ($LASTEXITCODE -ne 0) { Write-Output "MERGE_FAIL log=$mergeLog"; exit 1 }

$verifyLog = Join-Path $runLog "verify.log"
python tools/verify_firmware.py $BuildDir *>&1 | Out-File -FilePath $verifyLog
Get-Content $verifyLog -Tail 8
if ($LASTEXITCODE -ne 0) { Write-Output "VERIFY_FAIL log=$verifyLog"; exit 1 }

$archiveLog = Join-Path $runLog "archive.log"
python tools/archive_firmware.py create $BuildDir --archive-root "$repo\build\firmware" *>&1 |
    Out-File -FilePath $archiveLog
Get-Content $archiveLog -Tail 8
if ($LASTEXITCODE -ne 0) { Write-Output "ARCHIVE_FAIL log=$archiveLog"; exit 1 }

Copy-Item -Force "$BuildDir\FoloToy-AI-Passport-full.bin" "build\FoloToy-AI-Passport-full.bin"
Write-Output "FIRMWARE_GATE_PASS build=$BuildDir logs=$runLog"
