param(
  [string]$Compiler = 'g++',
  [string]$ArduinoJson = 'C:\XP-FW\work\src\.pio\libdeps\x4pro-gh_release\ArduinoJson\src'
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$out = Join-Path $PSScriptRoot 'out'
New-Item -ItemType Directory -Force $out | Out-Null
$exe = Join-Path $out 'session-tests.exe'
& $Compiler -std=c++20 -O2 -Wall -Wextra -Werror -DCROSSPOINT_LOCALSEND_HOST_TEST=1 -I $ArduinoJson (Join-Path $PSScriptRoot 'test-session.cpp') (Join-Path $root 'src/network/LocalSendSession.cpp') -o $exe
if ($LASTEXITCODE -ne 0) { throw 'LocalSend host compilation failed' }
& $exe
if ($LASTEXITCODE -ne 0) { throw 'LocalSend session tests failed' }

$receiver = Join-Path $out 'receiver-tests.exe'
& $Compiler -std=c++20 -O2 -Wall -Wextra -Werror -DCROSSPOINT_LOCALSEND=1 -I (Join-Path $PSScriptRoot 'stubs') -I (Join-Path $root 'lib/Memory') -I $ArduinoJson (Join-Path $PSScriptRoot 'test-receiver.cpp') (Join-Path $root 'src/network/LocalSendSession.cpp') (Join-Path $root 'src/network/LocalSendReceiver.cpp') -o $receiver
if ($LASTEXITCODE -ne 0) { throw 'LocalSend receiver host compilation failed' }
& $receiver
if ($LASTEXITCODE -ne 0) { throw 'LocalSend receiver tests failed' }

$history = Join-Path $out 'history-tests.exe'
& $Compiler -std=c++20 -O2 -Wall -Wextra -Werror -DCROSSPOINT_LOCALSEND_HOST_TEST=1 -I (Join-Path $PSScriptRoot 'stubs') -I (Join-Path $root 'src') -I $ArduinoJson (Join-Path $PSScriptRoot 'test-history.cpp') (Join-Path $root 'src/network/LocalSendSession.cpp') -o $history
if ($LASTEXITCODE -ne 0) { throw 'LocalSend history host compilation failed' }
& $history
if ($LASTEXITCODE -ne 0) { throw 'LocalSend history tests failed' }
