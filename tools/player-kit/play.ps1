# Play APB (the Windows download's "Play APB.bat" runs this).
#
# The download holds everything but the game's own code: that is made here,
# from your ROMs, the first time (and again after an update): qilin lifts
# both CPUs' programs into C++, the bundled compiler (LLVM-MinGW) compiles
# them, and they're linked with the prebuilt rest into APB.exe. Then, and
# every time after, it starts APB.exe.
#
#   play.ps1 [-Rebuild] [-BuildOnly]
#
# -Rebuild builds again even if APB.exe is up to date; -BuildOnly doesn't
# start the game afterwards.
param(
    [switch]$Rebuild,
    [switch]$BuildOnly
)
$ErrorActionPreference = "Stop"
trap {
    Write-Host ""
    Write-Host "Stopped: $_" -ForegroundColor Red
    exit 1
}

$kit = $PSScriptRoot
$top = Split-Path -Parent $kit
$work = Join-Path $top "work"
$game = Join-Path $top "APB.exe"
$stamp = Join-Path $work "built-with.txt"
$kitVersion = (Get-Content (Join-Path $kit "VERSION") -Raw).Trim()
$onWindows = ($PSVersionTable.PSEdition -eq "Desktop") -or $IsWindows   # Windows PowerShell 5.1 has no $IsWindows
# Off Windows (testing the download under Wine), QILIN_KIT_RUNNER runs the kit's programs.
$runner = $env:QILIN_KIT_RUNNER

# Runs one of the kit's programs, its output to the window; returns its exit code.
function Invoke-Kit([string]$exe, [string[]]$arguments) {
    if ($runner) { & $runner $exe @arguments | Out-Host } else { & $exe @arguments | Out-Host }
    return $LASTEXITCODE
}
# The same, its output (error output too) returned as text, with the exit code.
function Read-Kit([string]$exe, [string[]]$arguments) {
    $saved = $ErrorActionPreference
    $ErrorActionPreference = "Continue"   # Windows PowerShell 5.1 turns redirected error output into errors
    $text = if ($runner) { & $runner $exe @arguments 2>&1 | Out-String } else { & $exe @arguments 2>&1 | Out-String }
    $code = $LASTEXITCODE
    $ErrorActionPreference = $saved
    return @{ text = $text; code = $code }
}

# Up to date: just play.
$built = if (Test-Path $stamp) { (Get-Content $stamp -Raw).Trim() } else { "" }
if (-not $Rebuild -and (Test-Path $game) -and $built -eq $kitVersion) {
    if (-not $BuildOnly) { Start-Process -FilePath $game -WorkingDirectory $top }
    exit 0
}

Write-Host "APB QilinComp $kitVersion" -ForegroundColor Cyan
if (Test-Path $game) { Write-Host "Updating the game for this version: a few minutes." }
else { Write-Host "Building the game from your ROMs. This happens once (and after an update): a few minutes." }
Write-Host ""

# The ROM folder: the one APB used last, else ask (until one holds a verified set).
$qilin = Join-Path $kit "qilin.exe"
$settingsFolder = if ($onWindows) { Join-Path $env:APPDATA "Qilin\apb" }
                  elseif ($env:XDG_DATA_HOME) { Join-Path $env:XDG_DATA_HOME "Qilin/apb" }
                  else { Join-Path $HOME ".local/share/Qilin/apb" }
$romDir = ""
$ini = Join-Path $settingsFolder "apb.ini"
if (Test-Path $ini) {
    $line = Select-String -Path $ini -Pattern '^rom_dir = (.+)$' | Select-Object -First 1
    if ($line) { $romDir = $line.Matches[0].Groups[1].Value.Trim() }
}
while ($true) {
    if ($romDir) {
        $report = Read-Kit $qilin @("verify", $romDir)
        if ($report.code -eq 0) { break }
        Write-Host "The ROMs in $romDir aren't a complete rev 7 set:" -ForegroundColor Yellow
        Write-Host (($report.text -split "`n" | Select-Object -First 6) -join "`n")
    }
    if (-not $onWindows) { throw "no ROM folder: set rom_dir in $ini" }
    Add-Type -AssemblyName System.Windows.Forms
    $dialog = New-Object System.Windows.Forms.FolderBrowserDialog
    $dialog.Description = "Choose the folder that holds your APB ROMs (the rev 7 set: 36 files, as MAME's apb)"
    $dialog.ShowNewFolderButton = $false
    if ($dialog.ShowDialog() -ne [System.Windows.Forms.DialogResult]::OK) {
        Write-Host "No ROM folder chosen."
        exit 0
    }
    $romDir = $dialog.SelectedPath
}
Write-Host "ROMs: $romDir"

# Lift both CPUs' programs (unchanged files are left alone).
New-Item -ItemType Directory -Force -Path $work | Out-Null
Write-Host "== Recompiling the main CPU's program" -ForegroundColor Cyan
if ((Invoke-Kit $qilin @("lift", $romDir, "--out", (Join-Path $work "gen"))) -ne 0) { throw "qilin lift failed" }
Write-Host "== Recompiling the sound CPU's program" -ForegroundColor Cyan
if ((Invoke-Kit $qilin @("lift", $romDir, "--cpu", "sound", "--out", (Join-Path $work "gen-sound"))) -ne 0) { throw "qilin lift --cpu sound failed" }

# Compile and link: a ninja file over what the lift wrote, with the kit's
# flags and libraries (kit.ninja). Paths are relative to the work folder, so
# spaces in where the download was unpacked don't matter.
$sources = @(Get-ChildItem (Join-Path $work "gen") -Filter *.cpp) + @(Get-ChildItem (Join-Path $work "gen-sound") -Filter *.cpp)
$lines = @(
    "# Written by play.ps1: the game's lifted code, compiled against the kit.",
    "kit = ../kit",
    "include ../kit/kit.ninja",
    "cxx = ..\kit\toolchain\bin\x86_64-w64-mingw32-clang++.exe",
    "rule cxx",
    "  command = `$cxx `$cxxflags -MD -MF `$out.d -c `$in -o `$out",
    "  depfile = `$out.d",
    "  deps = gcc",
    "  description = Compiling `$in",
    "rule link",
    "  command = `$cxx `$ldflags -o `$out `$in `$libs",
    "  description = Linking `$out"
)
$objects = @()
foreach ($s in $sources) {
    $folder = Split-Path -Leaf $s.DirectoryName
    $object = "obj/$folder/$($s.BaseName).o"
    $objects += $object
    $lines += "build ${object}: cxx $folder/$($s.Name)"
}
$lines += "build APB.exe: link $($objects -join ' ') | ../kit/lib/libapb_kit.a"
$lines += "default APB.exe"
Set-Content -Path (Join-Path $work "build.ninja") -Value $lines -Encoding ASCII

Write-Host "== Compiling ($($sources.Count) files)" -ForegroundColor Cyan
if ((Invoke-Kit (Join-Path $kit "ninja.exe") @("-C", $work)) -ne 0) { throw "compiling failed" }
Copy-Item (Join-Path $work "APB.exe") $game -Force
Set-Content -Path $stamp -Value $kitVersion -Encoding ASCII
Write-Host ""
Write-Host "Built: $game" -ForegroundColor Green
if (-not $BuildOnly) {
    Write-Host "Starting APB..."
    Start-Process -FilePath $game -ArgumentList "`"$romDir`"" -WorkingDirectory $top
}
exit 0
