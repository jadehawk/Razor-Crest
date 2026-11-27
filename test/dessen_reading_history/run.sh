#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
build_dir="${TMPDIR:-/tmp}/crosspoint-dessen-reading-history-tests"
json_dir="${ARDUINOJSON_DIR:-.pio/libdeps/default/ArduinoJson/src}"
mkdir -p "$build_dir"
c++ -std=c++20 -Wall -Wextra -Werror -Wno-unused-parameter -fno-exceptions -DCROSSPOINT_DESSEN_APPS=1 \
  -I"$json_dir" -Itest/clipping_sync/stubs -Itest/library_builder/stubs \
  -Itest/clipping_store/stubs -Isrc -Idessen -Ilib/Utf8 \
  -Ifreeink-sdk/libs/ui/FreeInkUI/include \
  test/dessen_reading_history/DessenReadingHistoryTest.cpp src/activities/plugins/DessenReadingHistory.cpp \
  lib/Utf8/Utf8.cpp -o "$build_dir/test"
"$build_dir/test"
