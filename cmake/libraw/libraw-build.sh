#!/bin/sh
#
# libraw-build.sh  —  Configures and builds LibRaw from source.
#
# LibRaw has no CMake build and no WebAssembly build, so the WebAssembly
# target compiles it from a pinned source tarball.  This script drives the
# upstream autotools build; the environment carries the toolchain settings.
#
# Everything is passed in through the environment so that the CMake side
# stays in control of the toolchain:
#
#   LIBRAW_SRC_DIR     extracted source tree
#   LIBRAW_BUILD_DIR   build tree
#   LIBRAW_CC          C compiler
#   LIBRAW_CXX         C++ compiler
#   LIBRAW_AR          archiver (must understand the target object format)
#   LIBRAW_RANLIB      archive indexer (same)
#   LIBRAW_CFLAGS      C compiler flags
#   LIBRAW_CXXFLAGS    C++ compiler flags
#   LIBRAW_CPPFLAGS    preprocessor flags
#   LIBRAW_LDFLAGS     linker flags
#   LIBRAW_ZLIB_CFLAGS zlib include flags
#   LIBRAW_ZLIB_LIBS   zlib library flags
#   LIBRAW_HOST        the --host triple passed to configure
#   LIBRAW_JOBS        parallel make jobs
#
# Usage:
#   libraw-build.sh configure
#   libraw-build.sh build
set -e

stage="$1"

case "${stage}" in
  configure)
    # The archive may unpack into a versioned directory; find the tree that
    # actually holds the build files.
    if [ ! -f "${LIBRAW_SRC_DIR}/configure.ac" ]; then
      for candidate in "${LIBRAW_SRC_DIR}"/*/; do
        if [ -f "${candidate}configure.ac" ]; then
          LIBRAW_SRC_DIR="${candidate}"
          break
        fi
      done
    fi
    if [ ! -f "${LIBRAW_SRC_DIR}/configure.ac" ]; then
      echo "error: no LibRaw source tree found under ${LIBRAW_SRC_DIR}" >&2
      exit 1
    fi

    if [ ! -x "${LIBRAW_SRC_DIR}/configure" ]; then
      # The release tarball ships a generated configure script; the source
      # archive does not, so it is produced here.
      if ! command -v autoreconf >/dev/null 2>&1; then
        echo "error: autoreconf is required to build LibRaw from the source archive." >&2
        echo "       install autoconf, automake and libtool, or build LibRaw" >&2
        echo "       from the release tarball instead." >&2
        exit 1
      fi
      echo "Generating LibRaw's configure script with autoreconf"
      (cd "${LIBRAW_SRC_DIR}" && autoreconf -fi)
    fi

    mkdir -p "${LIBRAW_BUILD_DIR}"
    cd "${LIBRAW_BUILD_DIR}"
    echo "Configuring LibRaw for ${LIBRAW_HOST}"
    CC="${LIBRAW_CC}" \
    CXX="${LIBRAW_CXX}" \
    AR="${LIBRAW_AR}" \
    RANLIB="${LIBRAW_RANLIB}" \
    CFLAGS="${LIBRAW_CFLAGS}" \
    CXXFLAGS="${LIBRAW_CXXFLAGS}" \
    CPPFLAGS="${LIBRAW_CPPFLAGS}" \
    LDFLAGS="${LIBRAW_LDFLAGS}" \
    ZLIB_CFLAGS="${LIBRAW_ZLIB_CFLAGS}" \
    ZLIB_LIBS="${LIBRAW_ZLIB_LIBS}" \
      "${LIBRAW_SRC_DIR}/configure" \
        --host="${LIBRAW_HOST}" \
        --disable-openmp \
        --disable-lcms \
        --enable-zlib \
        --enable-jpeg \
        --disable-examples \
        --disable-shared \
        --enable-static
    ;;

  build)
    cd "${LIBRAW_BUILD_DIR}"
    make -j"${LIBRAW_JOBS:-4}"
    ;;

  *)
    echo "usage: $0 {configure|build}" >&2
    exit 2
    ;;
esac
