echo "== fuzz: inputs that once broke the library replay cleanly, and short blind bursts find nothing (no FFmpeg)"
FZ=$O/fuzz
rm -rf $FZ
mkdir -p $FZ/seeds
find fixtures -type f ! -name '*.sh' ! -name '*.py' ! -name '*.md5' ! -name '*.txt' ! -name '*.json' ! -name '*.means' ! -path 'fixtures/fuzz/*' -exec cp -f {} $FZ/seeds/ \;
for target in open packets probe audio video resample mpeg4; do
    kept=(fixtures/fuzz/$target/*)
    if [ -e "${kept[0]}" ]; then
        check "$target: ${#kept[@]} kept inputs replay without a crash" env TMPDIR=$FZ ../build/fuzz_$target "${kept[@]}"
        if [ -x ../build-fuzz/fuzz_$target ]; then check "$target: the same with undefined-behaviour checks" env TMPDIR=$FZ ../build-fuzz/fuzz_$target "${kept[@]}"; fi
    fi
    seeds=$FZ/seeds
    [ $target = resample ] && { mkdir -p $FZ/resample_seeds; seeds=$FZ/resample_seeds; }
    [ $target = resample ] || check "$target: every committed fixture goes through" env TMPDIR=$FZ ../build/fuzz_$target $FZ/seeds/*
    mkdir -p $FZ/$target
    check "$target: ${FUZZ_BURST:-3} s of mutated inputs, no crash, hang or memory blow-up" env TMPDIR=$FZ ../build/fuzz_$target -seed=7 -max_total_time=${FUZZ_BURST:-3} -corpus_limit_mb=0 -rss_limit_mb=2048 -artifact_prefix=$FZ/ $FZ/$target $seeds
done 2> $FZ/log.txt
rm -rf $FZ/seeds $FZ/absent_fuzz_*
