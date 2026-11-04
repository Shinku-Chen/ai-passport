# tools/nn_build.ps1 -- build the reader firmware (build only, never flashes).
#
# IDF location resolution order (so a single machine's C:\ install is not baked in):
#   1. explicit parameters -IdfPath / -IdfPython / -ExportScript
#   2. already-activated environment variables IDF_PATH / IDF_PYTHON_ENV_PATH
#   3. the local install used during development (fallback, noted in the log)
#
# Usage:
#   powershell -File tools/nn_build.ps1
#   powershell -File tools/nn_build.ps1 -IdfPath D:\esp\esp-idf -IdfPython D:\esp\python_env\idf5.5_py3.14_env
#   powershell -File tools/nn_build.ps1 -BuildDir build\fw-second
#
# This file is intentionally ASCII only: Windows PowerShell 5.1 reads BOM-less
# UTF-8 as ANSI, and non-ASCII text inside string literals then breaks parsing.
param(
    [string]$IdfPath = $env:IDF_PATH,
    [string]$IdfPython = $env:IDF_PYTHON_ENV_PATH,
    [string]$ExportScript = "",
    [string]$BuildDir = "build\fw",
    [string]$LogDir = "build\logs"
)

# Native tools (export.ps1, idf.py) write progress to stderr; keep the session
# non-terminating and judge results by $LASTEXITCODE instead of stderr text.
$ErrorActionPreference = "Continue"
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$logDir = Join-Path $root $LogDir
New-Item -ItemType Directory -Force -Path $logDir | Out-Null

$fallback = $false
if (-not $IdfPath) {
    $IdfPath = "C:\Espressif\frameworks\esp-idf-v5.5.3"
    $fallback = $true
}
if (-not $IdfPython) {
    $IdfPython = "C:\Espressif\python_env\idf5.5_py3.14_env"
    $fallback = $true
}
if (-not $env:IDF_TOOLS_PATH) { $env:IDF_TOOLS_PATH = "C:\Espressif" }
if (-not $ExportScript) { $ExportScript = Join-Path $IdfPath "export.ps1" }

$env:IDF_PATH = $IdfPath
$env:IDF_PYTHON_ENV_PATH = $IdfPython

$idfPy = Join-Path $IdfPath "tools\idf.py"
$idfPythonExe = Join-Path $IdfPython "Scripts\python.exe"

$missing = @()
foreach ($p in @($IdfPath, $IdfPython, $idfPy, $idfPythonExe)) {
    if (-not (Test-Path $p)) { $missing += $p }
}
if ($missing.Count -gt 0) {
    $msg = "ESP-IDF installation is incomplete, missing:" + [Environment]::NewLine
    $msg += ($missing -join [Environment]::NewLine)
    $msg += [Environment]::NewLine + "Pass -IdfPath/-IdfPython, or activate ESP-IDF 5.5.3 first."
    Write-Host $msg -ForegroundColor Red
    exit 2
}

$exportLog = Join-Path $logDir "idf-export.log"
$src = "env-or-argument"
if ($fallback) { $src = "fallback-local-install" }
@(
    "idf_path=$IdfPath",
    "idf_python=$IdfPython",
    "idf_tools_path=$env:IDF_TOOLS_PATH",
    "build_dir=$BuildDir",
    "path_source=$src"
) | Out-File -FilePath $exportLog -Encoding utf8
& $ExportScript *>&1 | Out-File -FilePath $exportLog -Encoding utf8 -Append

$buildLog = Join-Path $logDir "idf-build.log"
Push-Location $root
try {
    $buildAbs = Join-Path $root $BuildDir
    & $idfPythonExe $idfPy -B $buildAbs build *>&1 | Out-File -FilePath $buildLog -Encoding utf8
    $code = $LASTEXITCODE
    "EXIT=$code" | Out-File -FilePath $buildLog -Encoding utf8 -Append
    exit $code
} finally {
    Pop-Location
}
