#!/bin/sh
set -eu
PREFIX="${1:-}"
[ -n "$PREFIX" ] || { echo "usage: third_party/build_linux.sh <prefix> [work folder]  (x86_64, needs gcc, cmake, ninja, meson, nasm, autoconf, automake, libtool)" >&2; exit 2; }
mkdir -p "$PREFIX"
PREFIX="$(CDPATH= cd -- "$PREFIX" && pwd)"
WORK="${2:-$PREFIX/work}"
mkdir -p "$WORK/tarballs" "$WORK/src"
WORK="$(CDPATH= cd -- "$WORK" && pwd)"
JOBS="${JOBS:-8}"
export CFLAGS="-O2 -fPIC" CXXFLAGS="-O2 -fPIC"
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig"

fetch() {
    file="$WORK/tarballs/$4"
    [ -f "$file" ] || curl -fsSL -o "$file.part" "$1" && { [ -f "$file" ] || mv "$file.part" "$file"; }
    got="$(sha256sum "$file" | cut -d' ' -f1)"
    [ "$got" = "$2" ] || { echo "checksum of $file is $got, expected $2" >&2; exit 1; }
    rm -rf "$WORK/src/$3"
    mkdir -p "$WORK/src/$3"
    tar -xf "$file" -C "$WORK/src/$3" --strip-components 1
    cd "$WORK/src/$3"
}

with_cmake() {
    cmake -Wno-dev -Wno-deprecated -S "${SOURCE:-.}" -B out -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_PREFIX_PATH="$PREFIX" \
        -DCMAKE_INSTALL_LIBDIR=lib -DBUILD_SHARED_LIBS=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DCMAKE_POLICY_VERSION_MINIMUM=3.5 "$@" >/dev/null
    cmake --build out --parallel "$JOBS" >/dev/null
    cmake --install out >/dev/null
}

with_configure() {
    ./configure --prefix="$PREFIX" --libdir="$PREFIX/lib" --disable-shared --enable-static --with-pic "$@" >/dev/null
    make -j"$JOBS" >/dev/null
    make install >/dev/null
}

fetch https://ftp.osuosl.org/pub/xiph/releases/opus/opus-1.6.1.tar.gz 6ffcb593207be92584df15b32466ed64bbec99109f007c82205f0194572411a1 opus opus-1.6.1.tar.gz
with_cmake -DOPUS_BUILD_PROGRAMS=OFF -DOPUS_BUILD_TESTING=OFF

fetch https://ftp.osuosl.org/pub/xiph/releases/ogg/libogg-1.3.6.tar.gz 83e6704730683d004d20e21b8f7f55dcb3383cdf84c0daedf30bde175f774638 ogg libogg-1.3.6.tar.gz
with_cmake -DBUILD_TESTING=OFF -DINSTALL_DOCS=OFF

fetch https://ftp.osuosl.org/pub/xiph/releases/vorbis/libvorbis-1.3.7.tar.xz b33cc4934322bcbf6efcbacf49e3ca01aadbea4114ec9589d1b1e9d20f72954b vorbis libvorbis-1.3.7.tar.xz
with_cmake

fetch https://downloads.sourceforge.net/project/lame/lame/4.0/lame-4.0.tar.gz 3df5124d5ad3a98312ffd7ba6a9b36230e4f8a3e66d3ce0f425e336c32d216eb lame lame-4.0.tar.gz
with_configure --disable-frontend --disable-decoder

fetch https://www.mpg123.de/download/mpg123-1.33.7.tar.bz2 31d0e35a4ca567ec9b5ebda6c3062bb4435d6d3eacd6ef0d95cadd7854dc03ee mpg123 mpg123-1.33.7.tar.bz2
with_configure --disable-components --enable-libmpg123

fetch https://github.com/AcademySoftwareFoundation/openapv/archive/refs/tags/v1.1.3.0.tar.gz 38f841ce44e80c40b682a5323b4f3ebc6e339b6071965bc1c705a17d46ab2fc8 oapv v1.1.3.0.tar.gz
with_cmake -DOAPV_BUILD_APPS=OFF -DOAPV_BUILD_SHARED_LIB=OFF -DOAPV_APP_STATIC_BUILD=ON
[ -f "$PREFIX/lib/liboapv.a" ] || cp "$(find "$PREFIX/lib" out -name liboapv.a | head -n 1)" "$PREFIX/lib/liboapv.a"
[ -f "$PREFIX/include/oapv/oapv_exports.h" ] || printf '#pragma once\n#define OAPV_EXPORT\n' > "$PREFIX/include/oapv/oapv_exports.h"

fetch https://github.com/webmproject/libvpx/archive/refs/tags/v1.17.0.tar.gz 1020f184046187baa2985dbde38e0691f49c44088bca7a1842b0236c6081dc0a vpx v1.17.0.tar.gz
./configure --target=x86_64-linux-gcc --prefix="$PREFIX" --libdir="$PREFIX/lib" --disable-examples --disable-tools --disable-docs --disable-unit-tests --disable-shared --enable-static \
    --enable-pic --enable-vp9-highbitdepth --as=nasm >/dev/null
