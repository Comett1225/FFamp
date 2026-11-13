#!/bin/bash
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
APP="${APP_REPO:?set APP_REPO to the AbsentMotion source folder}"
WORK="${1:?usage: run_zero.sh <work folder> (FM_MODE, FM_PROJECT, FM_RUNS, FM_TAG, FM_NATIVE as for run_harness.sh; FM_HIDE = more folders to block, separated by colons)}"
SHIM="$HERE/../../build/shim"
[ -x "$SHIM/ffmpeg" ] && [ -x "$SHIM/ffprobe" ] || { echo "build the stand-ins first: cmake -S . -B build -DABSENT_SHIM=ON && cmake --build build"; exit 1; }
mkdir -p "$WORK"
WORK="$(cd "$WORK" && pwd -P)"

hidden=()
inert=()
for tool in ffmpeg ffprobe; do
    for found in $(which -a $tool 2> /dev/null || true); do
        real=$(python3 -c 'import os,sys; print(os.path.realpath(sys.argv[1]))' "$found")
        hidden+=("$(dirname "$(dirname "$real")")")
    done
done
[ -d "$APP/pc/build/native" ] && inert+=("$(cd "$APP/pc/build/native" && pwd -P)")
for folder in "$APP"/pc/build/native-*; do
    [ -d "$folder" ] && ls "$folder" | grep -q -E '^(ffmpeg|ffprobe|libav.*|libamdesktop\.dylib)$' && hidden+=("$(cd "$folder" && pwd -P)")
done
IFS=: read -r -a more <<< "${FM_HIDE:-}"
for folder in "${more[@]:-}"; do [ -n "$folder" ] && [ -e "$folder" ] && hidden+=("$(cd "$folder" && pwd -P)"); done

PROFILE="$WORK/zero.sb"
{
    echo "(version 1)"
    echo "(allow default)"
    printf '%s\n' "${hidden[@]}" | sort -u | while read -r folder; do echo "(deny file-read* process-exec* file-map-executable (subpath \"$folder\"))"; done
    for folder in "${inert[@]:-}"; do [ -n "$folder" ] && echo "(deny process-exec* file-map-executable (subpath \"$folder\"))"; done
} > "$PROFILE"

export FM_FFMPEG="$SHIM/ffmpeg" FM_FFPROBE="$SHIM/ffprobe" FM_WORK="$WORK"
export ABSENT_SHIM_LOG="$WORK/stand-in-calls-${FM_TAG:-run}.log"
export GRADLE_OPTS="-Dorg.gradle.daemon=false"
export PATH="${JAVA_HOME:+$JAVA_HOME/bin:}/usr/bin:/bin:/usr/sbin:/sbin"
: > "$ABSENT_SHIM_LOG"
EVIDENCE="$WORK/blocked-${FM_TAG:-run}.txt"
{
    echo "PATH inside the run: $PATH"
    echo "ffmpeg on that PATH: $(command -v ffmpeg || echo none)"
    echo "ffprobe on that PATH: $(command -v ffprobe || echo none)"
    echo "folders blocked for reading, running and loading, for the whole run and every program it starts:"
    grep 'deny file-read' "$PROFILE" | sed 's/.*subpath "\(.*\)").*/  \1/'
    echo "folders blocked for running and loading only (the build tool checks that the app's usual engine library exists; it can be looked at, not used):"
    grep 'deny process-exec' "$PROFILE" | sed 's/.*subpath "\(.*\)").*/  \1/'
    echo "trying to run or load each real FFmpeg program and library from inside the blocked run:"
    printf '%s\n' "${hidden[@]}" "${inert[@]:-}" | sort -u | while read -r folder; do
        [ -n "$folder" ] || continue
        find "$folder" -maxdepth 2 -type f \( -name ffmpeg -o -name ffprobe -o -name 'libav*.dylib' -o -name 'libsw*.dylib' -o -name 'libamdesktop.dylib' \) 2> /dev/null | while read -r file; do
            case "$file" in
                *.dylib) attempt='import ctypes,sys; ctypes.CDLL(sys.argv[1])'; sandbox-exec -f "$PROFILE" /usr/bin/python3 -c "$attempt" "$file" > /dev/null 2>&1 && state=USABLE || state=blocked ;;
                *) sandbox-exec -f "$PROFILE" "$file" -version > /dev/null 2>&1 && state=USABLE || state=blocked ;;
            esac
            echo "  $state: $file"
        done
    done
} > "$EVIDENCE"
cat "$EVIDENCE"
if grep -q USABLE "$EVIDENCE"; then echo "a real FFmpeg file is still reachable, stopping"; exit 1; fi
sandbox-exec -f "$PROFILE" "$HERE/run_harness.sh"
echo "stand-in calls: $ABSENT_SHIM_LOG"
