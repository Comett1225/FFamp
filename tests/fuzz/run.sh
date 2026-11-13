#!/bin/bash
set -u
cd "$(dirname "$0")"
seconds=${1:?usage: run.sh <seconds per target> [target...]   (JOBS=n processes per target, default 1; FUZZ_ARGS=more options for every target; GUARD=1 runs under guard malloc, which traps heap overruns)}
shift
targets=${*:-open packets probe audio video resample}
jobs=${JOBS:-1}
build=../../build-fuzz
[ -x $build/fuzz_open ] || ./build.sh || exit 1
export TMPDIR=$(mktemp -d)
export UBSAN_OPTIONS=print_stacktrace=1
guard=()
[ -n "${GUARD:-}" ] && guard=(env DYLD_INSERT_LIBRARIES=/usr/lib/libgmalloc.dylib MALLOC_FILL_SPACE=1)
mkdir -p corpus findings logs "$TMPDIR/seeds"
find ../fixtures -type f ! -name '*.sh' ! -name '*.py' ! -name '*.md5' ! -name '*.txt' ! -name '*.json' ! -name '*.means' ! -path '*/fuzz/resample/*' -exec cp -f {} "$TMPDIR/seeds/" \;
one() {
    local target=$1 job=$2 stop=$(($(date +%s) + seconds)) left
    mkdir -p corpus/$target findings/$target
    local seeds="$TMPDIR/seeds"
    [ $target = resample ] && { mkdir -p ../fixtures/fuzz/resample; seeds=../fixtures/fuzz/resample; }
    mkdir -p "$seeds"
    while left=$((stop - $(date +%s))) && [ $left -gt 0 ]; do
        ${guard[@]+"${guard[@]}"} $build/fuzz_$target ${FUZZ_ARGS:-} -max_total_time=$left -artifact_prefix=findings/$target/ corpus/$target "$seeds" >> logs/$target.$job.log 2>&1
    done
}
for target in $targets; do
    for ((job = 0; job < jobs; job++)); do one $target $job & done
done
wait
rm -rf "$TMPDIR"
status=0
for target in $targets; do
    found=$(ls findings/$target 2> /dev/null | wc -l | tr -d ' ')
    last=$(grep -h 'fuzz: ' logs/$target.*.log | grep -E 'runs' | tail -1)
    echo "$target: $found findings; $last"
    [ "$found" = 0 ] || status=1
done
exit $status
