#!/bin/bash
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
APP="${APP_REPO:?set APP_REPO to the AbsentMotion source folder}"
export FM_HARNESS="$HERE/harness"
export FM_AMEDIA="$HERE/../../build/ffamp"
cd "$APP/pc"
./gradlew --offline -q test -x nativeBuild --init-script "$HERE/harness.init.gradle" --tests 'app.absentmotion.export.FmpegCompareHarness'
