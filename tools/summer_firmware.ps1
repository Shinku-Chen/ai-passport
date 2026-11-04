param(
    [string]$BuildDir = "",
    [string]$IdfPath = "",
    [string]$ArchiveRoot = ""
)
# Summer Pockets clean firmware gate for Windows.
#
# ESP-IDF refuses to run inside Git Bash/MSYS, so this native PowerShell script
# performs the same steps as tools/validate.sh --firmware: fresh build dir ->
# merge-bin -> verify_firmware.py -> archive_firmware.py create + verify.
# Every run gets a timestamped directory; nothing is deleted.
#
#   powershell -File tools/summer_firmware.ps1
#   powershell -File tools/summer_firmware.ps1 -IdfPath C:\Espressif\frameworks\esp-idf-v5.5.3
$ErrorActionPreference = "Continue"
$PSDefaultParameterValues['Out-File:Encoding'] = 'utf8'
$repo = Split-Path -Parent $PSScriptRoot
Set-Location $repo

if (-not $IdfPath) {
    $IdfPath = $env:IDF_PATH
}
if (-not $IdfPath) {
    Write-Output "SUMMER_FIRMWARE_FAIL step=idf-not-specified hint='pass -IdfPath or set IDF_PATH to an ESP-IDF 5.5.3 tree'"
    exit 2
}
if (-not (Test-Path (Join-Path $IdfPath 'export.ps1'))) {
    Write-Output "SUMMER_FIRMWARE_FAIL step=idf-not-found path=$IdfPath"
    exit 2
}
if (-not $ArchiveRoot) {
    $ArchiveRoot = "build/firmware"
}

$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$runRoot = "build/firmware-gate"
if (-not $BuildDir) {
    $BuildDir = "$runRoot/run-$stamp"
}
if (Test-Path $BuildDir) {
    Write-Output "SUMMER_FIRMWARE_FAIL step=build-dir-exists path=$BuildDir"
    exit 1
}
New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null
$logDir = "build/logs/summer-firmware-$stamp"
New-Item -ItemType Directory -Force -Path $logDir | Out-Null

$env:MSYSTEM = $null
$env:MINGW_PREFIX = $null
. (Join-Path $IdfPath 'export.ps1') *> (Join-Path $logDir 'export.log')
$env:SDKCONFIG_DEFAULTS = "$repo\sdkconfig.defaults"
$env:IDF_CCACHE_ENABLE = '1'

$log = Join-Path $logDir 'build.log'
idf.py -B $BuildDir -D "SDKCONFIG=$repo\$BuildDir\sdkconfig" build *> $log
Get-Content $log -Tail 5
if ($LASTEXITCODE -ne 0) {
    Write-Output "SUMMER_FIRMWARE_FAIL step=build log=$log"
    exit 1
}

$mergeLog = Join-Path $logDir 'merge.log'
$full = Join-Path $repo "$BuildDir/FoloToy-AI-Passport-full.bin"
idf.py -B $BuildDir merge-bin -o $full *> $mergeLog
Get-Content $mergeLog -Tail 4
if ($LASTEXITCODE -ne 0) {
    Write-Output "SUMMER_FIRMWARE_FAIL step=merge log=$mergeLog"
    exit 1
}

$verifyLog = Join-Path $logDir 'verify.log'
python tools/verify_firmware.py $BuildDir *> $verifyLog
Get-Content $verifyLog -Tail 4
if ($LASTEXITCODE -ne 0) {
    Write-Output "SUMMER_FIRMWARE_FAIL step=verify log=$verifyLog"
    exit 1
}

$archiveLog = Join-Path $logDir 'archive.log'
$archiveOut = python tools/archive_firmware.py create $BuildDir --archive-root $ArchiveRoot *>&1
$archiveOut | Out-File -FilePath $archiveLog
$archiveOut | Select-Object -Last 4
if ($LASTEXITCODE -ne 0) {
    Write-Output "SUMMER_FIRMWARE_FAIL step=archive log=$archiveLog"
    exit 1
}
$archive = ($archiveOut | Select-String -Pattern 'Firmware debug archive:\s*(.+)$' | Select-Object -Last 1)
$archivePath = $null
if ($archive) {
    $archivePath = $archive.Matches[0].Groups[1].Value.Trim()
}

if ($archivePath) {
    $archiveVerifyLog = Join-Path $logDir 'archive-verify.log'
    python tools/archive_firmware.py verify $archivePath *> $archiveVerifyLog
    Get-Content $archiveVerifyLog -Tail 3
    if ($LASTEXITCODE -ne 0) {
        Write-Output "SUMMER_FIRMWARE_FAIL step=archive-verify log=$archiveVerifyLog"
        exit 1
    }
}

$app = Join-Path $repo "$BuildDir/FoloToy-AI-Passport.bin"
$appSize = (Get-Item $app).Length
$fullSize = (Get-Item $full).Length
Write-Output "SUMMER_FIRMWARE_PASS build=$BuildDir app=$appSize full=$fullSize archive=$archivePath logs=$logDir"
exit 0
