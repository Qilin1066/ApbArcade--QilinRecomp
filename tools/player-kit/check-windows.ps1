# Checks the Windows download as a player gets it (CI, on Windows, under
# Windows PowerShell 5.1):
#   powershell -File tools/player-kit/check-windows.ps1 -Zip <download.zip> -Place <empty folder>
#
# Unpacks it into a folder whose name has spaces, brackets and letters
# outside the ANSI code page, then: the layout and kit\files.txt (both
# ways); its own qilin.exe reads a ROM folder named that way (verify) and
# answers the identify contract for it; and its own compiler builds the game
# as the launcher does (build.cpp: build-flags.txt's flags, $kit as ..\kit
# from work\, -march=native and -Igen), with link_check.cpp standing in for
# the lifted ROMs, and the game starts and stops at its usage.
param([Parameter(Mandatory)][string]$Zip, [Parameter(Mandatory)][string]$Place)
$ErrorActionPreference = "Stop"
$here = $PSScriptRoot
function Fail([string]$why) { throw "check-windows: $why" }

$place = Join-Path $Place ("Kit [check] " + [char]0x00e9 + [char]0x0142)
Add-Type -AssemblyName System.IO.Compression.FileSystem
if (-not [System.IO.File]::Exists($Zip)) { Fail "no $Zip" }
[System.IO.Compression.ZipFile]::ExtractToDirectory($Zip, $place)   # not Expand-Archive: its destination is a wildcard pattern
$top = Join-Path $place "ApbArcade--QilinRecomp"
$kit = Join-Path $top "kit"

