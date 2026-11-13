#!/bin/sh
set -eu
ARCH="${1:-}"
PREFIX="${2:-}"
case "$ARCH" in arm64|x86_64) ;; *) echo "usage: third_party/build_mac.sh arm64|x86_64 <prefix> [work folder]" >&2; exit 2 ;; esac
[ -n "$PREFIX" ] || { echo "usage: third_party/build_mac.sh arm64|x86_64 <prefix> [work folder]" >&2; exit 2; }
mkdir -p "$PREFIX"
PREFIX="$(CDPATH= cd -- "$PREFIX" && pwd)"
WORK="${3:-$PREFIX/work}"
mkdir -p "$WORK/tarballs" "$WORK/$ARCH"
WORK="$(CDPATH= cd -- "$WORK" && pwd)"
JOBS="${JOBS:-4}"
TARGET=11.0
HOST="$ARCH-apple-darwin"
[ "$ARCH" = arm64 ] && HOST=aarch64-apple-darwin
FLAGS="-arch $ARCH -mmacosx-version-min=$TARGET -O2"
export MACOSX_DEPLOYMENT_TARGET="$TARGET"

fetch() {
    file="$WORK/tarballs/$(basename "$1")"
    [ -f "$file" ] || curl -fsSL -o "$file.part" "$1" && { [ -f "$file" ] || mv "$file.part" "$file"; }
    got="$(shasum -a 256 "$file" | cut -d' ' -f1)"
    [ "$got" = "$2" ] || { echo "checksum of $file is $got, expected $2" >&2; exit 1; }
    rm -rf "$WORK/$ARCH/$3"
    mkdir -p "$WORK/$ARCH/$3"
    tar -xf "$file" -C "$WORK/$ARCH/$3" --strip-components 1
    cd "$WORK/$ARCH/$3"
}

with_cmake() {
    cmake -Wno-dev -Wno-deprecated -S . -B out -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES="$ARCH" -DCMAKE_OSX_DEPLOYMENT_TARGET="$TARGET" \
        -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_PREFIX_PATH="$PREFIX" -DCMAKE_INSTALL_LIBDIR=lib -DBUILD_SHARED_LIBS=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_IGNORE_PREFIX_PATH="/opt/homebrew;/usr/local" -DCMAKE_SYSTEM_NAME=Darwin -DCMAKE_SYSTEM_PROCESSOR="$ARCH" "$@" >/dev/null
    cmake --build out --parallel "$JOBS" >/dev/null
    cmake --install out >/dev/null
}

with_configure() {
    CC="clang" CFLAGS="$FLAGS" LDFLAGS="$FLAGS" PKG_CONFIG=/usr/bin/true ./configure --host="$HOST" --prefix="$PREFIX" --disable-shared --enable-static "$@" >/dev/null
    make -j"$JOBS" >/dev/null
    make install >/dev/null
}

fetch https://ftp.osuosl.org/pub/xiph/releases/opus/opus-1.6.1.tar.gz 6ffcb593207be92584df15b32466ed64bbec99109f007c82205f0194572411a1 opus
with_cmake -DOPUS_BUILD_PROGRAMS=OFF -DOPUS_BUILD_TESTING=OFF

fetch https://ftp.osuosl.org/pub/xiph/releases/ogg/libogg-1.3.6.tar.gz 83e6704730683d004d20e21b8f7f55dcb3383cdf84c0daedf30bde175f774638 ogg
with_cmake -DBUILD_TESTING=OFF -DINSTALL_DOCS=OFF

fetch https://ftp.osuosl.org/pub/xiph/releases/vorbis/libvorbis-1.3.7.tar.xz b33cc4934322bcbf6efcbacf49e3ca01aadbea4114ec9589d1b1e9d20f72954b vorbis
with_cmake

fetch https://downloads.sourceforge.net/project/lame/lame/4.0/lame-4.0.tar.gz 3df5124d5ad3a98312ffd7ba6a9b36230e4f8a3e66d3ce0f425e336c32d216eb lame
with_configure --disable-frontend --disable-decoder

fetch https://www.mpg123.de/download/mpg123-1.33.7.tar.bz2 31d0e35a4ca567ec9b5ebda6c3062bb4435d6d3eacd6ef0d95cadd7854dc03ee mpg123
with_configure --disable-components --enable-libmpg123

fetch https://github.com/AcademySoftwareFoundation/openapv/archive/refs/tags/v1.1.3.0.tar.gz 38f841ce44e80c40b682a5323b4f3ebc6e339b6071965bc1c705a17d46ab2fc8 oapv
with_cmake -DOAPV_BUILD_APPS=OFF -DOAPV_BUILD_SHARED_LIB=OFF -DOAPV_APP_STATIC_BUILD=ON
[ -f "$PREFIX/lib/liboapv.a" ] || cp "$(find "$PREFIX/lib" out -name liboapv.a | head -n 1)" "$PREFIX/lib/liboapv.a"
[ -f "$PREFIX/include/oapv/oapv_exports.h" ] || printf '#pragma once\n#define OAPV_EXPORT\n' > "$PREFIX/include/oapv/oapv_exports.h"

fetch https://github.com/webmproject/libvpx/archive/refs/tags/v1.17.0.tar.gz 1020f184046187baa2985dbde38e0691f49c44088bca7a1842b0236c6081dc0a vpx
./configure --target="$ARCH-darwin20-gcc" --prefix="$PREFIX" --disable-examples --disable-tools --disable-docs --disable-unit-tests --disable-shared --enable-static \
    --enable-pic --enable-vp9-highbitdepth --extra-cflags="-mmacosx-version-min=$TARGET" --extra-cxxflags="-mmacosx-version-min=$TARGET" >/dev/null
make -j"$JOBS" >/dev/null
make install >/dev/null

fetch https://code.videolan.org/videolan/dav1d/-/archive/1.5.4/dav1d-1.5.4.tar.bz2 2abfb0c89212e6e4733a54e0ae509ec00a5b845a6360946f918806e14aedb011 dav1d
FAMILY=aarch64
[ "$ARCH" = x86_64 ] && FAMILY=x86_64
cat > cross.txt <<CROSS
[binaries]
c = ['clang', '-arch', '$ARCH', '-mmacosx-version-min=$TARGET']
ar = 'ar'
strip = 'strip'
[host_machine]
system = 'darwin'
cpu_family = '$FAMILY'
cpu = '$FAMILY'
endian = 'little'
CROSS
meson setup out --cross-file cross.txt --buildtype release --default-library static --prefix "$PREFIX" --libdir lib -Denable_tools=false -Denable_tests=false >/dev/null
ninja -C out -j "$JOBS" >/dev/null
ninja -C out install >/dev/null

fetch https://gitlab.com/AOMediaCodec/SVT-AV1/-/archive/v4.3.0/SVT-AV1-v4.3.0.tar.bz2 9223ce06e6172ad41357ed23b3b00f036e03794b86b0e20ea9ccb3f648b8955d svtav1
with_cmake -DBUILD_APPS=OFF -DBUILD_TESTING=OFF -DSVT_AV1_LTO=OFF

for lib in opus ogg vorbis mp3lame mpg123 oapv vpx dav1d SvtAv1Enc; do
    file="$PREFIX/lib/lib$lib.a"
    [ -f "$file" ] || { echo "missing $file" >&2; exit 1; }
    lipo -archs "$file" | grep -qx "$ARCH" || { echo "$file is not $ARCH only" >&2; exit 1; }
    printf '%s %s bytes\n' "lib$lib.a" "$(stat -f %z "$file")"
done
find "$PREFIX/lib" -name '*.dylib' -delete
