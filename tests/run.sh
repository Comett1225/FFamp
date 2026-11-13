#!/bin/bash
set -u
cd "$(dirname "$0")"
source lib.sh
for suite in suites/*.sh; do
    case "${ONLY:-}" in "") ;; *) [[ "$suite" == *"$ONLY"* ]] || continue ;; esac
    source "$suite"
done
rm -f $O/raw_1080.yuv $O/bench_*.mp4
[ $failed = 0 ] && echo "ALL PASS" || echo "SOME FAILED"
exit $failed
