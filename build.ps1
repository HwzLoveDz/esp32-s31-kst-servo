[CmdletBinding()]
param([string]$BuildDirectory = 'build')
$ErrorActionPreference = 'Stop'
$workspace = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$activate = Join-Path $workspace 'tools\activate-s31.ps1'
if (Test-Path -LiteralPath $activate) {
    . $activate
    $python = Join-Path $env:IDF_PYTHON_ENV_PATH 'Scripts\python.exe'
} else {
    if (-not $env:IDF_PATH) { throw 'Activate an ESP-IDF environment with esp32s31 support first.' }
    $python = (Get-Command python -ErrorAction Stop).Source
}
$env:IDF_TARGET = 'esp32s31'
$env:PYTHONUTF8 = '1'
Push-Location -LiteralPath $PSScriptRoot
try {
    & $python (Join-Path $env:IDF_PATH 'tools\idf.py') -B $BuildDirectory --preview build
    if ($LASTEXITCODE -ne 0) { throw "Build failed: $LASTEXITCODE (outputs retained)" }
} finally { Pop-Location }
