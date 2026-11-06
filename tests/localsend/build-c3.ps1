param(
  [ValidateSet('X3', 'X4')]
  [string]$Device = 'X3'
)
$ErrorActionPreference = 'Stop'
$source = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$out = Join-Path $PSScriptRoot 'out'
New-Item -ItemType Directory -Force -Path $out | Out-Null
$log = Join-Path $out ("build-{0}-{1}.log" -f $Device.ToLowerInvariant(), (Get-Date -Format 'yyyyMMdd-HHmmss'))
$buildExitCode = 0
Start-Transcript -Path $log -Force | Out-Null
try {
  & 'C:\XP-FW\build-fw.ps1' -Device $Device -SourcePath $source
  if ($LASTEXITCODE -ne 0) { $buildExitCode = $LASTEXITCODE }
} catch {
  $buildExitCode = 1
  Write-Host ($_ | Out-String) -ForegroundColor Red
} finally {
  Stop-Transcript | Out-Null
  Copy-Item -LiteralPath $log -Destination (Join-Path $out ("build-{0}-latest.log" -f $Device.ToLowerInvariant())) -Force
}
Write-Host "Build log: $log"
exit $buildExitCode
