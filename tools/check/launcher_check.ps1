param([string]$Stub, [string]$Launcher, [string]$Out)
$ErrorActionPreference = 'Stop'
$env:CI = 'true' # no Explorer window on the test runner
$fixture = Join-Path $Out ('Kit with spaces ' + [char]0x00e9)
if (Test-Path -LiteralPath $fixture) { Remove-Item -LiteralPath $fixture -Recurse -Force }
New-Item -ItemType Directory -Path (Join-Path $fixture 'kit'), (Join-Path $fixture 'work') -Force | Out-Null
Copy-Item -LiteralPath $Stub -Destination (Join-Path $fixture 'APB.exe')
Copy-Item -LiteralPath $Launcher -Destination (Join-Path $fixture 'kit/play.ps1')
Set-Content -LiteralPath (Join-Path $fixture 'kit/VERSION') -Value 'launcher-test'
New-Item -ItemType Directory -Path (Join-Path $fixture 'kit/lib') -Force | Out-Null
foreach ($part in @('lib/libapb_kit.a', 'kit.ninja')) {
    Set-Content -LiteralPath (Join-Path $fixture "kit/$part") -Value 'fixture'
}
Copy-Item -LiteralPath $Stub -Destination (Join-Path $fixture 'kit/qilin.exe')
Copy-Item -LiteralPath $Stub -Destination (Join-Path $fixture 'kit/ninja.exe')
$hashes = @('lib/libapb_kit.a', 'qilin.exe', 'kit.ninja', 'play.ps1') | ForEach-Object {
    (Get-FileHash -LiteralPath (Join-Path $fixture "kit/$_") -Algorithm SHA256).Hash
}
Set-Content -LiteralPath (Join-Path $fixture 'work/built-with.txt') -Value "launcher-test | $($hashes -join ':')"
$shellExe = (Get-Process -Id $PID).Path
for ($run = 1; $run -le 2; ++$run) {
    & $shellExe -NoProfile -File (Join-Path $fixture 'kit/play.ps1') -HardwareCheck
    if ($LASTEXITCODE -ne 0) { throw "Launcher failed: $LASTEXITCODE" }
}
$reports = @(Get-ChildItem -LiteralPath (Join-Path $fixture 'Hardware checks') -Directory)
if ($reports.Count -ne 2) { throw 'Expected two independent report folders' }
foreach ($report in $reports) {
    foreach ($name in @('inputs.qir', 'performance.txt', 'controllers.txt', 'checklist.txt', 'stdout.txt', 'stderr.txt')) {
        if (-not (Test-Path -LiteralPath (Join-Path $report.FullName $name))) { throw "Missing $name" }
    }
    if ((Get-Content -LiteralPath (Join-Path $report.FullName 'controllers.txt') -Raw).Trim() -ne 'launcher stub') { throw 'Wrong controller output' }
}
Write-Host 'Windows hardware-check launcher: real process, spaces/Unicode and separate sessions passed'

# A changed kit at the same version must rebuild; never read real user settings.
$env:LOCALAPPDATA = Join-Path $fixture 'local-data'
$env:APPDATA = Join-Path $fixture 'roaming-data'
$data = Join-Path $env:LOCALAPPDATA 'qilin/apb'
New-Item -ItemType Directory -Path $data -Force | Out-Null
Set-Content -LiteralPath (Join-Path $data 'apb.ini') -Value "rom_dir = $fixture"
$stamp = Join-Path $fixture 'work/built-with.txt'
$before = Get-Content -LiteralPath $stamp -Raw
Add-Content -LiteralPath (Join-Path $fixture 'kit/lib/libapb_kit.a') -Value 'updated kit'
& $shellExe -NoProfile -File (Join-Path $fixture 'kit/play.ps1') -BuildOnly
if ($LASTEXITCODE -ne 0) { throw 'Same-version kit rebuild failed' }
if ((Get-Content -LiteralPath $stamp -Raw) -eq $before) { throw 'Kit content change was ignored' }
if (-not (Test-Path -LiteralPath (Join-Path $fixture 'APB.exe.previous'))) { throw 'Missing previous executable' }

# An in-use executable cannot be replaced; its bytes and stamp survive.
$before = Get-Content -LiteralPath $stamp -Raw
$game = Join-Path $fixture 'APB.exe'
$hash = (Get-FileHash -LiteralPath $game).Hash
Add-Content -LiteralPath (Join-Path $fixture 'kit/lib/libapb_kit.a') -Value 'another update'
$locked = [System.IO.File]::Open($game, 'Open', 'Read', 'Read')
try {
    & $shellExe -NoProfile -File (Join-Path $fixture 'kit/play.ps1') -BuildOnly
    if ($LASTEXITCODE -eq 0) { throw 'Locked executable replacement unexpectedly succeeded' }
} finally { $locked.Dispose() }
if ((Get-FileHash -LiteralPath $game).Hash -ne $hash -or (Get-Content -LiteralPath $stamp -Raw) -ne $before) {
    throw 'Failed installation changed the executable or stamp'
}
Write-Host 'Same-version kit rebuild and failed installation preservation passed'
