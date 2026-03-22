cmake_minimum_required(VERSION 3.16)

project(hiraco_native LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(HIRACO_DNG_SDK_ROOT "${CMAKE_CURRENT_LIST_DIR}/../dng_sdk_1_7_1" CACHE PATH "Path to the Adobe DNG SDK bundle root")

if(NOT EXISTS "${HIRACO_DNG_SDK_ROOT}/dng_sdk/source")
  message(FATAL_ERROR "Adobe DNG SDK not found at ${HIRACO_DNG_SDK_ROOT}. Copy the SDK bundle there or pass -DHIRACO_DNG_SDK_ROOT=/path/to/dng_sdk_1_7_1.")
endif()

if(NOT EXISTS "${HIRACO_DNG_SDK_ROOT}/libjxl/libjxl/CMakeLists.txt")
  message(FATAL_ERROR "Bundled libjxl sources were not found under ${HIRACO_DNG_SDK_ROOT}. Ensure the full Adobe DNG SDK bundle is present.")
endif()

find_path(LIBRAW_INCLUDE_DIR
  NAMES libraw/libraw.h
  HINTS
    /opt/homebrew/include
    /usr/local/include
)

find_library(LIBRAW_LIBRARY
  NAMES raw_r libraw_r
  HINTS
    /opt/homebrew/lib
    /usr/local/lib
)

if(NOT LIBRAW_INCLUDE_DIR OR NOT LIBRAW_LIBRARY)
  message(FATAL_ERROR "LibRaw not found. Install libraw or adjust include/library hints.")
endif()

find_library(COREFOUNDATION_LIBRARY CoreFoundation)
find_library(CORESERVICES_LIBRARY CoreServices)
find_library(ZLIB_LIBRARY z)

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

add_subdirectory(${HIRACO_DNG_SDK_ROOT}/libjxl/libjxl ${CMAKE_CURRENT_BINARY_DIR}/libjxl EXCLUDE_FROM_ALL)

file(GLOB HIRACO_DNG_SDK_SOURCES CONFIGURE_DEPENDS
  "${HIRACO_DNG_SDK_ROOT}/dng_sdk/source/*.cpp"
)
list(FILTER HIRACO_DNG_SDK_SOURCES EXCLUDE REGEX ".*/dng_validate\\.cpp$")
list(FILTER HIRACO_DNG_SDK_SOURCES EXCLUDE REGEX ".*/dng_update_meta\\.cpp$")
list(FILTER HIRACO_DNG_SDK_SOURCES EXCLUDE REGEX ".*/dng_jxl\\.cpp$")
list(FILTER HIRACO_DNG_SDK_SOURCES EXCLUDE REGEX ".*/dng_xmp\\.cpp$")
list(FILTER HIRACO_DNG_SDK_SOURCES EXCLUDE REGEX ".*/dng_xmp_sdk\\.cpp$")

add_library(hiraco-dng-sdk STATIC
  ${HIRACO_DNG_SDK_SOURCES}
)
target_compile_definitions(hiraco-dng-sdk PRIVATE
  qMacOS=1
  qDNGUseXMP=0
  qDNGUseLibJPEG=0
)
target_include_directories(hiraco-dng-sdk PRIVATE
  ${HIRACO_DNG_SDK_ROOT}/dng_sdk/source
  ${HIRACO_DNG_SDK_ROOT}/libjxl/libjxl/lib/include
  ${HIRACO_DNG_SDK_ROOT}/libjpeg
)
target_link_libraries(hiraco-dng-sdk PRIVATE
  jxl
  jxl_threads
  ${COREFOUNDATION_LIBRARY}
  ${CORESERVICES_LIBRARY}
  ${ZLIB_LIBRARY}
)

add_executable(hiraco-native
  src/dng_sdk_bridge.cpp
  src/dng_jxl_stubs.cpp
  src/dng_writer_bridge.cpp
  src/main.cpp
)

target_include_directories(hiraco-native PRIVATE ${LIBRAW_INCLUDE_DIR})
target_link_libraries(hiraco-native PRIVATE ${LIBRAW_LIBRARY})
target_compile_definitions(hiraco-native PRIVATE
  HIRACO_ENABLE_DNG_SDK=1
  HIRACO_DNG_SDK_ROOT="${HIRACO_DNG_SDK_ROOT}"
  qMacOS=1
  qDNGUseXMP=0
  qDNGUseLibJPEG=0
)
target_include_directories(hiraco-native PRIVATE
  ${HIRACO_DNG_SDK_ROOT}/dng_sdk/source
  ${HIRACO_DNG_SDK_ROOT}/libjxl/libjxl/lib/include
  ${HIRACO_DNG_SDK_ROOT}/libjpeg
)
target_link_libraries(hiraco-native PRIVATE
  hiraco-dng-sdk
  ${COREFOUNDATION_LIBRARY}
  ${CORESERVICES_LIBRARY}
  ${ZLIB_LIBRARY}
)