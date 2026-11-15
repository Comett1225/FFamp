#!/bin/sh
set -eu
PREFIX="${1:-}"
[ -n "$PREFIX" ] || { echo "usage: third_party/build_android.sh <prefix> [work folder]  (needs ANDROID_HOME and NDK 30)" >&2; exit 2; }
mkdir -p "$PREFIX"
PREFIX="$(CDPATH= cd -- "$PREFIX" && pwd)"
WORK="${2:-$PREFIX/work}"
mkdir -p "$WORK/tarballs" "$WORK/arm64-v8a"
WORK="$(CDPATH= cd -- "$WORK" && pwd)"
JOBS="${JOBS:-4}"
NDK="${ANDROID_NDK:-$ANDROID_HOME/ndk/30.0.16248370}"
TOOLS="$(ls -d "$NDK"/toolchains/llvm/prebuilt/*/bin | head -1)"
API=26

fetch() {
    file="$WORK/tarballs/$(basename "$1")"
    [ -f "$file" ] || curl -fsSL -o "$file.part" "$1" && { [ -f "$file" ] || mv "$file.part" "$file"; }
    got="$(shasum -a 256 "$file" | cut -d' ' -f1)"
    [ "$got" = "$2" ] || { echo "checksum of $file is $got, expected $2" >&2; exit 1; }
    rm -rf "$WORK/arm64-v8a/$3"
    mkdir -p "$WORK/arm64-v8a/$3"
    tar -xf "$file" -C "$WORK/arm64-v8a/$3" --strip-components 1
    cd "$WORK/arm64-v8a/$3"
}

fetch https://downloads.xvid.com/downloads/xvidcore-1.3.7.tar.bz2 aeeaae952d4db395249839a3bd03841d6844843f5a4f84c271ff88f7aa1acff7 xvid
cd build/generic
CC="$TOOLS/aarch64-linux-android$API-clang" AR="$TOOLS/llvm-ar" RANLIB="$TOOLS/llvm-ranlib" CFLAGS="-O2 -fPIC" \
    ./configure --host=aarch64-linux-android --prefix="$PREFIX/arm64-v8a" --disable-assembly >/dev/null
make -j"$JOBS" >/dev/null
make install >/dev/null
rm -f "$PREFIX"/arm64-v8a/lib/libxvidcore.so*
printf 'libxvidcore.a %s bytes\n' "$(wc -c < "$PREFIX/arm64-v8a/lib/libxvidcore.a" | tr -d ' ')"
