param(
    [string]$BuildDir = "",
    [string]$LogDir = "build/logs"
)
# Windows firmware gate for the Steins reader. ESP-IDF refuses to run under MSYS,
# so tools/validate.sh --firmware cannot execute inside Git Bash; this script
# mirrors it: clean build -> merge-bin -> verify -> archive.
#
#   powershell -ExecutionPolicy Bypass -File tools/gate-firmware.ps1
#
# Every run gets its own directory under build/firmware-gate/; nothing is deleted,
# so earlier evidence stays on disk.
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

# ESP-IDF 位置发现(不写死个人安装)
if (-not $env:IDF_PATH) {
    foreach ($candidate in @($env:STEINS_IDF_PATH,
                            'C:\Espressif\frameworks\esp-idf-v5.5.3')) {
        if ($candidate -and (Test-Path (Join-Path $candidate 'export.ps1'))) {
            $env:IDF_PATH = $candidate
            break
        }
    }
}
if (-not $env:IDF_PATH) { Write-Output "IDF_NOT_FOUND"; exit 1 }
$env:MSYSTEM = ''
. (Join-Path $env:IDF_PATH 'export.ps1') *> $null
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

$archiveVerify = Join-Path $runLog "archive-verify.log"
$archivePath = (Select-String -Path $archiveLog -Pattern 'build[/\\]firmware[/\\][0-9a-f]{64}' |
    Select-Object -First 1).Matches.Value
if (-not $archivePath) {
    Write-Output "ARCHIVE_PATH_UNKNOWN log=$archiveLog"
    exit 1
}
python tools/archive_firmware.py verify "$repo\$archivePath" *>&1 | Out-File -FilePath $archiveVerify
Get-Content $archiveVerify -Tail 6
if ($LASTEXITCODE -ne 0) { Write-Output "ARCHIVE_VERIFY_FAIL log=$archiveVerify"; exit 1 }

Copy-Item -Force "$BuildDir\FoloToy-AI-Passport-full.bin" "build\FoloToy-AI-Passport-full.bin"
Write-Output "FIRMWARE_GATE_PASS build=$BuildDir logs=$runLog archive=$archivePath"
