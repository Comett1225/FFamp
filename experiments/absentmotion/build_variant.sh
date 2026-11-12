#!/bin/bash
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
MEDIA="$HERE/../.."
APP="${APP_REPO:?set APP_REPO to the AbsentMotion source folder}"
OUT="${1:?output folder}"
TREE="$APP/pc/build/native-mac"
mkdir -p "$OUT"
FLAGS=$(grep -A8 'video_mac.mm.o:' "$TREE/build.ninja" | grep ' FLAGS = ' | sed 's/.*FLAGS = //')
DEFINES=$(grep -A8 'video_mac.mm.o:' "$TREE/build.ninja" | grep ' DEFINES = ' | sed 's/.*DEFINES = //')
INCLUDES=$(grep -A8 'video_mac.mm.o:' "$TREE/build.ninja" | grep ' INCLUDES = ' | sed 's/.*INCLUDES = //')
clang++ $DEFINES $FLAGS $INCLUDES -I"$MEDIA/include" -c "$HERE/video_absent.mm" -o "$OUT/video_absent.o"
LINE=$(grep -n 'build .*/libamdesktop.dylib: CXX_SHARED_LIBRARY_LINKER' "$TREE/build.ninja" | head -1 | cut -d: -f1)
OBJECTS=$(sed -n "${LINE}p" "$TREE/build.ninja" | tr ' ' '\n' | grep '\.o$' | grep -v -e '/video\.cpp\.o$' -e 'video_mac\.mm\.o$' | tr '\n' ' ')
LIBS=$(sed -n "$((LINE + 1)),$((LINE + 8))p" "$TREE/build.ninja" | grep ' LINK_LIBRARIES = ' | sed 's/.*LINK_LIBRARIES = //' | tr ' ' '\n' | grep -v -e '^-lav' -e '^-lsw' -e 'ffmpeg' | tr '\n' ' ')
cd "$TREE"
clang++ -arch arm64 -dynamiclib -Wl,-headerpad_max_install_names -o "$OUT/libamdesktop.dylib" -install_name @rpath/libamdesktop.dylib \
    $OBJECTS "$OUT/video_absent.o" $LIBS "$MEDIA/build/libabsentmedia.a" -framework VideoToolbox -framework CoreMedia -framework AudioToolbox
otool -L "$OUT/libamdesktop.dylib" | grep -c -i -e libav -e libsw || true