# The layout.
foreach ($f in @("Play APB.exe", "README.txt", "Check controls and performance.bat", "kit\qilin.exe", "kit\game.txt",
                 "kit\build-flags.txt", "kit\files.txt", "kit\VERSION", "kit\lib\libapb_kit.a", "kit\lib\icon.o",
                 "kit\include\qilin_board.hpp", "kit\toolchain\bin\x86_64-w64-mingw32-clang++.exe",
                 "LICENSES\ApbArcade--QilinRecomp\LICENSE.txt", "LICENSES\SDL3\LICENSE.txt", "LICENSES\imgui\LICENSE.txt",
                 "LICENSES\volk\LICENSE.md", "LICENSES\ymfm\LICENSE", "LICENSES\mame-tms5220\LICENSE", "LICENSES\third-party.md",
                 "LICENSES\llvm-mingw\LLVM-LICENSE.TXT")) {
    if (-not [System.IO.File]::Exists((Join-Path $top $f))) { Fail "missing: $f" }
}
$game = [System.IO.File]::ReadAllLines((Join-Path $kit "game.txt"))
foreach ($line in @("game=apb", "exe=APB.exe", "input=folder", "compiler=toolchain/bin/x86_64-w64-mingw32-clang++.exe")) {
    if ($game -notcontains $line) { Fail "game.txt has no line $line" }
}
# files.txt names every file, and only those.
$listed = @([System.IO.File]::ReadAllLines((Join-Path $kit "files.txt")) | Where-Object { $_ } | Sort-Object)
$present = @(Get-ChildItem -LiteralPath $top -Recurse -File | ForEach-Object { $_.FullName.Substring($top.Length + 1).Replace('\', '/') } | Sort-Object)
$difference = Compare-Object $listed $present
if ($difference) { $difference | Format-Table | Out-String | Write-Host; Fail "kit\files.txt doesn't match the files" }
Write-Host "layout: ok ($($listed.Count) files)"

# The recompiler: a ROM folder named outside the ANSI code page; qilin must
# find the file in it (its SHA-1 is wrong, so it isn't "missing"), and say so
# as the identify contract's JSON.
$qilin = Join-Path $kit "qilin.exe"
& $qilin --version
if ($LASTEXITCODE -ne 0) { Fail "qilin --version failed" }
$roms = Join-Path $place ("ROMs " + [char]0x0142)
New-Item -ItemType Directory -Path $roms | Out-Null
[System.IO.File]::WriteAllBytes((Join-Path $roms "136051-1101.5t"), (New-Object byte[] 65536))
$report = (& $qilin verify $roms) -join "`n"
if ($LASTEXITCODE -eq 0 -or $report -notmatch "136051-1101\.5t +wrong SHA-1") { Fail "qilin verify misread a Unicode folder:`n$report" }
$answer = (& $qilin identify $roms --json) -join "`n"
$code = $LASTEXITCODE
Write-Host $answer
if ($code -ne 2 -or $answer -notmatch '"ok": false' -or $answer -notmatch "wrong size or contents \(136051-1101\.5t\)") { Fail "qilin identify said $code" }
Write-Host "recompiler: ok"

# The game's build, as the launcher does it, in work\.
$work = Join-Path $top "work"
New-Item -ItemType Directory -Path (Join-Path $work "gen"), (Join-Path $work "obj") | Out-Null
Copy-Item -LiteralPath (Join-Path $here "link_check.cpp") -Destination (Join-Path $work "gen\link_check.cpp")
$flags = @{}
foreach ($line in [System.IO.File]::ReadAllLines((Join-Path $kit "build-flags.txt"))) {
    if ($line -match '^\s*([A-Za-z0-9_.]+)\s*=\s*(.*)$') { $flags[$Matches[1]] = $Matches[2] }
}
function Split-Flags([string]$name) {
    if (-not $flags.ContainsKey($name)) { return @() }
    return @($flags[$name] -split '\s+' | Where-Object { $_ } | ForEach-Object { $_.Replace('$kit', '../kit') })
}
$native = if ($flags.ContainsKey("native")) { Split-Flags "native" } else { @("-march=native") }
$compiler = Join-Path $kit "toolchain\bin\x86_64-w64-mingw32-clang++.exe"
function Invoke-Compiler([string[]]$arguments) {
    $info = New-Object System.Diagnostics.ProcessStartInfo
    $info.FileName = $compiler
    $info.Arguments = ($arguments | ForEach-Object { if ($_ -match '\s') { "`"$_`"" } else { $_ } }) -join " "
    $info.WorkingDirectory = $work
    $info.UseShellExecute = $false
    Write-Host "> clang++ $($info.Arguments)"
    $process = [System.Diagnostics.Process]::Start($info)
    $process.WaitForExit()
    if ($process.ExitCode -ne 0) { Fail "the compiler said $($process.ExitCode)" }
}
Invoke-Compiler (@(Split-Flags "cxxflags") + $native + @("-Igen", "-c", "gen/link_check.cpp", "-o", "obj/link_check.o"))
Invoke-Compiler (@(Split-Flags "ldflags") + @("-o", "obj/APB.exe", "obj/link_check.o") + @(Split-Flags "libs"))
$apb = Get-Item -LiteralPath (Join-Path $work "obj\APB.exe")
if ($apb.Length -lt 1MB) { Fail "APB.exe is too small: $($apb.Length) bytes" }
Write-Host "Built $($apb.FullName), $($apb.Length) bytes"

# Started with an option it doesn't know, it stops at once with its usage
# (exit 2, recomp/runtime/app/apb.cpp), having loaded every library it links.
$info = New-Object System.Diagnostics.ProcessStartInfo
$info.FileName = $apb.FullName
$info.Arguments = "--no-such-option"
$info.WorkingDirectory = $top
$info.UseShellExecute = $false
$info.RedirectStandardError = $true
$info.RedirectStandardOutput = $true
$process = [System.Diagnostics.Process]::Start($info)
$stderr = $process.StandardError.ReadToEndAsync()
$stdout = $process.StandardOutput.ReadToEndAsync()
if (-not $process.WaitForExit(60000)) { $process.Kill(); Fail "the stand-in game didn't stop" }
Write-Host $stdout.Result $stderr.Result
if ($process.ExitCode -ne 2 -or $stderr.Result -notmatch "usage: apb") { Fail "the stand-in game ended with $($process.ExitCode), not its usage" }
Write-Host "check-windows: the download is good"
