param([string]$Stub, [string]$Launcher, [string]$Out)
$ErrorActionPreference = 'Stop'
$env:CI = 'true' # no Explorer window on the test runner
$fixture = Join-Path $Out ('Kit with spaces ' + [char]0x00e9)
if (Test-Path -LiteralPath $fixture) { Remove-Item -LiteralPath $fixture -Recurse -Force }
New-Item -ItemType Directory -Path (Join-Path $fixture 'kit'), (Join-Path $fixture 'work') -Force | Out-Null
Copy-Item -LiteralPath $Stub -Destination (Join-Path $fixture 'APB.exe')
Copy-Item -LiteralPath $Launcher -Destination (Join-Path $fixture 'kit/play.ps1')
Set-Content -LiteralPath (Join-Path $fixture 'kit/VERSION') -Value 'launcher-test'
Set-Content -LiteralPath (Join-Path $fixture 'work/built-with.txt') -Value 'launcher-test'
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
