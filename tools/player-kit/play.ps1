# Play APB (the Windows download's "Play APB.bat" runs this).
#
# The download holds everything but the game's own code: that is made here,
# from your ROMs, the first time (and again after an update): qilin lifts
# both CPUs' programs into C++, the bundled compiler (LLVM-MinGW) compiles
# them, and they're linked with the prebuilt rest into APB.exe. Then, and
# every time after, it starts APB.exe.
#
#   play.ps1 [-Rebuild] [-BuildOnly] [-HardwareCheck]
#
# -Rebuild compiles everything again, even if APB.exe is up to date;
# -BuildOnly doesn't start the game afterwards.
param(
    [switch]$Rebuild,
    [switch]$BuildOnly,
    [switch]$HardwareCheck
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
$kitVersion = (Get-Content -LiteralPath (Join-Path $kit "VERSION") -Raw).Trim()
$onWindows = ($PSVersionTable.PSEdition -eq "Desktop") -or $IsWindows   # Windows PowerShell 5.1 has no $IsWindows
# SHA-256 of a file, upper-case hex. Not Get-FileHash: that cmdlet is missing
# when Windows PowerShell inherits a PowerShell 7 module path, as it does
# when started from pwsh (or from CI's shell).
function Get-Sha256([string]$path) {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    $stream = [System.IO.File]::OpenRead($ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($path))  # .NET resolves against the process folder, not PowerShell location
    try { return ([BitConverter]::ToString($sha.ComputeHash($stream))).Replace('-', '') }
    finally { $stream.Dispose(); $sha.Dispose() }
}
# A version alone cannot detect a repaired/replaced kit with the same release.
$identityParts = @("lib/libapb_kit.a", "qilin.exe", "kit.ninja", "play.ps1") | ForEach-Object {
    (Get-Sha256 (Join-Path $kit $_))
}
$builtFor = "$kitVersion | $($identityParts -join ':')"
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

# Start-Process reads its paths as wildcard patterns ([ and ] in the folder's name): escaped, they're taken as they are.
function Get-Literal([string]$path) { return [System.Management.Automation.WildcardPattern]::Escape($path) }

# The same launch path after a build and on subsequent starts.
function Start-APB([string]$roms = "") {
    $gameArguments = @()
    # A trailing backslash would escape the closing quote ("E:\" reads as E:"): double it.
    if ($roms) { $gameArguments += "`"$roms$(if ($roms.EndsWith('\')) { '\' })`"" }
    if (-not $HardwareCheck) {
        if ($gameArguments.Count) { Start-Process -FilePath (Get-Literal $game) -ArgumentList $gameArguments -WorkingDirectory (Get-Literal $top) }
        else { Start-Process -FilePath (Get-Literal $game) -WorkingDirectory (Get-Literal $top) }
        return
    }
    $session = (Get-Date -Format "yyyyMMdd-HHmmss") + "-" + [guid]::NewGuid().ToString("N").Substring(0, 8)
    $reports = Join-Path (Join-Path $top "Hardware checks") $session
    New-Item -ItemType Directory -Path $reports | Out-Null
    $instructions = @"
APB hardware check - kit $kitVersion

1. Test one wheel setup per run. Open F1 / Controls and select your saved setup.
2. R5: turn several full turns each way. Check the physical-angle reading keeps
   tracking beyond the ordinary joystick endpoints. Note Pit House and firmware.
   G920: match the maximum angle to G HUB (usually 900 degrees), turn both ways,
   and check held-at-lock steering if enabled. Note G HUB and firmware.
3. Fully press/release the pedal. Test siren/start, fire and coin bindings.
4. Open F1, turn the wheel, then resume. Confirm no unwanted steering jump.
   Repeat with Alt-Tab. Check reconnect behaviour while the wheel is stationary.
5. Play for at least two minutes using your normal display settings. Note any
   stutters, sound gaps or unexpected force feel, and approximately when they occur.
6. Quit APB normally. Reports and the input recording are saved in this folder.

Fill these in, then return the folder's files for analysis:
Wheel model:
Driver / Pit House / G HUB version:
Firmware:
Display refresh / connection:
Continuous R5 angle or G920 endpoints correct:
Pedal / buttons correct:
Pause / Alt-Tab / reconnect correct:
Force feel:
Stutters or sound gaps and approximate time:

controllers.txt includes device serials and paths. Review before sharing publicly.
inputs.qir includes starting high scores/settings and board inputs, but no ROMs.
Recording I/O is timed separately in performance.txt. This is an actual hardware
run, not an automated certification of wheel support or input-to-photon latency.
"@
    Set-Content -LiteralPath (Join-Path $reports "checklist.txt") -Value $instructions -Encoding UTF8
    foreach ($option in @(@("--record", "inputs.qir"), @("--profile", "performance.txt"), @("--controller-report", "controllers.txt"))) {
        $gameArguments += $option[0]
        $gameArguments += "`"$(Join-Path $reports $option[1])`""
    }
    Write-Host "Hardware check: $reports" -ForegroundColor Cyan
    Write-Host $instructions
    # .NET, not Start-Process: its redirections can't take a folder whose name has [ or ].
    $info = New-Object System.Diagnostics.ProcessStartInfo
    $info.FileName = $game
    $info.Arguments = $gameArguments -join " "
    $info.WorkingDirectory = $top
    $info.UseShellExecute = $false
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $process = [System.Diagnostics.Process]::Start($info)
    $stdout = $process.StandardOutput.ReadToEndAsync()
    $stderr = $process.StandardError.ReadToEndAsync()
    $process.WaitForExit()
    [System.IO.File]::WriteAllText((Join-Path $reports "stdout.txt"), $stdout.Result)
    [System.IO.File]::WriteAllText((Join-Path $reports "stderr.txt"), $stderr.Result)
    Write-Host "Reports saved: $reports" -ForegroundColor Green
    if ($onWindows -and -not $env:CI) { Start-Process -FilePath "explorer.exe" -ArgumentList "`"$reports`"" }
    if ($process.ExitCode -ne 0) { throw "APB exited with code $($process.ExitCode); see stderr.txt in $reports" }
}

# Up to date: just play.
$built = if (Test-Path -LiteralPath $stamp) { (Get-Content -LiteralPath $stamp -Raw).Trim() } else { "" }
if (-not $Rebuild -and (Test-Path -LiteralPath $game) -and $built -eq $builtFor) {
    if (-not $BuildOnly) { Start-APB }
    exit 0
}

# One build at a time: a second Play (or the hardware check) while this one
# builds would lift and compile into the same folder.
New-Item -ItemType Directory -Force -Path $work | Out-Null
try {
    $lock = [System.IO.File]::Open((Join-Path $work "build.lock"), "OpenOrCreate", "ReadWrite", "None")
} catch {
    throw "APB is already being built in another window: wait for it to finish, then start it again."
}

Write-Host "APB QilinRecomp $kitVersion" -ForegroundColor Cyan
if (-not (Test-Path -LiteralPath (Join-Path ([Environment]::SystemDirectory) "vulkan-1.dll")) -and $onWindows) {
    Write-Host "This PC has no Vulkan driver (vulkan-1.dll), which APB needs: install your graphics card's latest driver." -ForegroundColor Yellow
}
if (Test-Path -LiteralPath $game) { Write-Host "Updating the game for this version: a few minutes." }
else { Write-Host "Building the game from your ROMs. This happens once (and after an update): a few minutes." }
Write-Host ""

# The ROM folder: the one APB used last, else ask (until one holds a verified set).
$qilin = Join-Path $kit "qilin.exe"
# The game's data folder: qilin\apb under the local application data. Up to
# v0.21.0 it was Qilin\apb under the roaming one; the game moves that across
# the first time it runs, so until then the settings are still read from there.
$dataBase = if ($onWindows) { $env:LOCALAPPDATA } elseif ($env:XDG_DATA_HOME) { $env:XDG_DATA_HOME } else { Join-Path $HOME ".local/share" }
$oldBase = if ($onWindows) { $env:APPDATA } else { $dataBase }
$settingsFolder = Join-Path (Join-Path $dataBase "qilin") "apb"
$oldSettingsFolder = Join-Path (Join-Path $oldBase "Qilin") "apb"
if (-not (Test-Path -LiteralPath (Join-Path $settingsFolder "apb.ini")) -and (Test-Path -LiteralPath (Join-Path $oldSettingsFolder "apb.ini"))) { $settingsFolder = $oldSettingsFolder }
$romDir = ""
$ini = Join-Path $settingsFolder "apb.ini"
if (Test-Path -LiteralPath $ini) {
    $line = Select-String -LiteralPath $ini -Pattern '^rom_dir = (.+)$' | Select-Object -First 1
    if ($line) { $romDir = $line.Matches[0].Groups[1].Value.Trim() }
}
while ($true) {
    if ($romDir) {
        $report = Read-Kit $qilin @("verify", $romDir)
        if ($report.code -eq 0) { break }
        Write-Host "The ROMs in $romDir aren't a complete rev 7 set:" -ForegroundColor Yellow
        $reportLines = @($report.text.TrimEnd() -split "`r?`n")
        if ($reportLines.Count -gt 7) { $reportLines = @($reportLines[0..5]) + @("...", $reportLines[-1]) }
        Write-Host ($reportLines -join "`n")
        if (@(Get-ChildItem -LiteralPath $romDir -Filter *.zip -ErrorAction SilentlyContinue).Count) {
            Write-Host "That folder holds a .zip: unzip it (MAME's apb.zip) into a folder of its own and choose that folder." -ForegroundColor Yellow
        }
    }
    if (-not $onWindows) { throw "no ROM folder: set rom_dir in $ini" }
    Add-Type -AssemblyName System.Windows.Forms
    $dialog = New-Object System.Windows.Forms.FolderBrowserDialog
    $dialog.Description = "Choose the folder that holds your APB ROMs (the rev 7 set: 36 files, as MAME's apb)"
    $dialog.ShowNewFolderButton = $false
    $owner = New-Object System.Windows.Forms.Form -Property @{ TopMost = $true }   # else the dialog can open behind the console
    $answer = $dialog.ShowDialog($owner)
    $owner.Dispose()
    if ($answer -ne [System.Windows.Forms.DialogResult]::OK) {
        Write-Host "No ROM folder chosen."
        exit 0
    }
    $romDir = $dialog.SelectedPath
}
Write-Host "ROMs: $romDir"

# Lift both CPUs' programs (unchanged files are left alone).
Write-Host "== Recompiling the main CPU's program" -ForegroundColor Cyan
if ((Invoke-Kit $qilin @("lift", $romDir, "--out", (Join-Path $work "gen"))) -ne 0) { throw "qilin lift failed" }
Write-Host "== Recompiling the sound CPU's program" -ForegroundColor Cyan
if ((Invoke-Kit $qilin @("lift", $romDir, "--cpu", "sound", "--out", (Join-Path $work "gen-sound"))) -ne 0) { throw "qilin lift --cpu sound failed" }

# Compile and link: a ninja file over what the lift wrote, with the kit's
# flags and libraries (kit.ninja). Paths are relative to the work folder, so
# spaces in where the download was unpacked don't matter.
$sources = @(Get-ChildItem -LiteralPath (Join-Path $work "gen") -Filter *.cpp) + @(Get-ChildItem -LiteralPath (Join-Path $work "gen-sound") -Filter *.cpp)
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
Set-Content -LiteralPath (Join-Path $work "build.ninja") -Value $lines -Encoding ASCII

# -Rebuild: compile everything again (ninja tracks the command lines, not the
# compiler, nor an object damaged after it was written).
if ($Rebuild -and (Test-Path -LiteralPath (Join-Path $work ".ninja_log"))) {
    if ((Invoke-Kit (Join-Path $kit "ninja.exe") @("-C", $work, "-t", "clean")) -ne 0) { throw "cleaning the build failed" }
}
Write-Host "== Compiling ($($sources.Count) files)" -ForegroundColor Cyan
if ((Invoke-Kit (Join-Path $kit "ninja.exe") @("-C", $work)) -ne 0) { throw "compiling failed" }
# Stage the whole executable before replacing the working one. If APB is
# still running or the copy fails, its previous executable and stamp survive.
$staged = "$game.new"
Copy-Item -LiteralPath (Join-Path $work "APB.exe") -Destination $staged -Force
# An antivirus scan of the new file, or APB still running, holds it for a while: try again for a few seconds.
for ($attempt = 1; ; $attempt++) {
    try {
        if (Test-Path -LiteralPath $game) { [System.IO.File]::Replace($staged, $game, "$game.previous", $true) }
        else { [System.IO.File]::Move($staged, $game) }
        break
    } catch {
        if ($attempt -ge 10) { throw "APB.exe couldn't be replaced: close APB if it is running (or allow APB.exe in your antivirus), then start Play again." }
        Start-Sleep -Milliseconds 500
    }
}
Set-Content -LiteralPath $stamp -Value $builtFor -Encoding ASCII
Write-Host ""
Write-Host "Built: $game" -ForegroundColor Green
if (-not $BuildOnly) {
    Write-Host "Starting APB..."
    Start-APB $romDir
}
exit 0
