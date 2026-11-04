# tools/build_firmware.ps1 — native Windows ESP-IDF build entry used by validate.sh.
#
# Git Bash exports MSYSTEM and the Windows idf.py wrapper may exit silently.
# Activating ESP-IDF inside native PowerShell avoids that and keeps the tracked
# sdkconfig.defaults as the only configuration source.
param(
    [Parameter(Mandatory = $true)][string]$BuildDir,
    [Parameter(Mandatory = $true)][string]$ProjectRoot,
    [string]$IdfPath = "C:\Espressif\frameworks\esp-idf-v5.5.3"
)

$ErrorActionPreference = "Stop"
# Git Bash may leak MSYS markers into this process; ESP-IDF refuses MSys.
Remove-Item Env:MSYSTEM -ErrorAction SilentlyContinue
Remove-Item Env:OSTYPE -ErrorAction SilentlyContinue
Remove-Item Env:MSYSTEM_PREFIX -ErrorAction SilentlyContinue
if (-not (Test-Path (Join-Path $IdfPath "export.ps1"))) {
    Write-Error "ESP-IDF export.ps1 not found under $IdfPath"
    exit 2
}
. (Join-Path $IdfPath "export.ps1") | Out-Null
$env:SDKCONFIG_DEFAULTS = Join-Path $ProjectRoot "sdkconfig.defaults"
Push-Location $ProjectRoot
try {
    idf.py -B $BuildDir -D "SDKCONFIG=$BuildDir\sdkconfig" build
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    $full = Join-Path $BuildDir "FoloToy-AI-Passport-full.bin"
    idf.py -B $BuildDir merge-bin -o $full
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    if (-not (Test-Path $full)) {
        Write-Error "merge-bin did not create $full"
        exit 3
    }
    exit 0
} finally {
    Pop-Location
}
