#!/bin/bash
set -eu
cd "$(dirname "$0")/../.."
engine=OFF
probe=$(mktemp -d)
printf '#include <cstdint>\n#include <cstddef>\nextern "C" int LLVMFuzzerTestOneInput(const uint8_t*, size_t) { return 0; }\n' > "$probe/t.cpp"
if ${CXX:-c++} -fsanitize=fuzzer "$probe/t.cpp" -o "$probe/t" 2> /dev/null; then engine=ON; fi
rm -rf "$probe"
[ $engine = ON ] && echo "fuzz build: the compiler's own fuzzing engine" || echo "fuzz build: own coverage-guided driver (this compiler ships no fuzzing engine)"
cmake -S . -B build-fuzz -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DABSENT_FUZZ=ON -DABSENT_LIBFUZZER=$engine > /dev/null
cmake --build build-fuzz -j4 --target fuzz_open fuzz_packets fuzz_probe fuzz_audio fuzz_video fuzz_resample fuzz_mpeg4 fuzz_vc1 2>&1 | grep -E "error|warning: [a-z]" | grep -v "was built for newer" || true
ls build-fuzz/fuzz_open build-fuzz/fuzz_packets build-fuzz/fuzz_probe build-fuzz/fuzz_audio build-fuzz/fuzz_video build-fuzz/fuzz_resample build-fuzz/fuzz_mpeg4 build-fuzz/fuzz_vc1 > /dev/null
echo "fuzz build: done, targets in build-fuzz/"
