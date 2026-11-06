$ErrorActionPreference = 'Stop'
$source = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$out = Join-Path $PSScriptRoot 'out'
New-Item -ItemType Directory -Force -Path $out | Out-Null
$log = Join-Path $out ("build-x4pro-{0}.log" -f (Get-Date -Format 'yyyyMMdd-HHmmss'))
$buildExitCode = 0
Start-Transcript -Path $log -Force | Out-Null
try {
  & 'C:\XP-FW\build-fw.ps1' -Device 'X4Pro' -SourcePath $source
  if ($LASTEXITCODE -ne 0) { $buildExitCode = $LASTEXITCODE }
} catch {
  $buildExitCode = 1
  Write-Host ($_ | Out-String) -ForegroundColor Red
} finally {
  Stop-Transcript | Out-Null
  Copy-Item -LiteralPath $log -Destination (Join-Path $out 'build-x4pro-latest.log') -Force
}
Write-Host "Build log: $log"
exit $buildExitCode
