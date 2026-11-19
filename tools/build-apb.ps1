# Builds apb in one go: the recompiler, the lift of both CPUs from your
# ROMs, then the game. Run it again after a `git pull`: only what changed is
# rebuilt (the lift leaves unchanged files alone).
#
#   powershell -ExecutionPolicy Bypass -File tools\build-apb.ps1 [-RomDir <folder>] [-Run]
#
# On Windows, run it from "Developer PowerShell for VS 2022" (it needs
# Visual Studio 2022 17.10 or newer and its CMake). With no -RomDir it uses
# the folder apb last used (apb.ini), else asks. -Run starts the game after.
# The lifted code goes to gen\ and gen-sound\, which git ignores: it's made
# from your ROMs and never leaves your computer.
param(
    [string]$RomDir = "",
    [switch]$Run
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$onWindows = ($PSVersionTable.PSEdition -eq "Desktop") -or $IsWindows   # Windows PowerShell 5.1 has no $IsWindows

function Invoke-Step([string]$what, [string]$exe, [string[]]$arguments) {
    Write-Host "== $what" -ForegroundColor Cyan
    & $exe @arguments
    if ($LASTEXITCODE -ne 0) { throw "$what failed (exit code $LASTEXITCODE)" }
}

# The first of these paths that exists.
function Find-Built([string[]]$candidates) {
    foreach ($c in $candidates) { if (Test-Path $c) { return $c } }
    throw "built, but none of these exists: $($candidates -join ', ')"
}

if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    Write-Host "CMake isn't on the PATH. Open 'Developer PowerShell for VS 2022' from the Start menu and run this again." -ForegroundColor Red
    exit 1
}

if (-not $RomDir) {
    if ($onWindows) {
        $ini = Join-Path $env:APPDATA "Qilin\apb\apb.ini"
    } else {
        $data = $env:XDG_DATA_HOME
        if (-not $data) { $data = Join-Path $HOME ".local/share" }
        $ini = Join-Path $data "Qilin/apb/apb.ini"
    }
    if (Test-Path $ini) {
        $line = Select-String -Path $ini -Pattern '^rom_dir = (.+)$' | Select-Object -First 1
        if ($line) { $RomDir = $line.Matches[0].Groups[1].Value.Trim() }
    }
}
if (-not $RomDir) { $RomDir = Read-Host "The folder with your APB ROMs" }
if (-not (Test-Path $RomDir -PathType Container)) { throw "no such folder: $RomDir" }

$build = Join-Path $root "build-apb"
$gen = (Join-Path $root "gen") -replace '\\', '/'
$genSound = (Join-Path $root "gen-sound") -replace '\\', '/'
$configure = @("-S", $root, "-B", $build)
if ($onWindows -and -not (Test-Path (Join-Path $build "CMakeCache.txt"))) { $configure += @("-G", "Visual Studio 17 2022", "-A", "x64") }
if (-not $onWindows) { $configure += "-DCMAKE_BUILD_TYPE=Release" }

Invoke-Step "Configuring" "cmake" $configure
Invoke-Step "Building the recompiler" "cmake" @("--build", $build, "--config", "Release", "--target", "qilin", "--parallel")
$exe = if ($onWindows) { ".exe" } else { "" }
$qilin = Find-Built @((Join-Path $build "Release/qilin$exe"), (Join-Path $build "qilin$exe"))
Invoke-Step "Checking the ROMs" $qilin @("verify", $RomDir)
Invoke-Step "Lifting the main CPU (T-11)" $qilin @("lift", $RomDir, "--out", $gen)
Invoke-Step "Lifting the sound CPU (6502)" $qilin @("lift", $RomDir, "--cpu", "sound", "--out", $genSound)
Invoke-Step "Configuring with the lifted code" "cmake" @("-S", $root, "-B", $build, "-DQILIN_GEN_DIR=$gen", "-DQILIN_SOUND_GEN_DIR=$genSound")
Invoke-Step "Building apb (the first time takes a while)" "cmake" @("--build", $build, "--config", "Release", "--target", "apb", "--parallel")
$apb = Find-Built @((Join-Path $build "Release/apb$exe"), (Join-Path $build "apb$exe"))

Write-Host "Built: $apb" -ForegroundColor Green
Write-Host "Run it with:  & `"$apb`"   (it remembers the ROM folder)"
if ($Run) { & $apb $RomDir }
