#!/bin/sh
set -eu
PREFIX="${1:-}"
[ -n "$PREFIX" ] || { echo "usage: third_party/build_windows.sh <prefix> [work folder]  (Git Bash inside a vcvars64 shell, cmake and ninja on PATH; the libraries build with MSVC, FFamp itself with clang-cl)" >&2; exit 2; }
mkdir -p "$PREFIX"
PREFIX="$(cd "$PREFIX" && pwd -W)"
WORK="${2:-$PREFIX/work}"
mkdir -p "$WORK/tarballs" "$WORK/src" "$WORK/bin"
WORK="$(cd "$WORK" && pwd -W)"
JOBS="${JOBS:-8}"

fetch() {
    file="$WORK/tarballs/$4"
    [ -f "$file" ] || curl -fsSL -o "$file.part" "$1" && { [ -f "$file" ] || mv "$file.part" "$file"; }
    got="$(sha256sum "$file" | cut -d' ' -f1)"
    [ "$got" = "$2" ] || { echo "checksum of $file is $got, expected $2" >&2; exit 1; }
    rm -rf "$WORK/src/$3"
    mkdir -p "$WORK/src/$3"
    tar --force-local -xf "$file" -C "$WORK/src/$3" --strip-components 1
    cd "$WORK/src/$3"
}

with_cmake() {
    cmake -Wno-dev -Wno-deprecated -S "${SOURCE:-.}" -B out -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER="${COMPILER:-cl}" -DCMAKE_CXX_COMPILER="${COMPILER:-cl}" \
        -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_PREFIX_PATH="$PREFIX" -DCMAKE_INSTALL_LIBDIR=lib \
        -DBUILD_SHARED_LIBS=OFF -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_POLICY_DEFAULT_CMP0091=NEW "$@" >/dev/null
    cmake --build out --parallel "$JOBS" > out/build.log 2>&1 || { tail -30 out/build.log; exit 1; }
    cmake --install out >/dev/null
}

plain_library() {
    name="$1"
    shift
    cat > CMakeLists.txt <<CMAKE
cmake_minimum_required(VERSION 3.20)
project($name C CXX)
file(GLOB PICKED $*)
add_library($name STATIC \${PICKED})
target_include_directories($name PRIVATE \${INCLUDES})
target_compile_definitions($name PRIVATE \${DEFINES})
target_compile_options($name PRIVATE /W0)
install(TARGETS $name ARCHIVE DESTINATION lib)
CMAKE
}

STARTED="${FROM:+no}"
step() {
    [ -n "${ONLY:-}" ] && { [ "$1" = "$ONLY" ]; return; }
    [ "${STARTED:-yes}" = yes ] && return 0
    [ "$1" = "$FROM" ] && STARTED=yes && return 0
    return 1
}

if step opus; then
fetch https://ftp.osuosl.org/pub/xiph/releases/opus/opus-1.6.1.tar.gz 6ffcb593207be92584df15b32466ed64bbec99109f007c82205f0194572411a1 opus opus-1.6.1.tar.gz
with_cmake -DOPUS_BUILD_PROGRAMS=OFF -DOPUS_BUILD_TESTING=OFF
fi

if step ogg; then
fetch https://ftp.osuosl.org/pub/xiph/releases/ogg/libogg-1.3.6.tar.gz 83e6704730683d004d20e21b8f7f55dcb3383cdf84c0daedf30bde175f774638 ogg libogg-1.3.6.tar.gz
with_cmake -DBUILD_TESTING=OFF -DINSTALL_DOCS=OFF
fi

if step vorbis; then
fetch https://ftp.osuosl.org/pub/xiph/releases/vorbis/libvorbis-1.3.7.tar.xz b33cc4934322bcbf6efcbacf49e3ca01aadbea4114ec9589d1b1e9d20f72954b vorbis libvorbis-1.3.7.tar.xz
with_cmake
fi

