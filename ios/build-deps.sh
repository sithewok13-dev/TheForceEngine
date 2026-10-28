#!/bin/bash
# Builds the static libraries the iOS app links, SDL2 and SDL2_image, and
# installs them into one prefix (default: build-ios-deps/ in the repo).
# CI caches that prefix, keyed on this file and the toolchain.
#
#   ios/build-deps.sh [prefix]
set -euo pipefail

SDL_TAG=release-2.32.10
SDL_IMAGE_TAG=release-2.8.12

ROOT=$(cd "$(dirname "$0")/.." && pwd)
PREFIX=${1:-$ROOT/build-ios-deps}
SRC=$PREFIX/src
TOOLCHAIN=$ROOT/ios/toolchain.cmake
mkdir -p "$SRC"

fetch() { # repo tag dir
	[ -d "$SRC/$3" ] || git clone --quiet --depth 1 --branch "$2" "$1" "$SRC/$3"
}

build() { # dir, then extra cmake args
	local dir=$1; shift
	cmake -S "$SRC/$dir" -B "$SRC/$dir/build-ios" -G Ninja \
		--toolchain "$TOOLCHAIN" \
		-DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_INSTALL_PREFIX="$PREFIX" \
		-DCMAKE_INSTALL_LIBDIR=lib \
		-DCMAKE_PREFIX_PATH="$PREFIX" \
		-DCMAKE_FIND_ROOT_PATH="$PREFIX" \
		-DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
		"$@"
	cmake --build "$SRC/$dir/build-ios"
	cmake --install "$SRC/$dir/build-ios"
}

fetch https://github.com/libsdl-org/SDL "$SDL_TAG" SDL2
# No OpenGL (ES) in SDL: ANGLE draws on SDL's Metal view (ios/iosAngle.cpp).
# SDL2main is kept: on iOS it is the real main(), which starts UIKit and then
# calls the engine's main() (renamed SDL_main by SDL_main.h).
build SDL2 \
	-DSDL_SHARED=OFF -DSDL_STATIC=ON \
	-DSDL_OPENGLES=OFF -DSDL_OPENGL=OFF \
	-DSDL_TEST=OFF -DSDL_TESTS=OFF \
	-DSDL2_DISABLE_SDL2MAIN=OFF

# PNG and JPEG through stb_image (no libpng/libjpeg to build); PNG saving
# then uses SDL_image's built-in miniz. TFE only needs PNG and JPEG.
fetch https://github.com/libsdl-org/SDL_image "$SDL_IMAGE_TAG" SDL2_image
build SDL2_image \
	-DBUILD_SHARED_LIBS=OFF \
	-DSDL2IMAGE_SAMPLES=OFF -DSDL2IMAGE_TESTS=OFF \
	-DSDL2IMAGE_VENDORED=OFF -DSDL2IMAGE_DEPS_SHARED=OFF \
	-DSDL2IMAGE_BACKEND_STB=ON -DSDL2IMAGE_BACKEND_IMAGEIO=OFF \
	-DSDL2IMAGE_PNG=ON -DSDL2IMAGE_PNG_SAVE=ON \
	-DSDL2IMAGE_JPG=ON -DSDL2IMAGE_JPG_SAVE=OFF \
	-DSDL2IMAGE_AVIF=OFF -DSDL2IMAGE_JXL=OFF -DSDL2IMAGE_TIF=OFF \
	-DSDL2IMAGE_WEBP=OFF -DSDL2IMAGE_SVG=OFF

echo "Dependencies installed in $PREFIX"
