param([Parameter(ValueFromRemainingArguments = $true)][string[]]$IdfArgs)
# Maintained ESP-IDF entry point for Windows:
#   powershell -ExecutionPolicy Bypass -File tools/idf.ps1 -B build <cmd>
#
# ESP-IDF 位置按以下顺序发现(不写死个人安装):
#   1) $env:IDF_PATH(若已 export)
#   2) $env:STEINS_IDF_PATH
#   3) C:\Espressif\frameworks\esp-idf-v5.5.3(本机默认安装)
if (-not $env:IDF_PATH) {
    foreach ($candidate in @($env:STEINS_IDF_PATH,
                            'C:\Espressif\frameworks\esp-idf-v5.5.3')) {
        if ($candidate -and (Test-Path (Join-Path $candidate 'export.ps1'))) {
            $env:IDF_PATH = $candidate
            break
        }
    }
}
if (-not $env:IDF_PATH) {
    Write-Output "IDF_NOT_FOUND"
    exit 1
}
# 从 Git Bash 启动 PowerShell 时 MSYSTEM 会把 IDF 的 export.ps1 顶回来,先清掉。
$env:MSYSTEM = ''
. (Join-Path $env:IDF_PATH 'export.ps1') *> $null
$env:IDF_CCACHE_ENABLE = '1'
idf.py @IdfArgs
exit $LASTEXITCODE
