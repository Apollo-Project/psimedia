#!/bin/sh

# Author:  Boris Pek
# Version: N/A
# License: Public Domain

set -e
set -x

EXTRA_FLAGS="${EXTRA_FLAGS} -Wall"
CPPFLAGS="$(dpkg-buildflags --get CPPFLAGS) ${EXTRA_FLAGS}"
CFLAGS="$(dpkg-buildflags --get CFLAGS) ${CPPFLAGS}"
CXXFLAGS="$(dpkg-buildflags --get CXXFLAGS) ${CPPFLAGS}"
LDFLAGS="$(dpkg-buildflags --get LDFLAGS) -Wl,--as-needed"

[ -z "${BUILD_DEMO}" ] && BUILD_DEMO="OFF"

BUILD_OPTIONS="-DCMAKE_INSTALL_PREFIX=/usr \
               -DCMAKE_BUILD_TYPE=Release \
               -DBUILD_DEMO=${BUILD_DEMO}"

mkdir -p builddir
cd builddir

cmake .. ${BUILD_OPTIONS} \
      -DCMAKE_CXX_FLAGS="${CXXFLAGS}" \
      -DCMAKE_SHARED_LINKER_FLAGS="${LDFLAGS}" \
      -DCMAKE_EXE_LINKER_FLAGS="${LDFLAGS}"
make -k -j $(nproc) VERBOSE=1