if step lame; then
fetch https://downloads.sourceforge.net/project/lame/lame/4.0/lame-4.0.tar.gz 3df5124d5ad3a98312ffd7ba6a9b36230e4f8a3e66d3ce0f425e336c32d216eb lame lame-4.0.tar.gz
cp configMS.h libmp3lame/config.h
echo '#undef HAVE_MPGLIB' >> libmp3lame/config.h
plain_library mp3lame $(ls libmp3lame/*.c libmp3lame/vector/*.c | grep -v mpglib_interface)
with_cmake "-DINCLUDES=include;libmp3lame" "-DDEFINES=HAVE_CONFIG_H;_CRT_SECURE_NO_WARNINGS"
mkdir -p "$PREFIX/include/lame"
cp include/lame.h "$PREFIX/include/lame/"
fi

if step mpg123; then
fetch https://www.mpg123.de/download/mpg123-1.33.7.tar.bz2 31d0e35a4ca567ec9b5ebda6c3062bb4435d6d3eacd6ef0d95cadd7854dc03ee mpg123 mpg123-1.33.7.tar.bz2
SOURCE=ports/cmake with_cmake -DBUILD_PROGRAMS=OFF -DBUILD_LIBOUT123=OFF -DBUILD_LIBSYN123=OFF
fi

if step oapv; then
fetch https://github.com/AcademySoftwareFoundation/openapv/archive/refs/tags/v1.1.3.0.tar.gz 38f841ce44e80c40b682a5323b4f3ebc6e339b6071965bc1c705a17d46ab2fc8 oapv v1.1.3.0.tar.gz
with_cmake -DOAPV_BUILD_APPS=OFF -DOAPV_BUILD_SHARED_LIB=OFF -DOAPV_APP_STATIC_BUILD=ON
[ -f "$PREFIX/include/oapv/oapv_exports.h" ] || printf '#pragma once\n#define OAPV_EXPORT\n' > "$PREFIX/include/oapv/oapv_exports.h"
fi

if step svtav1; then
fetch https://gitlab.com/AOMediaCodec/SVT-AV1/-/archive/v4.3.0/SVT-AV1-v4.3.0.tar.bz2 9223ce06e6172ad41357ed23b3b00f036e03794b86b0e20ea9ccb3f648b8955d svtav1 SVT-AV1-v4.3.0.tar.bz2
with_cmake -DBUILD_APPS=OFF -DBUILD_TESTING=OFF -DSVT_AV1_LTO=OFF -DCOMPILE_C_ONLY=ON
fi

if step xvid; then
fetch https://downloads.xvid.com/downloads/xvidcore-1.3.7.tar.bz2 aeeaae952d4db395249839a3bd03841d6844843f5a4f84c271ff88f7aa1acff7 xvid xvidcore-1.3.7.tar.bz2
sources=""
for f in $(sed -n '/^SRC_GENERIC/,/^$/p' build/generic/sources.inc | grep -o '[a-z_0-9/]*\.c'); do sources="$sources src/$f"; done
plain_library xvidcore $sources
sed -i "s/#if !defined(_MSC_VER) || defined(ARCH_IS_X86_64)/#if 1/" src/motion/gmc.c
with_cmake "-DINCLUDES=src" "-DDEFINES=ARCH_IS_GENERIC;ARCH_IS_64BIT;ARCH_IS_LITTLE_ENDIAN"
cp src/xvid.h "$PREFIX/include/"
fi

if step de265; then
fetch https://github.com/strukturag/libde265/archive/refs/tags/v1.1.3.tar.gz 189baa08fd6d2dd34db099de411bd6b2e6bd5eb88e81236a3d0fa3826b9715c4 de265 libde265-v1.1.3.tar.gz
with_cmake -DENABLE_SDL=OFF -DENABLE_DECODER=OFF -DENABLE_ENCODER=OFF
fi

if step amr; then
fetch https://downloads.sourceforge.net/project/opencore-amr/opencore-amr/opencore-amr-0.1.6.tar.gz 483eb4061088e2b34b358e47540b5d495a96cd468e361050fae615b1809dc4a1 amr opencore-amr-0.1.6.tar.gz
NB=opencore/codecs_v2/audio/gsm_amr/amr_nb
WB=opencore/codecs_v2/audio/gsm_amr/amr_wb
plain_library opencore-amrnb amrnb/wrapper.cpp $(ls $NB/dec/src/*.cpp $NB/common/src/*.cpp | grep -v -E '/(decoder_gsm_amr|pvgsmamrdecoder|bits2prm|copy|div_32|l_abs|r_fft|vad1|vad2)\.cpp$')
with_cmake "-DINCLUDES=oscl;$NB/dec/src;$NB/common/include;$NB/dec/include;opencore/codecs_v2/audio/gsm_amr/common/dec/include;$NB/enc/src" "-DDEFINES=DISABLE_AMRNB_ENCODER"
rm -rf out
plain_library opencore-amrwb amrwb/wrapper.cpp $(ls $WB/dec/src/*.cpp | grep -v '/decoder_amr_wb\.cpp$')
with_cmake "-DINCLUDES=oscl;$WB/dec/src;$WB/dec/include;opencore/codecs_v2/audio/gsm_amr/common/dec/include"
mkdir -p "$PREFIX/include/opencore-amrnb" "$PREFIX/include/opencore-amrwb"
cp amrnb/interf_dec.h "$PREFIX/include/opencore-amrnb/"
cp amrwb/dec_if.h amrwb/if_rom.h "$PREFIX/include/opencore-amrwb/"
fi

if step voaac; then
fetch https://downloads.sourceforge.net/project/opencore-amr/vo-aacenc/vo-aacenc-0.1.3.tar.gz e51a7477a359f18df7c4f82d195dab4e14e7414cbd48cf79cc195fc446850f36 voaac vo-aacenc-0.1.3.tar.gz
plain_library vo-aacenc common/cmnMemory.c aacenc/basic_op/*.c aacenc/src/*.c
with_cmake "-DINCLUDES=common/include;aacenc/inc;aacenc/basic_op"
mkdir -p "$PREFIX/include/vo-aacenc"
cp common/include/*.h "$PREFIX/include/vo-aacenc/"
fi

if step x265; then
fetch https://download.videolan.org/pub/videolan/x265/x265_4.2.tar.gz 40b1ea0453e0309f0eba934e0ddf533f8f6295966679e8894e8f1c1c8d5e1210 x265 x265_4.2.tar.gz
SOURCE=source with_cmake -DENABLE_SHARED=OFF -DENABLE_CLI=OFF -DENABLE_ASSEMBLY=OFF -DHIGH_BIT_DEPTH=OFF -DSTATIC_LINK_CRT=OFF -DENABLE_HDR10_PLUS=OFF
fi

export PATH="$(cygpath -u "$VCToolsInstallDir")bin/Hostx64/x64:$(cygpath -u "$WORK")/bin:$PATH"
if step make; then
fetch https://ftp.gnu.org/gnu/make/make-4.4.1.tar.gz dd16fb1d67bfab79a72f5e8390735c49e3e8e70b4945a15ab1f81ddb78658fb3 make make-4.4.1.tar.gz
cmd //c "build_w32.bat --without-guile" >/dev/null
mkdir -p "$WORK/bin"
cp WinRel/gnumake.exe "$WORK/bin/make.exe"
fi

if step x264; then
fetch https://code.videolan.org/videolan/x264/-/archive/b35605ace3ddf7c1a5d67a2eb553f034aef41d55/x264-b35605ace3ddf7c1a5d67a2eb553f034aef41d55.tar.bz2 6eeb82934e69fd51e043bd8c5b0d152839638d1ce7aa4eea65a3fedcf83ff224 x264 x264-b35605ace3ddf7c1a5d67a2eb553f034aef41d55.tar.bz2
CC=cl ./configure --prefix="$PREFIX" --enable-static --disable-cli --disable-asm --disable-opencl --bit-depth=8 --chroma-format=420 \
    --disable-lavf --disable-swscale --disable-ffms --disable-gpac --disable-lsmash >/dev/null
make -j"$JOBS" >/dev/null
make install >/dev/null
fi

if step vpx; then
fetch https://github.com/webmproject/libvpx/archive/refs/tags/v1.17.0.tar.gz 1020f184046187baa2985dbde38e0691f49c44088bca7a1842b0236c6081dc0a vpx v1.17.0.tar.gz
CC="clang --target=x86_64-pc-windows-msvc" CXX="clang++ --target=x86_64-pc-windows-msvc" AR=llvm-ar LD="clang --target=x86_64-pc-windows-msvc" \
    ./configure --target=generic-gnu --prefix="$PREFIX" --disable-examples --disable-tools --disable-docs --disable-unit-tests --disable-shared --enable-static \
    --enable-vp9-highbitdepth --enable-multithread >/dev/null
for file in Makefile config.mk *-generic-gnu.mk; do sed -i "s#$(pwd)#$(pwd -W)#g" $file; done
make -j"$JOBS" >/dev/null
make install >/dev/null
fi

if step dav1d; then
fetch https://code.videolan.org/videolan/dav1d/-/archive/1.5.4/dav1d-1.5.4.tar.bz2 2abfb0c89212e6e4733a54e0ae509ec00a5b845a6360946f918806e14aedb011 dav1d dav1d-1.5.4.tar.bz2
python -m pip install -q meson >/dev/null
CC=cl meson setup out --buildtype release --default-library static --prefix "$PREFIX" --libdir lib -Db_vscrt=md -Denable_asm=false -Denable_tools=false -Denable_tests=false >/dev/null
ninja -C out -j "$JOBS" >/dev/null
ninja -C out install >/dev/null
fi

ls -l "$PREFIX/lib"
