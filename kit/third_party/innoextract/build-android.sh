#!/bin/sh
# Build innoextract's command line for the Android launcher, which installs a
# game from the player's GOG offline installer by running it as the Linux
# launcher runs the system's (host/launcher/linux/installer.cpp). The APK
# carries it as lib/<abi>/libinnoextract.so: the system unpacks the APK's
# libraries into a folder an app may run programs from, and nowhere else.
#
#   kit/third_party/innoextract/build-android.sh <output-dir> [x86_64|arm64-v8a ...]
#
# Writes <output-dir>/<abi>/innoextract for each ABI (both by default), for
# Android 10 (API 29, the app's minimum) with the NDK in ANDROID_NDK_HOME:
# innoextract 1.9 with the Mac helper's patches and android-patches/ (a
# document the launcher links as /proc/self/fd/<n> is read through that
# descriptor, which Android allows where opening it by name is not), Boost 1.89's Filesystem,
# Iostreams and Program_Options sources, bzip2 1.0.8 (Android has no libbz2)
# and liblzma (XZ Utils) compiled in; zlib and iconv are the system's. Every
# source archive is pinned by SHA-256; downloads are kept in
# <output-dir>/downloads for the next build. $CMAKE names the cmake to use.
set -eu

INNO_VERSION=1.9
INNO_SHA256=6344a69fc1ed847d4ed3e272e0da5998948c6b828cb7af39c6321aba6cf88126
INNO_URL="https://constexpr.org/innoextract/files/innoextract-$INNO_VERSION.tar.gz"
BOOST_DIR=boost_1_89_0
BOOST_SHA256=85a33fa22621b4f314f8e85e1a5e2a9363d22e4f4992925d4bb3bc631b5a0c7a
BOOST_URL="https://archives.boost.io/release/1.89.0/source/$BOOST_DIR.tar.bz2"
XZ_VERSION=5.8.1
XZ_SHA256=507825b599356c10dca1cd720c9d0d0c9d5400b9de300af00e4d1ea150795543
XZ_URL="https://github.com/tukaani-project/xz/releases/download/v$XZ_VERSION/xz-$XZ_VERSION.tar.gz"
BZIP2_VERSION=1.0.8
BZIP2_SHA256=ab5a03176ee106d3f0fa90e381da478ddae405918153cca248e682cd0c4a2269
BZIP2_URL="https://sourceware.org/pub/bzip2/bzip2-$BZIP2_VERSION.tar.gz"
# The app's own minimum (platform/android/app/build.gradle.kts.in).
ANDROID_API=29

here=$(cd "$(dirname "$0")" && pwd)
out=${1:?usage: build-android.sh <output-dir> [x86_64|arm64-v8a ...]}
shift
abis=${*:-x86_64 arm64-v8a}
ndk=${ANDROID_NDK_HOME:?set ANDROID_NDK_HOME to the NDK}
cmake=${CMAKE:-cmake}
mkdir -p "$out"
out=$(cd "$out" && pwd)
cache="$out/downloads"
work="$out/build"
mkdir -p "$cache"

# Runs a build step quietly; on failure shows the end of what it printed.
quiet() {
    if ! "$@" > "$work/step.log" 2>&1; then
        tail -40 "$work/step.log" >&2
        exit 1
    fi
}
sha256() { # file
    if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | cut -d' ' -f1; else shasum -a 256 "$1" | cut -d' ' -f1; fi
}
fetch() { # url sha256 file
    if [ -f "$cache/$3" ] && [ "$(sha256 "$cache/$3")" = "$2" ]; then
        return
    fi
    curl -fsSL -o "$cache/$3.part" "$1"
    if [ "$(sha256 "$cache/$3.part")" != "$2" ]; then
        echo "$3: SHA-256 mismatch" >&2
        exit 1
    fi
    mv "$cache/$3.part" "$cache/$3"
}
fetch "$INNO_URL" "$INNO_SHA256" "innoextract-$INNO_VERSION.tar.gz"
fetch "$BOOST_URL" "$BOOST_SHA256" "$BOOST_DIR.tar.bz2"
fetch "$XZ_URL" "$XZ_SHA256" "xz-$XZ_VERSION.tar.gz"
fetch "$BZIP2_URL" "$BZIP2_SHA256" "bzip2-$BZIP2_VERSION.tar.gz"

rm -rf "$work"
mkdir -p "$work/src"
tar -xzf "$cache/innoextract-$INNO_VERSION.tar.gz" -C "$work/src"
tar -xzf "$cache/xz-$XZ_VERSION.tar.gz" -C "$work/src"
tar -xzf "$cache/bzip2-$BZIP2_VERSION.tar.gz" -C "$work/src"
# Only the headers and the three libraries' sources (the archive is 150 MB).
tar -xjf "$cache/$BOOST_DIR.tar.bz2" -C "$work/src" "$BOOST_DIR/boost" \
    "$BOOST_DIR/libs/filesystem/src" "$BOOST_DIR/libs/iostreams/src" \
    "$BOOST_DIR/libs/program_options/src" "$BOOST_DIR/LICENSE_1_0.txt"
inno="$work/src/innoextract-$INNO_VERSION"
# The upstream build fixes and Android's own: the progress hook in
# lib-patches/ is the iOS library's, and the program prints its own progress.
for p in "$here"/patches/*.patch "$here"/android-patches/*.patch; do
    patch -s -p1 -d "$inno" < "$p"
done

for abi in $abis; do
    common="-G Ninja -Wno-dev -DCMAKE_BUILD_TYPE=Release
        -DCMAKE_TOOLCHAIN_FILE=$ndk/build/cmake/android.toolchain.cmake
        -DANDROID_ABI=$abi -DANDROID_PLATFORM=android-$ANDROID_API -DANDROID_STL=c++_static"

    # liblzma alone: no command line tools, translations or docs.
    # shellcheck disable=SC2086
    quiet "$cmake" -S "$work/src/xz-$XZ_VERSION" -B "$work/xz-$abi" $common -DBUILD_SHARED_LIBS=OFF \
        -DXZ_NLS=OFF -DXZ_DOC=OFF -DXZ_TOOL_XZ=OFF -DXZ_TOOL_XZDEC=OFF -DXZ_TOOL_LZMADEC=OFF \
        -DXZ_TOOL_LZMAINFO=OFF -DXZ_TOOL_SCRIPTS=OFF -DXZ_SANDBOX=no
    quiet "$cmake" --build "$work/xz-$abi" --target liblzma

    # shellcheck disable=SC2086
    quiet "$cmake" -S "$here/lib" -B "$work/program-$abi" $common -DRECOMP_INNOEXTRACT_PROGRAM=ON \
        -DINNOEXTRACT_SRC="$inno" -DBOOST_SRC="$work/src/$BOOST_DIR" \
        -DBZIP2_SRC="$work/src/bzip2-$BZIP2_VERSION" \
        -DLZMA_INCLUDE="$work/src/xz-$XZ_VERSION/src/liblzma/api" \
        -DLZMA_LIBRARY="$work/xz-$abi/liblzma.a"
    quiet "$cmake" --build "$work/program-$abi"

    mkdir -p "$out/$abi"
    cp "$work/program-$abi/innoextract" "$out/$abi/innoextract"
    echo "built $out/$abi/innoextract ($(du -h "$out/$abi/innoextract" | cut -f1))"
done
rm -rf "$work"
