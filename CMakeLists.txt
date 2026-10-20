cmake_minimum_required(VERSION 3.28)

project(hiraco LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(HIRACO_BUILD_CLI "Build the hiraco-cli binary" ON)
option(HIRACO_BUILD_GUI "Build the hiraco-gui binary" ON)
option(HIRACO_BUILD_TESTS "Build hiraco core tests" ON)
option(HIRACO_BUILD_WASM "Build a browser/WebAssembly target without the wxWidgets GUI" OFF)
set(HIRACO_BOOST_INCLUDE_DIR "" CACHE PATH "Override the path to Boost headers for the Adobe XMP toolkit")

if(HIRACO_BUILD_WASM)
  if(NOT EMSCRIPTEN)
    message(FATAL_ERROR
      "HIRACO_BUILD_WASM requires an Emscripten toolchain. Configure with emcmake cmake -DHIRACO_BUILD_WASM=ON ...")
  endif()
  set(HIRACO_BUILD_CLI OFF CACHE BOOL "WebAssembly builds do not include the desktop CLI target" FORCE)
  set(HIRACO_BUILD_GUI OFF CACHE BOOL "WebAssembly builds are non-visual" FORCE)
  set(HIRACO_BUILD_TESTS OFF CACHE BOOL "WebAssembly builds skip desktop test binaries" FORCE)
  set(HIRACO_EMSCRIPTEN_PORT_FLAGS -sUSE_ZLIB=1 -sUSE_LIBPNG=1)
  add_compile_options(${HIRACO_EMSCRIPTEN_PORT_FLAGS})
  add_link_options(${HIRACO_EMSCRIPTEN_PORT_FLAGS})
  set(ZLIB_FOUND TRUE)
  set(ZLIB_INCLUDE_DIRS "")
  set(ZLIB_LIBRARIES "")
  set(PNG_FOUND TRUE)
  set(PNG_INCLUDE_DIRS "")
  set(PNG_LIBRARIES "")
  message(STATUS "Configuring Hiraco as a WebAssembly target; desktop GUI and CLI targets are intentionally disabled.")
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
# The desktop core uses these AOT pipelines, but the browser-only build is
# intentionally limited to the DNG SDK conversion layer and does not create the
# native image-processing core.
if(NOT HIRACO_BUILD_WASM)
  find_package(Halide REQUIRED)
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
endif()

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

if(HIRACO_BUILD_WASM)
  add_library(hiraco-core STATIC
    src/dng_sdk_bridge.cpp
    src/dng_writer_bridge.cpp
    src/vendor_makernote.cpp
    src/stack_guidance.cpp
  )

  target_include_directories(hiraco-core
    PUBLIC
      ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_link_libraries(hiraco-core
    PUBLIC
      hiraco-dng-sdk
      hiraco-xmp-core
      ZLIB::ZLIB
  )
  target_compile_definitions(hiraco-core PRIVATE
    HIRACO_ENABLE_DNG_SDK=1
    HIRACO_DNG_SDK_ROOT="${HIRACO_DNG_SDK_ROOT}"
    ${DNG_OS_FLAG}
    ${XMP_OS_FLAG}
    qDNGUseXMP=1
    qDNGXMPFiles=0
    qDNGUseLibJPEG=0
  )
  if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    target_compile_definitions(hiraco-core PRIVATE XMP_64=1)
  else()
    target_compile_definitions(hiraco-core PRIVATE XMP_64=0)
  endif()
  target_include_directories(hiraco-core PRIVATE
    ${HIRACO_DNG_SDK_ROOT}/dng_sdk/source
    ${HIRACO_DNG_SDK_ROOT}/libjxl/libjxl/lib/include
    ${HIRACO_DNG_SDK_ROOT}/libjpeg
    ${HIRACO_DNG_SDK_ROOT}/xmp/toolkit/public/include
  )
else()
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
  )
  target_link_libraries(hiraco-core
    PUBLIC
      ${LIBRAW_LIBRARIES}
      ${FFTW3_LIBRARIES}
      hiraco_atrous_wavelet
      hiraco_guided_filter
      hiraco-dng-sdk
      hiraco-xmp-core
      ZLIB::ZLIB
  )
  if(OpenMP_CXX_FOUND)
    target_link_libraries(hiraco-core PUBLIC OpenMP::OpenMP_CXX)
  endif()
  target_compile_definitions(hiraco-core PRIVATE
    HIRACO_ENABLE_DNG_SDK=1
    HIRACO_DNG_SDK_ROOT="${HIRACO_DNG_SDK_ROOT}"
    ${DNG_OS_FLAG}
    qDNGUseXMP=1
    qDNGXMPFiles=0
    qDNGUseLibJPEG=0
  )

  target_include_directories(hiraco-core PRIVATE
    ${HIRACO_DNG_SDK_ROOT}/dng_sdk/source
    ${HIRACO_DNG_SDK_ROOT}/libjxl/libjxl/lib/include
    ${HIRACO_DNG_SDK_ROOT}/libjpeg
    ${HIRACO_DNG_SDK_ROOT}/xmp/toolkit/public/include
  )

  if(WIN32)
    target_compile_definitions(hiraco-core PRIVATE _USE_MATH_DEFINES)
  endif()

  if(APPLE)
    target_link_libraries(hiraco-core PRIVATE
      ${COREFOUNDATION_LIBRARY}
      ${CORESERVICES_LIBRARY}
    )
  endif()

  if(HIRACO_BUILD_CLI)
    add_executable(hiraco-cli
      src/main.cpp
    )
    target_link_libraries(hiraco-cli PRIVATE hiraco-core)
  endif()

  if(HIRACO_BUILD_GUI AND NOT HIRACO_BUILD_WASM)
    find_package(wxWidgets REQUIRED COMPONENTS core base adv)
    include(${wxWidgets_USE_FILE})

    add_executable(hiraco-gui
      src/hiraco_gui.cpp
    )
    target_link_libraries(hiraco-gui PRIVATE hiraco-core ${wxWidgets_LIBRARIES})
  endif()

  if(HIRACO_BUILD_TESTS AND NOT HIRACO_BUILD_WASM)
    enable_testing()
    add_executable(hiraco-core-tests
      tests/hiraco_core_tests.cpp
    )
    target_link_libraries(hiraco-core-tests PRIVATE hiraco-core)
    add_test(NAME hiraco-core-tests COMMAND hiraco-core-tests)
  endif()
endif()

if(HIRACO_BUILD_WASM)
  add_executable(hiraco-wasm
    src/hiraco_wasm_bridge.cpp
  )
  target_link_libraries(hiraco-wasm PRIVATE hiraco-core)
  target_include_directories(hiraco-wasm PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_link_options(hiraco-wasm PRIVATE
    -sENVIRONMENT=web
    -sWASM=1
    -sMODULARIZE=1
    -sEXPORT_NAME=hiracoWasm
    -sEXPORTED_FUNCTIONS=[\"_hiraco_wasm_is_dng_sdk_enabled\",\"_hiraco_wasm_get_dng_status\",\"_hiraco_wasm_get_writer_summary\"]
    -sEXPORTED_RUNTIME_METHODS=[\"ccall\",\"cwrap\"]
  )
endif()
