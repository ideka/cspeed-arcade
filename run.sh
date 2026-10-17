#!/bin/sh
# Rebuild and run with a time limit: run.sh [seconds] (output in run.log as well)
cd "$(dirname "$0")"
cmd //c "$(cygpath -w "$PWD/build.bat")" 2>&1 | grep -E "error|FAILED" && exit 1
timeout "${1:-20}" ./build/CaliforniaSpeedArcade.exe data 2>&1 | tee run.log | head -${2:-100}
