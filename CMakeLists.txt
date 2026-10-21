cmake_minimum_required(VERSION 3.28)

# ---------------------------------------------------------------------------
# 64-bit WebAssembly.
#
# Two independent reasons make Emscripten's default 32-bit wasm target unusable
# for this project:
#
#   * The Adobe Halide host package only accepts a 64-bit consumer, and Halide
#     is needed on the WebAssembly path to AOT-compile the atrous wavelet and
#     guided filter pipelines.
#   * The image sizes this project handles do not fit a 32-bit address space.
#     A 10386 x 7792 sensor-shift capture is 242 M samples, and the pipeline
#     keeps several `double` luma/approximation buffers of that size alive at
#     once - roughly 2 GB each.
#
# The switch has to happen before `project()`, because the Emscripten
# toolchain file inspects CMAKE_C_FLAGS to decide which pointer size it
# reports to CMake.  It only reads that variable, so setting it here is safe.
# ---------------------------------------------------------------------------
set(HIRACO_WASM_TARGET_TRIPLE "wasm64-unknown-emscripten" CACHE STRING
  "WebAssembly target triple used for the Emscripten build")

if(CMAKE_TOOLCHAIN_FILE AND CMAKE_TOOLCHAIN_FILE MATCHES "Emscripten")
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} --target=${HIRACO_WASM_TARGET_TRIPLE}")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} --target=${HIRACO_WASM_TARGET_TRIPLE}")
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} --target=${HIRACO_WASM_TARGET_TRIPLE}")
endif()