make -j"$JOBS" >/dev/null
make install >/dev/null

fetch https://code.videolan.org/videolan/dav1d/-/archive/1.5.4/dav1d-1.5.4.tar.bz2 2abfb0c89212e6e4733a54e0ae509ec00a5b845a6360946f918806e14aedb011 dav1d dav1d-1.5.4.tar.bz2
meson setup out --buildtype release --default-library static --prefix "$PREFIX" --libdir lib -Db_staticpic=true -Denable_tools=false -Denable_tests=false >/dev/null
ninja -C out -j "$JOBS" >/dev/null
ninja -C out install >/dev/null

fetch https://gitlab.com/AOMediaCodec/SVT-AV1/-/archive/v4.3.0/SVT-AV1-v4.3.0.tar.bz2 9223ce06e6172ad41357ed23b3b00f036e03794b86b0e20ea9ccb3f648b8955d svtav1 SVT-AV1-v4.3.0.tar.bz2
with_cmake -DBUILD_APPS=OFF -DBUILD_TESTING=OFF -DSVT_AV1_LTO=OFF

fetch https://downloads.xvid.com/downloads/xvidcore-1.3.7.tar.bz2 aeeaae952d4db395249839a3bd03841d6844843f5a4f84c271ff88f7aa1acff7 xvid xvidcore-1.3.7.tar.bz2
cd build/generic
./configure --prefix="$PREFIX" --libdir="$PREFIX/lib" >/dev/null
make -j"$JOBS" >/dev/null
make install >/dev/null

fetch https://github.com/cisco/openh264/archive/refs/tags/v2.7.0.tar.gz 5178775c8b440073e70920e97d195efff721c0e56d0a78cc82c919e95d54924d openh264 openh264-v2.7.0.tar.gz
make -j"$JOBS" OS=linux ARCH=x86_64 USE_ASM=Yes BUILDTYPE=Release PREFIX="$PREFIX" LIBDIR_NAME=lib libopenh264.a >/dev/null
make OS=linux ARCH=x86_64 USE_ASM=Yes BUILDTYPE=Release PREFIX="$PREFIX" LIBDIR_NAME=lib install-static >/dev/null

fetch https://github.com/strukturag/libde265/archive/refs/tags/v1.1.3.tar.gz 189baa08fd6d2dd34db099de411bd6b2e6bd5eb88e81236a3d0fa3826b9715c4 de265 libde265-v1.1.3.tar.gz
with_cmake -DENABLE_SDL=OFF -DENABLE_DECODER=OFF -DENABLE_ENCODER=OFF

fetch https://github.com/knik0/faad2/archive/refs/tags/2.11.4.tar.gz ee479ccbae4a8387ab696e6f21a481bd83fe3881471cafa81b4ae59d7d3aed43 faad2 faad2-2.11.4.tar.gz
with_cmake -DFAAD_BUILD_CLI=OFF

fetch https://downloads.sourceforge.net/project/opencore-amr/vo-aacenc/vo-aacenc-0.1.3.tar.gz e51a7477a359f18df7c4f82d195dab4e14e7414cbd48cf79cc195fc446850f36 voaac vo-aacenc-0.1.3.tar.gz
with_configure

fetch https://code.videolan.org/videolan/x264/-/archive/b35605ace3ddf7c1a5d67a2eb553f034aef41d55/x264-b35605ace3ddf7c1a5d67a2eb553f034aef41d55.tar.bz2 6eeb82934e69fd51e043bd8c5b0d152839638d1ce7aa4eea65a3fedcf83ff224 x264 x264-b35605ace3ddf7c1a5d67a2eb553f034aef41d55.tar.bz2
./configure --prefix="$PREFIX" --libdir="$PREFIX/lib" --enable-static --enable-pic --disable-cli --disable-opencl --bit-depth=8 --chroma-format=420 \
    --disable-lavf --disable-swscale --disable-ffms --disable-gpac --disable-lsmash >/dev/null
make -j"$JOBS" >/dev/null
make install >/dev/null

fetch https://download.videolan.org/pub/videolan/x265/x265_4.2.tar.gz 40b1ea0453e0309f0eba934e0ddf533f8f6295966679e8894e8f1c1c8d5e1210 x265 x265_4.2.tar.gz
SOURCE=source with_cmake -DENABLE_SHARED=OFF -DENABLE_CLI=OFF -DENABLE_PIC=ON -DHIGH_BIT_DEPTH=OFF -DENABLE_LIBNUMA=OFF -DENABLE_HDR10_PLUS=OFF

for lib in opus ogg vorbis mp3lame mpg123 oapv vpx dav1d SvtAv1Enc xvidcore openh264 de265 faad vo-aacenc x264 x265; do
    file="$PREFIX/lib/lib$lib.a"
    [ -f "$file" ] || { echo "missing $file" >&2; exit 1; }
    printf '%s %s bytes\n' "lib$lib.a" "$(stat -c %s "$file")"
done
find "$PREFIX/lib" -name '*.so*' -delete
