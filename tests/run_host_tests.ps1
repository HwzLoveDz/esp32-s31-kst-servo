param(
    [string]$Compiler = 'gcc',
    [switch]$Wsl,
    [string]$WslDistribution = 'Ubuntu-24.04'
)
$ErrorActionPreference = 'Stop'
$taskCompilerCommand = Get-Command $Compiler -ErrorAction SilentlyContinue
if ($Wsl -or $null -eq $taskCompilerCommand) {
    Get-Command wsl.exe -ErrorAction Stop | Out-Null
    $taskLinuxPath = & wsl.exe -d $WslDistribution --exec wslpath -a -u $PSScriptRoot
    if ($LASTEXITCODE -ne 0) { throw 'Could not translate test directory for WSL.' }
    & wsl.exe -d $WslDistribution --exec sh "$taskLinuxPath/run_host_tests.sh"
    if ($LASTEXITCODE -ne 0) { throw "WSL protocol tests failed with exit code $LASTEXITCODE" }
    exit 0
}
$taskCompiler = $taskCompilerCommand.Source
$taskBuildDir = Join-Path $PSScriptRoot 'build-host'
$taskExecutable = Join-Path $taskBuildDir 'test_kst_protocol.exe'
New-Item -ItemType Directory -Path $taskBuildDir -Force | Out-Null
& $taskCompiler -std=c99 -Wall -Wextra -Werror -pedantic `
    "-I$PSScriptRoot/../components/kst_protocol/include" `
    "$PSScriptRoot/test_kst_protocol.c" `
    "$PSScriptRoot/../components/kst_protocol/kst_protocol.c" `
    -o $taskExecutable
if ($LASTEXITCODE -ne 0) { throw "C compilation failed with exit code $LASTEXITCODE" }
& $taskExecutable
if ($LASTEXITCODE -ne 0) { throw "Protocol tests failed with exit code $LASTEXITCODE" }
$taskDriverExecutable = Join-Path $taskBuildDir 'test_kst_servo.exe'
& $taskCompiler -std=c99 -Wall -Wextra -Werror -pedantic `
    "-I$PSScriptRoot/../components/kst_protocol/include" `
    "-I$PSScriptRoot/../components/kst_servo/include" `
    "$PSScriptRoot/test_kst_servo.c" `
    "$PSScriptRoot/../components/kst_servo/kst_servo.c" `
    "$PSScriptRoot/../components/kst_protocol/kst_protocol.c" `
    -o $taskDriverExecutable
if ($LASTEXITCODE -ne 0) { throw "Driver compilation failed with exit code $LASTEXITCODE" }
& $taskDriverExecutable
if ($LASTEXITCODE -ne 0) { throw "Driver tests failed with exit code $LASTEXITCODE" }
$taskControllerExecutable = Join-Path $taskBuildDir 'test_app_controller.exe'
& $taskCompiler -std=c11 -Wall -Wextra -Werror -pedantic `
    "-I$PSScriptRoot/fakes" `
    "-I$PSScriptRoot/../components/kst_protocol/include" `
    "-I$PSScriptRoot/../components/kst_servo/include" `
    "-I$PSScriptRoot/../components/kst_twai/include" `
    "$PSScriptRoot/test_app_controller.c" `
    "$PSScriptRoot/../components/kst_servo/kst_servo.c" `
    "$PSScriptRoot/../components/kst_protocol/kst_protocol.c" `
    -o $taskControllerExecutable
if ($LASTEXITCODE -ne 0) { throw "Controller compilation failed with exit code $LASTEXITCODE" }
& $taskControllerExecutable
if ($LASTEXITCODE -ne 0) { throw "Controller tests failed with exit code $LASTEXITCODE" }
$taskUsbControllerExecutable = Join-Path $taskBuildDir 'test_app_controller_usb.exe'
& $taskCompiler -std=c11 -Wall -Wextra -Werror -pedantic -DCONFIG_KST_TRANSPORT_USB_GS=1 `
    "-I$PSScriptRoot/fakes" `
    "-I$PSScriptRoot/../components/kst_protocol/include" `
    "-I$PSScriptRoot/../components/kst_servo/include" `
    "-I$PSScriptRoot/../components/kst_usb_gs/include" `
    "$PSScriptRoot/test_app_controller.c" `
    "$PSScriptRoot/../components/kst_servo/kst_servo.c" `
    "$PSScriptRoot/../components/kst_protocol/kst_protocol.c" `
    -o $taskUsbControllerExecutable
if ($LASTEXITCODE -ne 0) { throw "USB controller compilation failed: $LASTEXITCODE" }
& $taskUsbControllerExecutable
if ($LASTEXITCODE -ne 0) { throw "USB controller tests failed: $LASTEXITCODE" }
$taskGsExecutable = Join-Path $taskBuildDir 'test_gs_usb_protocol.exe'
& $taskCompiler -std=c99 -Wall -Wextra -Werror -pedantic `
    "-I$PSScriptRoot/../components/gs_usb_protocol/include" `
    "$PSScriptRoot/test_gs_usb_protocol.c" `
    "$PSScriptRoot/../components/gs_usb_protocol/gs_usb_protocol.c" `
    -o $taskGsExecutable
if ($LASTEXITCODE -ne 0) { throw "gs_usb compilation failed: $LASTEXITCODE" }
& $taskGsExecutable
if ($LASTEXITCODE -ne 0) { throw "gs_usb tests failed: $LASTEXITCODE" }
$taskUsbHostExecutable = Join-Path $taskBuildDir 'test_kst_usb_gs.exe'
& $taskCompiler -std=c11 -Wall -Wextra -Werror -pedantic `
    "-I$PSScriptRoot/fakes_usb" `
    "-I$PSScriptRoot/../components/kst_protocol/include" `
    "-I$PSScriptRoot/../components/kst_servo/include" `
    "-I$PSScriptRoot/../components/kst_usb_gs/include" `
    "-I$PSScriptRoot/../components/gs_usb_protocol/include" `
    "$PSScriptRoot/test_kst_usb_gs.c" `
    "$PSScriptRoot/../components/gs_usb_protocol/gs_usb_protocol.c" `
    -o $taskUsbHostExecutable
if ($LASTEXITCODE -ne 0) { throw "USB Host compilation failed: $LASTEXITCODE" }
& $taskUsbHostExecutable
if ($LASTEXITCODE -ne 0) { throw "USB Host tests failed: $LASTEXITCODE" }
