# Builds apb in one go: the recompiler, the lift of both CPUs from your
# ROMs, then the game. Run it again after a `git pull`: only what changed is
# rebuilt (the lift leaves unchanged files alone).
#
#   powershell -ExecutionPolicy Bypass -File tools\build-apb.ps1 [-RomDir <folder>] [-Run] [-Pull]
#
# On Windows, "Play APB.bat" and "Update APB.bat" in the project's folder run
# it for you: double-click them. It needs Visual Studio 2022 17.10 or newer
# (with "Desktop development with C++") and finds its CMake and Git itself,
# from any window. With no -RomDir it uses the folder apb last used
# (apb.ini), else asks with a folder picker. -Run starts the game after;
# -Pull fetches the latest code first.
# The lifted code goes to gen\ and gen-sound\, which git ignores: it's made
# from your ROMs and never leaves your computer.
param(
    [string]$RomDir = "",
    [switch]$Run,
    [switch]$Pull      # git pull first (Update APB.bat)
)
$ErrorActionPreference = "Stop"
trap {   # any failure: say what in one red line, and end with an error code
    Write-Host ""
    Write-Host "Stopped: $_" -ForegroundColor Red
    exit 1
}
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

# CMake and Git: on the PATH, else the ones Visual Studio installs, else the usual places.
function Add-Tool([string]$name, [string[]]$folders) {
    if (Get-Command $name -ErrorAction SilentlyContinue) { return $true }
    foreach ($f in $folders) {
        if ($f -and (Test-Path (Join-Path $f "$name.exe"))) {
            $env:PATH = "$f;$env:PATH"
            return $true
        }
    }
    return $false
}
if ($onWindows) {
    $vs = ""
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) { $vs = & $vswhere -latest -products * -property installationPath | Select-Object -First 1 }
    $ide = if ($vs) { Join-Path $vs "Common7\IDE\CommonExtensions\Microsoft" } else { "" }
    $haveCmake = Add-Tool "cmake" @($(if ($ide) { Join-Path $ide "CMake\CMake\bin" }), (Join-Path $env:ProgramFiles "CMake\bin"))
    $haveGit = Add-Tool "git" @((Join-Path $env:ProgramFiles "Git\cmd"), $(if ($ide) { Join-Path $ide "TeamFoundation\Team Explorer\Git\cmd" }))
    if (-not $vs) { throw "Visual Studio 2022 wasn't found. Install it (the free Community edition) with 'Desktop development with C++'." }
    if (-not $haveCmake) { throw "CMake wasn't found. In the Visual Studio Installer, add 'C++ CMake tools for Windows'." }
    if (-not $haveGit) { throw "Git wasn't found. Install Git for Windows (git-scm.com), or add 'Git for Windows' in the Visual Studio Installer." }
} elseif (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    throw "CMake isn't installed."
}

if ($Pull) {
    Write-Host "== Getting the latest code" -ForegroundColor Cyan
    & git -C $root pull --ff-only
    if ($LASTEXITCODE -ne 0) { throw "git pull failed (exit code $LASTEXITCODE). If you changed files in the project folder, that's why." }
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
if (-not $RomDir -and $onWindows) {
    Add-Type -AssemblyName System.Windows.Forms
    $dialog = New-Object System.Windows.Forms.FolderBrowserDialog
    $dialog.Description = "Choose the folder that holds your APB ROMs (the rev 7 set, 36 files)"
    $dialog.ShowNewFolderButton = $false
    if ($dialog.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) { $RomDir = $dialog.SelectedPath }
    else { throw "no ROM folder chosen" }
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
if ($Run) {
    Write-Host "Starting APB..."
    Start-Process -FilePath $apb -ArgumentList "`"$RomDir`"" -WorkingDirectory (Split-Path -Parent $apb)
} else {
    Write-Host "Run it with:  & `"$apb`"   (it remembers the ROM folder)"
}
