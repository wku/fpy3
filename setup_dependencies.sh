#!/bin/bash
set -e

VENDOR_DIR="$(pwd)/vendor"
mkdir -p "$VENDOR_DIR"

# --- nghttp3 ---
echo ">>> Building nghttp3..."
cd "$VENDOR_DIR"
if [ ! -d "nghttp3" ]; then
    git clone --depth 1 -b v1.0.0 https://github.com/ngtcp2/nghttp3
fi
cd nghttp3
cmake -S . -B build -DCMAKE_INSTALL_PREFIX="$VENDOR_DIR/dist" -DENABLE_LIB_ONLY=ON -DENABLE_STATIC_LIB=ON -DENABLE_SHARED_LIB=ON
cmake --build build --target install

# --- msquic ---
echo ">>> Building msquic..."
cd "$VENDOR_DIR"
if [ ! -d "msquic" ]; then
    # v2.2.3 is a stable release
    git clone --depth 1 -b v2.2.3 --recursive https://github.com/microsoft/msquic
fi
cd msquic
# MsQuic uses cmake. Linux build.
# We turn off docs, tests, tools to speed it up.
cmake -S . -B build \
    -DCMAKE_INSTALL_PREFIX="$VENDOR_DIR/dist" \
    -DQUIC_BUILD_TEST=OFF \
    -DQUIC_BUILD_TOOLS=OFF \
    -DQUIC_BUILD_PERF=OFF 
cmake --build build --parallel $(nproc) 
cmake --install build

# --- libwtf ---
echo ">>> Building libwtf..."
# libwtf is in the source tree root/libwtf
LIBWTF_SRC="$(pwd)/../../libwtf" 
# relative to vendor/dist where we run this script? No, $(pwd) is FPY_ROOT/vendor/msquic during msquic build.
# Let's fix paths.
cd "$VENDOR_DIR"

if [ ! -d "$LIBWTF_SRC" ]; then
    echo "Using libwtf from source tree: $LIBWTF_SRC"
fi

# We build libwtf OUT of source tree, inside vendor/libwtf-build
mkdir -p libwtf-build
cd libwtf-build

cmake -S "$LIBWTF_SRC" -B . \
    -DCMAKE_INSTALL_PREFIX="$VENDOR_DIR/dist" \
    -DCMAKE_PREFIX_PATH="$VENDOR_DIR/dist" \
    -DWTF_USE_EXTERNAL_MSQUIC=ON \
    -Dmsquic_DIR="$VENDOR_DIR/dist/share/msquic" \
    -DMSQUIC_INCLUDE_DIRS="$VENDOR_DIR/dist/include" \
    -DWTF_BUILD_SAMPLES=OFF \
    -DWTF_BUILD_TESTS=OFF

cmake --build . --parallel $(nproc)
cmake --install .

echo ">>> Dependencies built in $VENDOR_DIR/dist"