project(hiraco LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(HIRACO_BUILD_CLI "Build the hiraco-cli binary" ON)
option(HIRACO_BUILD_GUI "Build the hiraco-gui binary" ON)
option(HIRACO_BUILD_TESTS "Build hiraco core tests" ON)
option(HIRACO_BUILD_WASM "Build a browser/WebAssembly target without the wxWidgets GUI" OFF)
option(HIRACO_WASM_PORTING_HELPER "Emit the native dependency porting checklist for Emscripten builds" ON)
set(HIRACO_HALIDE_DIR "" CACHE PATH
  "Path to the Halide CMake package directory (the directory containing HalideConfig.cmake). \
The Halide tool is a host tool that runs on the build machine and emits code for the target, \
so WebAssembly builds need the host Halide installation; Emscripten hides it from the default \
package search, which is why it can be pointed at explicitly here.")
set(HIRACO_BOOST_INCLUDE_DIR "" CACHE PATH "Override the path to Boost headers for the Adobe XMP toolkit")

if(HIRACO_BUILD_WASM)
  if(NOT EMSCRIPTEN)
    message(FATAL_ERROR
      "HIRACO_BUILD_WASM requires an Emscripten toolchain. Configure with emcmake cmake -DHIRACO_BUILD_WASM=ON ...")
  endif()
  set(HIRACO_BUILD_GUI OFF CACHE BOOL "WebAssembly builds are non-visual" FORCE)
  set(HIRACO_BUILD_TESTS OFF CACHE BOOL "WebAssembly builds skip desktop test binaries" FORCE)
  set(HIRACO_EMSCRIPTEN_PORT_FLAGS -sUSE_ZLIB=1 -sUSE_LIBPNG=1 -sUSE_LIBJPEG=1)
  add_compile_options(${HIRACO_EMSCRIPTEN_PORT_FLAGS})
  add_link_options(${HIRACO_EMSCRIPTEN_PORT_FLAGS})
  # The Adobe DNG SDK runs its compressor on worker threads, and the Halide
  # runtime for this build uses std::thread to evaluate the pipelines in
  # parallel, so the module needs shared memory and a worker pool.
  #
  # OpenMP is enabled as well: Emscripten ships libomp 5.1 and it really does
  # evaluate the parallel regions in parallel (a 200 million iteration
  # reduction finishes in 0.06 s), which matters for the per-pixel
  # reconstruction loops over the sensor-shift captures.
  set(HIRACO_EMSCRIPTEN_THREAD_FLAGS -pthread)
  add_compile_options(${HIRACO_EMSCRIPTEN_THREAD_FLAGS})
  add_link_options(${HIRACO_EMSCRIPTEN_THREAD_FLAGS})
  # The pool is bounded so that a machine with many cores does not spawn an
  # unbounded number of workers inside the sandbox.
  include(ProcessorCount)
  ProcessorCount(HIRACO_WASM_HOST_CORES)
  if(HIRACO_WASM_HOST_CORES LESS 1)
    set(HIRACO_WASM_HOST_CORES 4)
  endif()
  set(HIRACO_WASM_PTHREAD_POOL_SIZE "${HIRACO_WASM_HOST_CORES}" CACHE STRING
    "Number of worker threads in the WebAssembly thread pool")
  add_link_options(-sPTHREAD_POOL_SIZE=${HIRACO_WASM_PTHREAD_POOL_SIZE})
  # The conversion keeps several double precision buffers of the size of the
  # image alive at once.  A 10386 x 7792 sensor-shift capture is 242 M
  # samples, and the native build peaks at about 8 GB of resident memory, so
  # the module has to be able to grow well past the default limits.
  set(HIRACO_EMSCRIPTEN_MEMORY_FLAGS
    -sALLOW_MEMORY_GROWTH=1
    -sMAXIMUM_MEMORY=16GB
    -sINITIAL_MEMORY=256MB
  )
  add_link_options(${HIRACO_EMSCRIPTEN_MEMORY_FLAGS})
  # LibRaw's identify step uses large stack buffers; the default 64 KB stack
  # overflows on the very first RAW file.
  set(HIRACO_EMSCRIPTEN_STACK_FLAGS -sSTACK_SIZE=16777216)
  add_link_options(${HIRACO_EMSCRIPTEN_STACK_FLAGS})
  set(ZLIB_FOUND TRUE)
  set(ZLIB_INCLUDE_DIRS "")
  set(ZLIB_LIBRARIES "")
  set(PNG_FOUND TRUE)
  set(PNG_INCLUDE_DIRS "")
  set(PNG_LIBRARIES "")
  # LibRaw and FFTW3 are resolved for the WebAssembly build by their own
  # porting steps; until those exist there is nothing to link against.
  set(LIBRAW_LIBRARIES "")
  set(LIBRAW_INCLUDE_DIRS "")
  set(FFTW3_LIBRARIES "")
  set(FFTW3_INCLUDE_DIRS "")
  if(HIRACO_WASM_PORTING_HELPER)
    message(STATUS "WASM porting notes: the DNG SDK, LibRaw, FFTW3 and the Halide pipelines are all built for WebAssembly. The WebAssembly build differs from the native one in that it has no SIMD FFT, no OpenMP in the demosaic and no fused multiply-add, so its output differs from the native output in the last bits; see docs/wasm-verification.md.")
  endif()
  message(STATUS "Configuring Hiraco as a WebAssembly target; the desktop GUI and test binaries are intentionally disabled.")
endif()

if(APPLE AND NOT DEFINED CMAKE_OSX_ARCHITECTURES)
  set(CMAKE_OSX_ARCHITECTURES "${CMAKE_HOST_SYSTEM_PROCESSOR}" CACHE STRING "Build architectures for macOS" FORCE)
endif()

# The Adobe DNG SDK is treated as a core conversion dependency rather than as a
# desktop GUI dependency. That keeps the native desktop build unchanged while
# allowing a separate Emscripten/WebAssembly target to reuse the same conversion
# implementation without wxWidgets.
set(HIRACO_DNG_SDK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/dng_sdk_1_7_1" CACHE PATH "Path to the Adobe DNG SDK bundle root")

if(NOT EXISTS "${HIRACO_DNG_SDK_ROOT}/dng_sdk/source")
  message(FATAL_ERROR "Adobe DNG SDK not found at ${HIRACO_DNG_SDK_ROOT}. Ensure the dng_sdk_1_7_1 directory exists in the project root or provide HIRACO_DNG_SDK_ROOT.")
endif()

if(NOT EXISTS "${HIRACO_DNG_SDK_ROOT}/libjxl/libjxl/CMakeLists.txt")
  message(FATAL_ERROR "Bundled libjxl sources were not found under ${HIRACO_DNG_SDK_ROOT}. Ensure the full Adobe DNG SDK bundle is present.")
endif()

if(NOT EXISTS "${HIRACO_DNG_SDK_ROOT}/xmp/toolkit/public/include/XMP.incl_cpp")
  message(FATAL_ERROR "Bundled Adobe XMP toolkit sources were not found under ${HIRACO_DNG_SDK_ROOT}. Ensure the full Adobe DNG SDK bundle is present.")
endif()

# ---------------------------------------------------------------------------
# Versioned Adobe DNG SDK patches.
#
# The bundled SDK is a pristine, gitignored vendor drop. Where the SDK offers
# no public API for something Hiraco needs, the fix is kept as a patch named
# `patches/<sdk-directory-name>_<topic>.patch` and applied here during
# configure. Embedding the SDK directory name (for example dng_sdk_1_7_1)
# encodes the SDK version in the filename, so a patch is never silently
# applied to a different SDK release than the one it was generated against.
# Applying in place also re-applies automatically when the SDK bundle is
# refreshed, and stays a no-op when it has already been applied.
# ---------------------------------------------------------------------------
function(hiraco_apply_sdk_patch topic marker)
  foreach(relative_path IN LISTS HIRACO_SDK_PATCH_FILES)
    set(absolute_path "${CMAKE_CURRENT_SOURCE_DIR}/${relative_path}")
    if(NOT EXISTS "${absolute_path}")
      message(FATAL_ERROR
        "DNG SDK patch ${topic} targets a missing file: ${relative_path}")
    endif()
  endforeach()

  # The SDK directory name carries the version, for example dng_sdk_1_7_1.
  get_filename_component(sdk_version_tag "${HIRACO_DNG_SDK_ROOT}" NAME)

  # Clear any .rej/.orig debris left behind by an interrupted apply. This runs
  # before the idempotency check so a stale file is never left in place.
  foreach(relative_path IN LISTS HIRACO_SDK_PATCH_FILES)
    get_filename_component(touched_directory
      "${CMAKE_CURRENT_SOURCE_DIR}/${relative_path}" PATH)
    file(GLOB stale_patch_debris
      "${touched_directory}/*.rej" "${touched_directory}/*.orig")
    foreach(stale_file IN LISTS stale_patch_debris)
      file(REMOVE "${stale_file}")
    endforeach()
  endforeach()

  # Skip when every touched file already carries this patch's marker. This makes
  # configure idempotent and avoids relying on patch exit codes.
  set(needs_apply FALSE)
  foreach(relative_path IN LISTS HIRACO_SDK_PATCH_FILES)
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/${relative_path}" touched_content)
    string(FIND "${touched_content}" "${marker}" marker_position)
    if(marker_position EQUAL -1)
      set(needs_apply TRUE)
    endif()
  endforeach()
  if(NOT needs_apply)
    message(STATUS "DNG SDK patch ${topic} already applied to ${sdk_version_tag}.")
    return()
  endif()

  # The SDK directory name already resolved above; fall through to patch lookup.
  set(patch_file "${CMAKE_CURRENT_SOURCE_DIR}/patches/${sdk_version_tag}_${topic}.patch")
  if(NOT EXISTS "${patch_file}")
    message(WARNING
      "No DNG SDK patch ${patch_file}; the bundled SDK has not been patched for ${topic}.")
    return()
  endif()

  find_program(HIRACO_PATCH_EXECUTABLE patch)
  if(NOT HIRACO_PATCH_EXECUTABLE)
    message(FATAL_ERROR
      "A patch executable is required to apply DNG SDK patch ${patch_file} but none was found.")
  endif()

  # The marker check above keeps this a no-op once the patch is in place; -N
  # additionally ignores any already-applied hunks.
  message(STATUS
    "Applying DNG SDK patch ${topic} to ${sdk_version_tag}: ${HIRACO_SDK_PATCH_FILES}")
  execute_process(
    COMMAND "${HIRACO_PATCH_EXECUTABLE}" -p1 -N --forward --no-backup-if-mismatch
            -i "${patch_file}"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    RESULT_VARIABLE patch_result
    OUTPUT_QUIET
    ERROR_VARIABLE patch_stderr)
  if(NOT patch_result EQUAL 0)
    message(FATAL_ERROR "Failed to apply DNG SDK patch ${patch_file}: ${patch_stderr}")
  endif()

  # A rejected hunk means the bundle does not match the patch it claims to be
  # for, so surface it instead of silently leaving a partially patched tree.
  foreach(relative_path IN LISTS HIRACO_SDK_PATCH_FILES)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${relative_path}.rej")
      file(READ "${CMAKE_CURRENT_SOURCE_DIR}/${relative_path}.rej" reject_content)
      file(REMOVE "${CMAKE_CURRENT_SOURCE_DIR}/${relative_path}.rej")
      message(FATAL_ERROR
        "DNG SDK patch ${patch_file} was rejected by ${relative_path}; the bundled "
        "SDK does not match the version this patch was generated against.\n${reject_content}")
    endif()
  endforeach()
endfunction()

option(HIRACO_APPLY_SDK_PATCHES "Apply bundled Adobe DNG SDK patches during configure" ON)

# Deflate (Compression=8) for 16-bit linear raw. The stock SDK writer always
# picks lossless JPEG for ttShort payloads and exposes no way to request it.
set(HIRACO_SDK_PATCH_FILES
  dng_sdk_1_7_1/dng_sdk/source/dng_host.h
  dng_sdk_1_7_1/dng_sdk/source/dng_image_writer.cpp
)
hiraco_apply_sdk_patch(deflate "SaveDeflate")

# Find ZLIB first. The browser toolchain provides zlib through the Emscripten
# ports system instead of a host installation, so the native find_package path
# is only used for desktop builds.
if(EMSCRIPTEN)
  set(ZLIB_FOUND TRUE)
  add_library(ZLIB::ZLIB INTERFACE IMPORTED)
  message(STATUS "Using Emscripten zlib port for WASM build")
else()
  find_package(ZLIB REQUIRED)
endif()

if(NOT HIRACO_BUILD_WASM)
  find_package(PkgConfig QUIET)
  if(PKG_CONFIG_FOUND)
    pkg_check_modules(PKG_LIBRAW QUIET libraw)
    pkg_check_modules(PKG_FFTW3 QUIET fftw3)
  endif()

  # Fallback / VCPKG configurations for libraw
  find_package(libraw CONFIG QUIET)
  if(TARGET libraw::libraw)
    set(LIBRAW_LIBRARIES libraw::libraw)
  elseif(PKG_LIBRAW_FOUND)
    set(LIBRAW_LIBRARIES ${PKG_LIBRAW_LIBRARIES})
    set(LIBRAW_INCLUDE_DIRS ${PKG_LIBRAW_INCLUDE_DIRS})
    link_directories(${PKG_LIBRAW_LIBRARY_DIRS})
  else()
    find_path(LIBRAW_INCLUDE_DIR NAMES libraw/libraw.h HINTS /opt/homebrew/include /usr/local/include)
    find_library(LIBRAW_LIBRARY NAMES raw_r libraw_r raw libraw HINTS /opt/homebrew/lib /usr/local/lib)
    if(NOT LIBRAW_INCLUDE_DIR OR NOT LIBRAW_LIBRARY)
      message(FATAL_ERROR "LibRaw not found.")
    endif()
    set(LIBRAW_LIBRARIES ${LIBRAW_LIBRARY})
    set(LIBRAW_INCLUDE_DIRS ${LIBRAW_INCLUDE_DIR})
  endif()
endif()
# --- LibRaw ---
#
# LibRaw has no WebAssembly build, so the WebAssembly target compiles it from
# a pinned source tarball.  The source is fetched through an ExternalProject
# and its checksum is verified; the build fails loudly if the two do not
# match.
#
# The native build keeps using the system library by default, because that one
# is built with SIMD and OpenMP and is therefore faster.  Setting
# HIRACO_USE_BUNDLED_LIBRAW switches the native build over to the same source
# build the WebAssembly target uses.
#
# The bundled build compiles with -ffp-contract=off.  Without it, aarch64
# contracts `a * b + c` into a single fused multiply-add, which WebAssembly
# cannot do; the demosaic is iterative, so the two round differently and
# about 600 of 61 million decoded samples end up differing.  With the
# contraction disabled the difference drops to about 30 samples.
set(HIRACO_LIBRAW_VERSION "0.22.2" CACHE STRING "LibRaw version to build from source")
set(HIRACO_LIBRAW_SHA256
    "627928088300ecde6ca91ffd202e189203f04ad61ad12f0fe9dc57b9a7a0fb3c"
    CACHE STRING "SHA256 of the LibRaw source tarball")
option(HIRACO_USE_BUNDLED_LIBRAW "Build LibRaw from source instead of using the system library" OFF)
if(HIRACO_BUILD_WASM)
  set(HIRACO_USE_BUNDLED_LIBRAW ON CACHE BOOL "" FORCE)
endif()
set(HIRACO_LIBRAW_JOBS "4" CACHE STRING "Parallel jobs used when building LibRaw from source")

if(HIRACO_USE_BUNDLED_LIBRAW)
  include(ExternalProject)

  set(HIRACO_LIBRAW_ROOT "${CMAKE_CURRENT_BINARY_DIR}/libraw")
  # With SOURCE_SUBDIR set, ExternalProject strips the archive's top level
  # directory and unpacks the contents into the subdirectory it creates.
  set(HIRACO_LIBRAW_SOURCE_DIR "${HIRACO_LIBRAW_ROOT}/src/hiraco_libraw_source")
  set(HIRACO_LIBRAW_BUILD_DIR "${HIRACO_LIBRAW_ROOT}/build")

  if(EMSCRIPTEN)
    # The Emscripten sysroot provides zlib and libjpeg through its ports
    # system.  The compiler finds both on its default search paths, so no
    # extra flags are needed.
    set(HIRACO_LIBRAW_CC emcc)
    set(HIRACO_LIBRAW_CXX em++)
    # The host `ar` and `ranlib` cannot index WebAssembly objects, so the
    # Emscripten equivalents are used instead.
    set(HIRACO_LIBRAW_AR emar)
    set(HIRACO_LIBRAW_RANLIB emranlib)
    set(HIRACO_LIBRAW_CFLAGS "--target=${HIRACO_WASM_TARGET_TRIPLE} -O2 -ffp-contract=off -pthread")
    set(HIRACO_LIBRAW_CXXFLAGS "--target=${HIRACO_WASM_TARGET_TRIPLE} -O2 -ffp-contract=off -pthread")
    set(HIRACO_LIBRAW_CPPFLAGS "-DUSE_ZLIB -DUSE_JPEG")
    set(HIRACO_LIBRAW_LDFLAGS "--target=${HIRACO_WASM_TARGET_TRIPLE} -pthread")
    set(HIRACO_LIBRAW_HOST "${HIRACO_WASM_TARGET_TRIPLE}")
    set(HIRACO_LIBRAW_ZLIB_CFLAGS "")
    set(HIRACO_LIBRAW_ZLIB_LIBS "-lz")
  else()
    find_program(HIRACO_LIBRAW_CC NAMES cc gcc clang REQUIRED)
    find_program(HIRACO_LIBRAW_CXX NAMES c++ g++ clang++ REQUIRED)
    find_program(HIRACO_LIBRAW_AR NAMES ar llvm-ar REQUIRED)
    find_program(HIRACO_LIBRAW_RANLIB NAMES ranlib llvm-ranlib REQUIRED)
    set(HIRACO_LIBRAW_CFLAGS "-O2 -ffp-contract=off")
    set(HIRACO_LIBRAW_CXXFLAGS "-O2 -ffp-contract=off")
    set(HIRACO_LIBRAW_CPPFLAGS "-DUSE_ZLIB -DUSE_JPEG")
    set(HIRACO_LIBRAW_LDFLAGS "")
    set(HIRACO_LIBRAW_HOST "")
    find_package(PkgConfig QUIET)
    if(PKG_CONFIG_FOUND)
      pkg_check_modules(PKG_LIBRAW_ZLIB QUIET zlib)
      pkg_check_modules(PKG_LIBRAW_JPEG QUIET libjpeg jpeg-turbo)
    endif()
    if(PKG_LIBRAW_ZLIB_FOUND)
      set(HIRACO_LIBRAW_ZLIB_CFLAGS "${PKG_LIBRAW_ZLIB_INCLUDE_DIRS}")
      set(HIRACO_LIBRAW_ZLIB_LIBS "${PKG_LIBRAW_ZLIB_LINK_LIBRARIES}")
    else()
      set(HIRACO_LIBRAW_ZLIB_CFLAGS "")
      set(HIRACO_LIBRAW_ZLIB_LIBS "-lz")
    endif()
    if(PKG_LIBRAW_JPEG_FOUND)
      set(HIRACO_LIBRAW_CPPFLAGS "-DUSE_ZLIB -DUSE_JPEG ${PKG_LIBRAW_JPEG_INCLUDE_DIRS}")
      set(HIRACO_LIBRAW_LDFLAGS "${PKG_LIBRAW_JPEG_LIBRARY_DIRS}")
    endif()
  endif()

  ExternalProject_Add(hiraco_libraw_source
    URL "https://github.com/LibRaw/LibRaw/archive/refs/tags/${HIRACO_LIBRAW_VERSION}.tar.gz"
    URL_HASH SHA256=${HIRACO_LIBRAW_SHA256}
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR "LibRaw-${HIRACO_LIBRAW_VERSION}"
    PREFIX "${HIRACO_LIBRAW_ROOT}"
    CONFIGURE_COMMAND
      ${CMAKE_COMMAND} -E env
        "LIBRAW_SRC_DIR=${HIRACO_LIBRAW_SOURCE_DIR}"
        "LIBRAW_BUILD_DIR=${HIRACO_LIBRAW_BUILD_DIR}"
        "LIBRAW_CC=${HIRACO_LIBRAW_CC}"
        "LIBRAW_CXX=${HIRACO_LIBRAW_CXX}"
        "LIBRAW_AR=${HIRACO_LIBRAW_AR}"
        "LIBRAW_RANLIB=${HIRACO_LIBRAW_RANLIB}"
        "LIBRAW_CFLAGS=${HIRACO_LIBRAW_CFLAGS}"
        "LIBRAW_CXXFLAGS=${HIRACO_LIBRAW_CXXFLAGS}"
        "LIBRAW_CPPFLAGS=${HIRACO_LIBRAW_CPPFLAGS}"
        "LIBRAW_LDFLAGS=${HIRACO_LIBRAW_LDFLAGS}"
        "LIBRAW_ZLIB_CFLAGS=${HIRACO_LIBRAW_ZLIB_CFLAGS}"
        "LIBRAW_ZLIB_LIBS=${HIRACO_LIBRAW_ZLIB_LIBS}"
        "LIBRAW_HOST=${HIRACO_LIBRAW_HOST}"
        "LIBRAW_JOBS=${HIRACO_LIBRAW_JOBS}"
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/libraw/libraw-build.sh" configure
    BUILD_COMMAND
      ${CMAKE_COMMAND} -E env
        "LIBRAW_SRC_DIR=${HIRACO_LIBRAW_SOURCE_DIR}"
        "LIBRAW_BUILD_DIR=${HIRACO_LIBRAW_BUILD_DIR}"
        "LIBRAW_JOBS=${HIRACO_LIBRAW_JOBS}"
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/libraw/libraw-build.sh" build
    INSTALL_COMMAND ""
    BUILD_BYPRODUCTS
      "${HIRACO_LIBRAW_BUILD_DIR}/lib/.libs/libraw.a"
  )

  add_custom_target(hiraco-libraw-ready DEPENDS hiraco_libraw_source)

  add_library(hiraco-libraw STATIC IMPORTED GLOBAL)
  set_target_properties(hiraco-libraw PROPERTIES
    IMPORTED_LOCATION "${HIRACO_LIBRAW_BUILD_DIR}/lib/.libs/libraw.a"
    INTERFACE_INCLUDE_DIRECTORIES "${HIRACO_LIBRAW_SOURCE_DIR}"
  )

  set(LIBRAW_LIBRARIES hiraco-libraw)
  set(LIBRAW_INCLUDE_DIRS "${HIRACO_LIBRAW_SOURCE_DIR}")
  message(STATUS "Using bundled LibRaw ${HIRACO_LIBRAW_VERSION} for ${HIRACO_LIBRAW_HOST}")
endif()

# --- FFTW3 ---
#
# FFTW has no supported WebAssembly build, but its own CMake build is
# portable: with the SSE and AVX options off (its defaults) it compiles the
# generic codelets, which run on any target.  The threads library is built
# as well, because the Stage 1 Wiener filter uses fftw_init_threads().
#
# The WebAssembly build always uses the bundled copy.  The native build uses
# the system library by default, because that one is built with SIMD and
# therefore produces slightly different (and faster) results; setting
# HIRACO_USE_BUNDLED_FFTW switches the native build over as well.
set(HIRACO_FFTW_VERSION "3.3.11" CACHE STRING "FFTW version to build from source")
set(HIRACO_FFTW_SHA256
    "5630c24cdeb33b131612f7eb4b1a9934234754f9f388ff8617458d0be6f239a1"
    CACHE STRING "SHA256 of the FFTW source tarball")
option(HIRACO_USE_BUNDLED_FFTW "Build FFTW from source instead of using the system library" OFF)
if(HIRACO_BUILD_WASM)
  set(HIRACO_USE_BUNDLED_FFTW ON CACHE BOOL "" FORCE)
endif()

if(HIRACO_USE_BUNDLED_FFTW)
  # FetchContent is an ExternalProject under the hood: the tarball is
  # downloaded from a pinned URL and its checksum is verified, and the
  # build fails loudly if the two do not match.
  include(FetchContent)
  FetchContent_Declare(hiraco_fftw
    URL "https://www.fftw.org/fftw-${HIRACO_FFTW_VERSION}.tar.gz"
    URL_HASH SHA256=${HIRACO_FFTW_SHA256}
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  )
  # FFTW's own options.  The SIMD families default to off, which is what
  # makes the generic codelets portable to WebAssembly.
  set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
  set(DISABLE_FORTRAN ON CACHE BOOL "" FORCE)
  set(ENABLE_THREADS ON CACHE BOOL "" FORCE)
  set(ENABLE_OPENMP OFF CACHE BOOL "" FORCE)
  set(ENABLE_SSE OFF CACHE BOOL "" FORCE)
  set(ENABLE_SSE2 OFF CACHE BOOL "" FORCE)
  set(ENABLE_AVX OFF CACHE BOOL "" FORCE)
  set(ENABLE_AVX2 OFF CACHE BOOL "" FORCE)
  # FFTW 3.3.11 still declares cmake_minimum_required(VERSION 3.0), which
  # CMake 4 no longer accepts.  This restores the old policy behaviour for
  # the fetched project only.
  set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
  # FFTW looks for a separate libm.  Emscripten merges libm into libc, so
  # the lookup fails and CMake would abort on the NOTFOUND value.  The value
  # is only used as a "is there a libm" flag - FFTW then links the literal
  # `m`, which the Emscripten toolchain accepts.
  if(EMSCRIPTEN)
    set(LIBM_LIBRARY "m" CACHE STRING "The math library FFTW links against" FORCE)
  endif()
  FetchContent_MakeAvailable(hiraco_fftw)
  set(FFTW3_LIBRARIES fftw3 fftw3_threads)
  set(FFTW3_INCLUDE_DIRS "${hiraco_fftw_SOURCE_DIR}/api")
  message(STATUS "Using bundled FFTW ${HIRACO_FFTW_VERSION} (generic codelets)")
else()
  # Fallback / VCPKG configurations for fftw3
  find_package(FFTW3 CONFIG QUIET)
  if(TARGET FFTW3::fftw3)
    set(FFTW3_LIBRARIES FFTW3::fftw3)
  elseif(PKG_FFTW3_FOUND)
    set(FFTW3_LIBRARIES ${PKG_FFTW3_LIBRARIES})
    set(FFTW3_INCLUDE_DIRS ${PKG_FFTW3_INCLUDE_DIRS})
    link_directories(${PKG_FFTW3_LIBRARY_DIRS})
  else()
    find_path(FFTW3_INCLUDE_DIR NAMES fftw3.h HINTS /opt/homebrew/include /usr/local/include)
    find_library(FFTW3_LIBRARY NAMES fftw3 fftw3-3 HINTS /opt/homebrew/lib /usr/local/lib)
    if(NOT FFTW3_INCLUDE_DIR OR NOT FFTW3_LIBRARY)
      message(FATAL_ERROR "FFTW3 not found.")
    endif()
    set(FFTW3_LIBRARIES ${FFTW3_LIBRARY})
    set(FFTW3_INCLUDE_DIRS ${FFTW3_INCLUDE_DIR})
  endif()

  # FFTW3 threading library (pthreads-based, works on all platforms)
  find_library(FFTW3_THREADS_LIBRARY NAMES fftw3_threads HINTS /opt/homebrew/lib /usr/local/lib)
  if(NOT FFTW3_THREADS_LIBRARY)
    message(FATAL_ERROR "FFTW3 threads library (fftw3_threads) not found. Install fftw with threading support.")
  endif()
  list(APPEND FFTW3_LIBRARIES ${FFTW3_THREADS_LIBRARY})
  message(STATUS "Found FFTW3 threads: ${FFTW3_THREADS_LIBRARY}")
endif()

# OpenMP for loop-level parallelism
# Apple Clang needs explicit hints to find Homebrew libomp
if(APPLE)
  execute_process(
    COMMAND brew --prefix libomp
    OUTPUT_VARIABLE HOMEBREW_LIBOMP_PREFIX
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
  )
  if(HOMEBREW_LIBOMP_PREFIX)
    set(OpenMP_C_FLAGS "-Xpreprocessor -fopenmp -I${HOMEBREW_LIBOMP_PREFIX}/include")
    set(OpenMP_CXX_FLAGS "-Xpreprocessor -fopenmp -I${HOMEBREW_LIBOMP_PREFIX}/include")
    set(OpenMP_C_LIB_NAMES "omp")
    set(OpenMP_CXX_LIB_NAMES "omp")
    set(OpenMP_omp_LIBRARY "${HOMEBREW_LIBOMP_PREFIX}/lib/libomp.dylib")
  endif()
endif()
find_package(OpenMP)
if(OpenMP_CXX_FOUND)
  message(STATUS "OpenMP found: ${OpenMP_CXX_FLAGS}")
else()
  message(STATUS "OpenMP not found; loop-level parallelism disabled")
endif()

# Halide for AOT-compiled image processing pipelines (wavelet, guided filter).
#
# The Halide tool itself always runs on the host: it is the *emitted* code
# that differs between the two builds.  The target names are identical in both
# cases so that `hiraco-core` links the same way regardless of the toolchain.
if(EMSCRIPTEN AND NOT HIRACO_HALIDE_DIR)
  # Emscripten sets CMAKE_FIND_ROOT_PATH_MODE_PACKAGE to ONLY and narrows
  # CMAKE_SYSTEM_PREFIX_PATH to its own sysroot, so the host Halide
  # installation is invisible to find_package().  Look for it in the usual
  # host prefixes; HIRACO_HALIDE_DIR overrides this.
  foreach(_hiraco_host_prefix IN ITEMS /opt/homebrew /usr/local)
    if(EXISTS "${_hiraco_host_prefix}/lib/cmake/Halide/HalideConfig.cmake")
      set(HIRACO_HALIDE_DIR "${_hiraco_host_prefix}/lib/cmake/Halide"
          CACHE PATH "Path to the Halide CMake package directory" FORCE)
      message(STATUS "Using host Halide package at ${HIRACO_HALIDE_DIR}")
      break()
    endif()
  endforeach()
endif()
if(HIRACO_HALIDE_DIR)
  # find_package() consults <Package>_DIR directly, so this bypasses the
  # Emscripten root-path restriction.  HalideConfig.cmake pulls in
  # HalideHelpers with a HINT, and HINTs are subject to the same restriction,
  # so that package has to be pointed at as well.
  set(Halide_DIR "${HIRACO_HALIDE_DIR}" CACHE PATH "Halide CMake package directory" FORCE)
  get_filename_component(_hiraco_halide_prefix "${HIRACO_HALIDE_DIR}/../../.." ABSOLUTE)
  if(EXISTS "${_hiraco_halide_prefix}/lib/cmake/HalideHelpers/HalideHelpersConfig.cmake")
    set(HalideHelpers_DIR "${_hiraco_halide_prefix}/lib/cmake/HalideHelpers"
        CACHE PATH "HalideHelpers CMake package directory" FORCE)
  endif()
endif()
find_package(Halide REQUIRED)

# ---------------------------------------------------------------------------
# Halide AOT pipelines for the WebAssembly build.
#
# Halide 21 cannot emit WebAssembly for a 64-bit target: its `wasmrt`
# backend only supports wasm32 ("Only wasm32 is supported").  A wasm32 object
# cannot be linked into the wasm64 module this build produces, and wasm64 is
# not optional: a 10386 x 7792 sensor-shift capture peaks at about 8 GB of
# resident memory in the native build, well past the 4 GB ceiling of wasm32.
#
# The pipeline code Halide generates is nevertheless ordinary, target-neutral
# C++.  Only the *object file* Halide would emit is tied to wasm32.  So the
# WebAssembly build asks the generators for C source and compiles that source
# with the Emscripten toolchain, which produces wasm64 objects.
#
# The generators themselves are host tools - they run on the build machine and
# cannot be built with the Emscripten toolchain - so they are compiled here
# with the host compiler against the host Halide library.
# ---------------------------------------------------------------------------
if(HIRACO_BUILD_WASM)
  set(HIRACO_WASM_HALIDE_TARGET "wasm-32-wasmrt-no_runtime" CACHE STRING
    "Halide target triple used to generate the WebAssembly pipeline sources")

  get_filename_component(HIRACO_HALIDE_PREFIX "${HIRACO_HALIDE_DIR}/../../.." ABSOLUTE)
  # NO_CMAKE_FIND_ROOT_PATH: these are host files, and Emscripten restricts
  # every search path to its own sysroot.
  find_path(HIRACO_HALIDE_INCLUDE_DIR NAMES Halide.h
            HINTS "${HIRACO_HALIDE_PREFIX}/include"
            NO_CMAKE_FIND_ROOT_PATH REQUIRED)

  # The Halide libraries are taken from the imported targets that
  # find_package(Halide) already resolved, so the exact file names of the
  # installation (which are versioned) do not have to be guessed.
  get_target_property(_hiraco_halide_lib Halide::Halide IMPORTED_LOCATION_RELEASE)
  if(NOT _hiraco_halide_lib)
    get_target_property(_hiraco_halide_lib Halide::Halide IMPORTED_LOCATION)
  endif()
  get_target_property(_hiraco_halide_gengen_lib Halide::GenGen IMPORTED_LOCATION_RELEASE)
  if(NOT _hiraco_halide_gengen_lib)
    get_target_property(_hiraco_halide_gengen_lib Halide::GenGen IMPORTED_LOCATION)
  endif()
  if(NOT _hiraco_halide_lib OR NOT _hiraco_halide_gengen_lib)
    find_library(HIRACO_HALIDE_LIBRARY NAMES Halide
                 HINTS "${HIRACO_HALIDE_PREFIX}/lib"
                 NO_CMAKE_FIND_ROOT_PATH REQUIRED)
    find_library(HIRACO_HALIDE_GENGEN_LIBRARY NAMES Halide_GenGen
                 HINTS "${HIRACO_HALIDE_PREFIX}/lib"
                 NO_CMAKE_FIND_ROOT_PATH REQUIRED)
  else()
    set(HIRACO_HALIDE_LIBRARY "${_hiraco_halide_lib}")
    set(HIRACO_HALIDE_GENGEN_LIBRARY "${_hiraco_halide_gengen_lib}")
  endif()
  message(STATUS "Halide host library: ${HIRACO_HALIDE_LIBRARY}")
  message(STATUS "Halide generator library: ${HIRACO_HALIDE_GENGEN_LIBRARY}")

  if(CMAKE_HOST_APPLE)
    set(HIRACO_HALIDE_WHOLE_ARCHIVE "-Wl,-force_load")
  else()
    set(HIRACO_HALIDE_WHOLE_ARCHIVE "-Wl,--whole-archive")
  endif()

  # The Halide generators are host tools, so they need a host C++ compiler.
  # CMAKE_HOST_CXX_COMPILER is not always populated in a cross-compiling
  # project, so fall back to the `c++` on PATH and let it be overridden.
  if(NOT HIRACO_HOST_CXX_COMPILER)
    if(CMAKE_HOST_CXX_COMPILER)
      set(HIRACO_HOST_CXX_COMPILER "${CMAKE_HOST_CXX_COMPILER}" CACHE FILEPATH
          "Host C++ compiler used to build the Halide generator tools")
    else()
      find_program(HIRACO_HOST_CXX_COMPILER NAMES c++ g++ clang++)
    endif()
  endif()
  message(STATUS "Halide host compiler: ${HIRACO_HOST_CXX_COMPILER}")

  # The Halide runtime for the WebAssembly build.  The official runtime
  # cannot be generated for a 64-bit WebAssembly target, so this provides the
  # entry points the generated pipeline sources reference.
  add_library(hiraco-halide-runtime STATIC
    src/halide/halide_wasm_runtime.cpp
  )
  target_include_directories(hiraco-halide-runtime PUBLIC
    "${HIRACO_HALIDE_INCLUDE_DIR}"
  )

  # Builds one host generator and turns it into a WebAssembly pipeline
  # library.  The library is named exactly like the native one so that
  # `hiraco-core` links the same way in both builds.
  function(hiraco_wasm_halide_pipeline target generator_name generator_source)
    set(gen_dir "${CMAKE_CURRENT_BINARY_DIR}/halide-host")
    set(gen_exe "${gen_dir}/${target}_gen")
    set(out_dir "${CMAKE_CURRENT_BINARY_DIR}/halide-generated")
    file(MAKE_DIRECTORY "${gen_dir}")
    file(MAKE_DIRECTORY "${out_dir}")

    add_custom_command(
      OUTPUT "${gen_exe}"
      COMMAND ${HIRACO_HOST_CXX_COMPILER} -O2 -std=c++17
              -I${HIRACO_HALIDE_INCLUDE_DIR}
              ${generator_source}
              -o ${gen_exe}
              -L${HIRACO_HALIDE_PREFIX}/lib
              ${HIRACO_HALIDE_WHOLE_ARCHIVE} ${HIRACO_HALIDE_GENGEN_LIBRARY}
              ${HIRACO_HALIDE_LIBRARY}
      DEPENDS "${generator_source}"
      COMMENT "Building host Halide generator ${target}_gen"
      VERBATIM)

    set(generated_source "${out_dir}/${target}.halide_generated.cpp")
    set(generated_header "${out_dir}/${target}.h")
    add_custom_command(
      OUTPUT "${generated_source}" "${generated_header}"
      COMMAND "${gen_exe}"
              -n "${target}"
              -g "${generator_name}"
              -f "${target}"
              -e c_header,c_source
              -o "${out_dir}"
              "target=${HIRACO_WASM_HALIDE_TARGET}"
      DEPENDS "${gen_exe}"
      COMMENT "Generating WebAssembly Halide pipeline ${target}"
      VERBATIM)

    add_library("${target}" STATIC "${generated_source}")
    target_include_directories("${target}" PUBLIC
      "${out_dir}"
      "${HIRACO_HALIDE_INCLUDE_DIR}"
    )
    target_link_libraries("${target}" PUBLIC hiraco-halide-runtime)
    set_target_properties("${target}" PROPERTIES LINKER_LANGUAGE CXX)
  endfunction()

  hiraco_wasm_halide_pipeline(hiraco_atrous_wavelet atrous_wavelet
                              "${CMAKE_CURRENT_SOURCE_DIR}/src/halide/wavelet_generator.cpp")
  hiraco_wasm_halide_pipeline(hiraco_guided_filter guided_filter
                              "${CMAKE_CURRENT_SOURCE_DIR}/src/halide/guided_filter_generator.cpp")

  set(HIRACO_HALIDE_LIBRARIES
    hiraco_atrous_wavelet
    hiraco_guided_filter
    hiraco-halide-runtime)
endif()

function(hiraco_resolve_emscripten_sysroot_include_dir out_var)
  if(DEFINED EMSCRIPTEN_SYSROOT AND EMSCRIPTEN_SYSROOT)
    set(${out_var} "${EMSCRIPTEN_SYSROOT}/include" PARENT_SCOPE)
    return()
  endif()

  if(DEFINED CMAKE_SYSROOT AND CMAKE_SYSROOT)
    set(${out_var} "${CMAKE_SYSROOT}/include" PARENT_SCOPE)
    return()
  endif()

  execute_process(
    COMMAND "${CMAKE_CXX_COMPILER}" -print-sysroot
    OUTPUT_VARIABLE HIRACO_EMSCRIPTEN_SYSROOT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)

  if(HIRACO_EMSCRIPTEN_SYSROOT)
    set(${out_var} "${HIRACO_EMSCRIPTEN_SYSROOT}/include" PARENT_SCOPE)
  else()
    set(${out_var} "" PARENT_SCOPE)
  endif()
endfunction()

if(EMSCRIPTEN)
  hiraco_resolve_emscripten_sysroot_include_dir(HIRACO_EMSCRIPTEN_SYSROOT_INCLUDE_DIR)
  if(NOT HIRACO_EMSCRIPTEN_SYSROOT_INCLUDE_DIR)
    message(FATAL_ERROR "Could not determine the Emscripten sysroot include directory. Ensure the toolchain is configured correctly.")
  endif()
endif()

set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(JPEGXL_ENABLE_TOOLS OFF CACHE BOOL "" FORCE)
set(JPEGXL_ENABLE_MANPAGES OFF CACHE BOOL "" FORCE)
set(JPEGXL_ENABLE_BENCHMARK OFF CACHE BOOL "" FORCE)
set(JPEGXL_ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)
set(JPEGXL_ENABLE_JNI OFF CACHE BOOL "" FORCE)
set(JPEGXL_ENABLE_PLUGINS OFF CACHE BOOL "" FORCE)
set(JPEGXL_ENABLE_FUZZERS OFF CACHE BOOL "" FORCE)
set(JPEGXL_ENABLE_DEVTOOLS OFF CACHE BOOL "" FORCE)
set(JPEGXL_ENABLE_DOXYGEN OFF CACHE BOOL "" FORCE)
set(JPEGXL_ENABLE_SJPEG OFF CACHE BOOL "" FORCE)
set(JPEGXL_ENABLE_OPENEXR OFF CACHE BOOL "" FORCE)
set(JPEGXL_ENABLE_VIEWERS OFF CACHE BOOL "" FORCE)
set(JPEGXL_ENABLE_TCMALLOC OFF CACHE BOOL "" FORCE)

if(EMSCRIPTEN)
  set(XMP_OS_FLAG "WEB_ENV=1")
  set(DNG_OS_FLAG "qLinux=1")
  # libjxl defaults to bundling libpng on Emscripten, but the toolchain already
  # provides libpng/zlib through the Emscripten ports system. Keeping bundled PNG
  # disabled avoids a configure-time failure when the vendor submodule is absent.
  set(JPEGXL_BUNDLE_LIBPNG OFF CACHE BOOL "Use the Emscripten-provided libpng/zlib instead of vendor sources" FORCE)
elseif(APPLE)
  set(XMP_OS_FLAG "MAC_ENV=1")
  set(DNG_OS_FLAG "qMacOS=1")
  find_library(COREFOUNDATION_LIBRARY CoreFoundation)
  find_library(CORESERVICES_LIBRARY CoreServices)
elseif(WIN32)
  set(XMP_OS_FLAG "WIN_ENV=1")
  set(DNG_OS_FLAG "qWinOS=1")
else()
  set(XMP_OS_FLAG "UNIX_ENV=1")
  set(DNG_OS_FLAG "qLinux=1")
endif()

set(XMP_RESOURCE_DIR "")
if(EMSCRIPTEN)
  set(XMP_RESOURCE_DIR "${HIRACO_DNG_SDK_ROOT}/xmp/toolkit/XMPCore/resource/linux")
elseif(APPLE)
  set(XMP_RESOURCE_DIR "${HIRACO_DNG_SDK_ROOT}/xmp/toolkit/XMPCore/resource/mac")
elseif(WIN32)
  set(XMP_RESOURCE_DIR "${HIRACO_DNG_SDK_ROOT}/xmp/toolkit/XMPCore/resource/win")
else()
  set(XMP_RESOURCE_DIR "${HIRACO_DNG_SDK_ROOT}/xmp/toolkit/XMPCore/resource/linux")
endif()

file(GLOB HIRACO_XMPCORE_SOURCES CONFIGURE_DEPENDS
  "${HIRACO_DNG_SDK_ROOT}/xmp/toolkit/XMPCore/source/WXMP*.cpp"
  "${HIRACO_DNG_SDK_ROOT}/xmp/toolkit/XMPCore/source/XMP*.cpp"
  "${HIRACO_DNG_SDK_ROOT}/xmp/toolkit/XMPCore/source/CoreObjectFactoryImpl.cpp"
  "${HIRACO_DNG_SDK_ROOT}/xmp/toolkit/XMPCore/source/ExpatAdapter.cpp"
  "${HIRACO_DNG_SDK_ROOT}/xmp/toolkit/XMPCore/source/ParseRDF.cpp"
  "${HIRACO_DNG_SDK_ROOT}/xmp/toolkit/XMPCore/source/XMPCore_Impl.cpp"
  "${HIRACO_DNG_SDK_ROOT}/xmp/toolkit/source/UnicodeConversions.cpp"
  "${HIRACO_DNG_SDK_ROOT}/xmp/toolkit/source/XML_Node.cpp"
  "${HIRACO_DNG_SDK_ROOT}/xmp/toolkit/source/XMP_LibUtils.cpp"
  "${HIRACO_DNG_SDK_ROOT}/xmp/toolkit/third-party/zuid/sources/*.cpp"
)

add_library(hiraco-xmp-core STATIC
  ${HIRACO_XMPCORE_SOURCES}
  ${HIRACO_DNG_SDK_ROOT}/xmp/toolkit/XMPCore/third-party/expat/public/lib/xmlparse.c
  ${HIRACO_DNG_SDK_ROOT}/xmp/toolkit/XMPCore/third-party/expat/public/lib/xmlrole.c
  ${HIRACO_DNG_SDK_ROOT}/xmp/toolkit/XMPCore/third-party/expat/public/lib/xmltok.c
)
target_compile_definitions(hiraco-xmp-core PRIVATE
  ${XMP_OS_FLAG}
  BUILDING_XMPCORE_LIB=1
  BUILDING_XMPCORE_AS_STATIC=1
  XMP_COMPONENT_INT_NAMESPACE=AdobeXMPCore_Int
  ENABLE_CPP_DOM_MODEL=0
  XML_STATIC=1
  HAVE_EXPAT_CONFIG_H=1
  XMP_StaticBuild=1
)
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
  target_compile_definitions(hiraco-xmp-core PRIVATE XMP_64=1)
else()
  target_compile_definitions(hiraco-xmp-core PRIVATE XMP_64=0)
endif()
set(BOOST_INCLUDE_DIRS "")
if(HIRACO_BOOST_INCLUDE_DIR)
  set(BOOST_INCLUDE_DIRS "${HIRACO_BOOST_INCLUDE_DIR}")
else()
  find_package(Boost QUIET)
  if(Boost_FOUND AND Boost_INCLUDE_DIRS)
    set(BOOST_INCLUDE_DIRS "${Boost_INCLUDE_DIRS}")
  else()
    set(_hiraco_boost_hint_roots)
    if(DEFINED ENV{BOOST_ROOT})
      list(APPEND _hiraco_boost_hint_roots "$ENV{BOOST_ROOT}")
    endif()
    if(DEFINED ENV{BOOST_INCLUDEDIR})
      list(APPEND _hiraco_boost_hint_roots "$ENV{BOOST_INCLUDEDIR}")
    endif()
    list(APPEND _hiraco_boost_hint_roots
      /opt/homebrew/include
      /opt/homebrew/opt/boost/include
      /usr/local/include
      /usr/include
    )
    foreach(_hiraco_boost_root IN LISTS CMAKE_PREFIX_PATH)
      list(APPEND _hiraco_boost_hint_roots "${_hiraco_boost_root}/include")
    endforeach()
    foreach(_hiraco_boost_root IN LISTS _hiraco_boost_hint_roots)
      if(EXISTS "${_hiraco_boost_root}/boost/uuid/uuid.hpp")
        set(BOOST_INCLUDE_DIRS "${_hiraco_boost_root}")
        break()
      elseif(EXISTS "${_hiraco_boost_root}/boost/version.hpp")
        set(BOOST_INCLUDE_DIRS "${_hiraco_boost_root}")
        break()
      endif()
    endforeach()
  endif()
endif()
if(NOT BOOST_INCLUDE_DIRS)
  message(FATAL_ERROR "Boost headers not found; required by the Adobe XMP toolkit used by the DNG SDK. Set HIRACO_BOOST_INCLUDE_DIR or install Boost in a standard location.")
endif()
target_include_directories(hiraco-xmp-core PRIVATE ${BOOST_INCLUDE_DIRS})

if(EMSCRIPTEN)
  target_include_directories(hiraco-xmp-core PRIVATE ${HIRACO_EMSCRIPTEN_SYSROOT_INCLUDE_DIR})
endif()

target_include_directories(hiraco-xmp-core PRIVATE
  ${HIRACO_DNG_SDK_ROOT}/xmp/toolkit
  ${HIRACO_DNG_SDK_ROOT}/xmp/toolkit/public/include
  ${HIRACO_DNG_SDK_ROOT}/xmp/toolkit/XMPCore/third-party/expat/public/lib
  ${XMP_RESOURCE_DIR}
  ${HIRACO_DNG_SDK_ROOT}/xmp/toolkit/XMPFiles/resource/linux
)

if(APPLE)
  target_include_directories(hiraco-xmp-core PRIVATE
    ${HIRACO_DNG_SDK_ROOT}/xmp/toolkit/XMPCore/resource/mac
  )
  target_link_libraries(hiraco-xmp-core PRIVATE
    ${COREFOUNDATION_LIBRARY}
    ${CORESERVICES_LIBRARY}
  )
endif()

add_subdirectory(${HIRACO_DNG_SDK_ROOT}/libjxl/libjxl ${CMAKE_CURRENT_BINARY_DIR}/libjxl EXCLUDE_FROM_ALL)

if(NOT HIRACO_BUILD_WASM)
  # --- Halide AOT generators ---
  add_halide_generator(hiraco_wavelet_gen
    SOURCES src/halide/wavelet_generator.cpp)
  add_halide_generator(hiraco_guided_filter_gen
    SOURCES src/halide/guided_filter_generator.cpp)

  add_halide_library(hiraco_atrous_wavelet FROM hiraco_wavelet_gen
    GENERATOR atrous_wavelet)
  add_halide_library(hiraco_guided_filter FROM hiraco_guided_filter_gen
    GENERATOR guided_filter)

  set(HIRACO_HALIDE_LIBRARIES hiraco_atrous_wavelet hiraco_guided_filter)
endif()
# The WebAssembly build sets HIRACO_HALIDE_LIBRARIES earlier, where the
# cross-compiled pipelines are created.
file(GLOB HIRACO_DNG_SDK_SOURCES CONFIGURE_DEPENDS
  "${HIRACO_DNG_SDK_ROOT}/dng_sdk/source/*.cpp"
)
list(FILTER HIRACO_DNG_SDK_SOURCES EXCLUDE REGEX ".*/dng_validate\\.cpp$")
list(FILTER HIRACO_DNG_SDK_SOURCES EXCLUDE REGEX ".*/dng_update_meta\\.cpp$")

add_library(hiraco-dng-sdk STATIC
  ${HIRACO_DNG_SDK_SOURCES}
)
target_compile_definitions(hiraco-dng-sdk PRIVATE
  ${DNG_OS_FLAG}
  ${XMP_OS_FLAG}
  BUILDING_XMPCORE_LIB=1
  BUILDING_XMPCORE_AS_STATIC=1
  ENABLE_CPP_DOM_MODEL=0
  HAVE_EXPAT_CONFIG_H=1
  XML_STATIC=1
  XMP_COMPONENT_INT_NAMESPACE=AdobeXMPCore_Int
  XMP_StaticBuild=1
  qDNGUseXMP=1
  qDNGXMPFiles=0
  qDNGUseLibJPEG=0
)
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
  target_compile_definitions(hiraco-dng-sdk PRIVATE XMP_64=1)
else()
  target_compile_definitions(hiraco-dng-sdk PRIVATE XMP_64=0)
endif()
target_include_directories(hiraco-dng-sdk PRIVATE
  ${HIRACO_EMSCRIPTEN_SYSROOT_INCLUDE_DIR}
  ${HIRACO_DNG_SDK_ROOT}/dng_sdk/source
  ${HIRACO_DNG_SDK_ROOT}/libjxl/libjxl/lib/include
  ${HIRACO_DNG_SDK_ROOT}/libjpeg
  ${HIRACO_DNG_SDK_ROOT}/xmp/toolkit/public/include
)
target_link_libraries(hiraco-dng-sdk PRIVATE
  jxl
  jxl_threads
  hiraco-xmp-core
  ZLIB::ZLIB
)

if(APPLE)
  target_link_libraries(hiraco-dng-sdk PRIVATE
    ${COREFOUNDATION_LIBRARY}
    ${CORESERVICES_LIBRARY}
  )
endif()

# ---------------------------------------------------------------------------
# The shared processing core.
#
# There is exactly one definition of `hiraco-core`.  The native and the
# WebAssembly builds compile the same sources and differ only in which
# dependency libraries they link, so the two builds cannot drift apart.  The
# WebAssembly build used to compile a reduced core without the conversion
# pipeline; that reduced variant is gone.
# ---------------------------------------------------------------------------
if(HIRACO_BUILD_WASM)
  # The XMP toolkit selects its platform behaviour from these defines.  The
  # native build has always been compiled without one, so it keeps that
  # behaviour; the WebAssembly build needs the web environment flag.
  set(HIRACO_CORE_XMP_DEFS ${XMP_OS_FLAG})
else()
  set(HIRACO_CORE_XMP_DEFS "")
endif()

add_library(hiraco-core STATIC
  src/dng_sdk_bridge.cpp
  src/dng_writer_bridge.cpp
  src/hiraco_core.cpp
  src/vendor_makernote.cpp
  src/stack_guidance.cpp
)

target_include_directories(hiraco-core
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  PRIVATE
    ${LIBRAW_INCLUDE_DIRS}
    ${FFTW3_INCLUDE_DIRS}
    ${HIRACO_DNG_SDK_ROOT}/dng_sdk/source
    ${HIRACO_DNG_SDK_ROOT}/libjxl/libjxl/lib/include
    ${HIRACO_DNG_SDK_ROOT}/libjpeg
    ${HIRACO_DNG_SDK_ROOT}/xmp/toolkit/public/include
)

target_link_libraries(hiraco-core
  PUBLIC
    ${LIBRAW_LIBRARIES}
    ${FFTW3_LIBRARIES}
    ${HIRACO_HALIDE_LIBRARIES}
    hiraco-dng-sdk
    hiraco-xmp-core
    ZLIB::ZLIB
)

target_compile_definitions(hiraco-core PRIVATE

  HIRACO_DNG_SDK_ROOT="${HIRACO_DNG_SDK_ROOT}"
  ${DNG_OS_FLAG}
  ${HIRACO_CORE_XMP_DEFS}
  qDNGUseXMP=1
  qDNGXMPFiles=0
  qDNGUseLibJPEG=0
)

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
  target_compile_definitions(hiraco-core PRIVATE XMP_64=1)
else()
  target_compile_definitions(hiraco-core PRIVATE XMP_64=0)
endif()

if(OpenMP_CXX_FOUND)
  target_link_libraries(hiraco-core PUBLIC OpenMP::OpenMP_CXX)
endif()

if(WIN32)
  target_compile_definitions(hiraco-core PRIVATE _USE_MATH_DEFINES)
endif()

if(APPLE)
  target_link_libraries(hiraco-core PRIVATE
    ${COREFOUNDATION_LIBRARY}
    ${CORESERVICES_LIBRARY}
  )
endif()

# The bundled LibRaw is produced by an ExternalProject, so the core has to
# wait for it before it is archived.
if(TARGET hiraco-libraw-ready)
  add_dependencies(hiraco-core hiraco-libraw-ready)
endif()

# ---------------------------------------------------------------------------
# The command line front end.  The WebAssembly build produces the same
# executable, linked for Node.
# ---------------------------------------------------------------------------
if(HIRACO_BUILD_CLI)
  add_executable(hiraco-cli
    src/main.cpp
  )
  target_link_libraries(hiraco-cli PRIVATE hiraco-core)
  if(HIRACO_BUILD_WASM)
    target_link_options(hiraco-cli PRIVATE
      -sENVIRONMENT=node
      -sNODERAWFS=1
      -sWASM=1
      -sEXIT_RUNTIME=1
    )
  endif()
endif()

# ---------------------------------------------------------------------------
# Desktop-only front ends.  The WebAssembly build has no GUI and no native
# test binary.
# ---------------------------------------------------------------------------
if(NOT HIRACO_BUILD_WASM)
  if(HIRACO_BUILD_GUI)
    find_package(wxWidgets REQUIRED COMPONENTS core base adv)
    include(${wxWidgets_USE_FILE})

    add_executable(hiraco-gui
      src/hiraco_gui.cpp
    )
    target_link_libraries(hiraco-gui PRIVATE hiraco-core ${wxWidgets_LIBRARIES})
  endif()

  if(HIRACO_BUILD_TESTS)
    enable_testing()
    add_executable(hiraco-core-tests
      tests/hiraco_core_tests.cpp
    )
    target_link_libraries(hiraco-core-tests PRIVATE hiraco-core)
    add_test(NAME hiraco-core-tests COMMAND hiraco-core-tests)
  endif()
endif()

# ---------------------------------------------------------------------------
# The Halide pipeline check.
#
# It is built for both toolchains so the numbers the two builds produce can be
# compared directly.  The WebAssembly build is a Node program.
# ---------------------------------------------------------------------------
if(HIRACO_BUILD_TESTS OR HIRACO_BUILD_WASM)
  add_executable(hiraco-halide-check
    tests/halide_pipeline_check.cpp
  )
  target_link_libraries(hiraco-halide-check PRIVATE
    hiraco_atrous_wavelet
    hiraco_guided_filter
  )
  if(HIRACO_HALIDE_INCLUDE_DIR)
    target_include_directories(hiraco-halide-check PRIVATE
      "${HIRACO_HALIDE_INCLUDE_DIR}"
    )
  endif()
  if(HIRACO_BUILD_WASM)
    target_link_options(hiraco-halide-check PRIVATE
      -sENVIRONMENT=node
      -sNODERAWFS=1
      -sWASM=1
      -sEXIT_RUNTIME=1
    )
  endif()
endif()

# ---------------------------------------------------------------------------
# The LibRaw decode check.
#
# The WebAssembly build compiles LibRaw from source, so the pixels it derives
# from a RAW file are produced by a different build of the same library.  This
# program exercises the decode so the two can be compared directly.
# ---------------------------------------------------------------------------
if(HIRACO_BUILD_TESTS OR HIRACO_BUILD_WASM)
  add_executable(hiraco-libraw-check
    tests/libraw_decode_check.cpp
  )
  target_link_libraries(hiraco-libraw-check PRIVATE ${LIBRAW_LIBRARIES})
  target_include_directories(hiraco-libraw-check PRIVATE ${LIBRAW_INCLUDE_DIRS})
  if(TARGET hiraco-libraw-ready)
    add_dependencies(hiraco-libraw-check hiraco-libraw-ready)
  endif()
  if(HIRACO_BUILD_WASM)
    target_link_options(hiraco-libraw-check PRIVATE
      -sENVIRONMENT=node
      -sNODERAWFS=1
      -sWASM=1
      -sEXIT_RUNTIME=1
    )
  endif()
endif()

# ---------------------------------------------------------------------------
# The capability check.
#
# The DNG capability summaries are what the browser bridge reports, so they
# have to describe the running build rather than the source.  The check is
# built for both toolchains because the two report the capability separately.
# ---------------------------------------------------------------------------
add_executable(hiraco-capability-check
  tests/capability_check.cpp
)
target_link_libraries(hiraco-capability-check PRIVATE hiraco-core)
if(HIRACO_BUILD_WASM)
  target_link_options(hiraco-capability-check PRIVATE
    -sENVIRONMENT=node
    -sNODERAWFS=1
    -sWASM=1
    -sEXIT_RUNTIME=1
  )
endif()

if(HIRACO_BUILD_WASM)
  add_executable(hiraco-wasm
    src/hiraco_wasm_bridge.cpp
  )
  target_link_libraries(hiraco-wasm PRIVATE hiraco-core)
  target_link_options(hiraco-wasm PRIVATE
    # The module is loaded in a browser, but the same build is exercised
    # under Node by tests/wasm_convert_harness.js.
    -sENVIRONMENT=web,node
    -sWASM=1
    -sMODULARIZE=1
    -sEXPORT_NAME=hiracoWasm
    -sEXPORTED_FUNCTIONS=["_hiraco_wasm_is_dng_sdk_enabled","_hiraco_wasm_get_dng_status","_hiraco_wasm_get_writer_summary","_hiraco_wasm_convert","_hiraco_wasm_free_buffer","_hiraco_wasm_get_last_error","_malloc","_free"]
    -sEXPORTED_RUNTIME_METHODS=["ccall","cwrap","HEAPU8","HEAPU32","lengthBytesUTF8","stringToUTF8","stringToNewUTF8","UTF8ToString"]
  )
endif()
