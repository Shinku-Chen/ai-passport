param([Parameter(ValueFromRemainingArguments = $true)][string[]]$IdfArgs)
# Maintained ESP-IDF entry point for Windows:
#   powershell -File tools/idf.ps1 -B build build
# Activates the local ESP-IDF 5.5.3 installation and forwards all arguments.
. 'C:\Espressif\frameworks\esp-idf-v5.5.3\export.ps1' *> $null
$env:IDF_CCACHE_ENABLE = '1'
idf.py @IdfArgs
exit $LASTEXITCODE
