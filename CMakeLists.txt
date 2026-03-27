cmake_minimum_required(VERSION 3.16)

project(hiraco LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(APPLE AND NOT DEFINED CMAKE_OSX_ARCHITECTURES)
  set(CMAKE_OSX_ARCHITECTURES "${CMAKE_HOST_SYSTEM_PROCESSOR}" CACHE STRING "Build architectures for macOS" FORCE)
endif()

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

find_path(FFTW3_INCLUDE_DIR
  NAMES fftw3.h
  HINTS
    /opt/homebrew/include
    /usr/local/include
)

find_library(FFTW3_LIBRARY
  NAMES fftw3
  HINTS
    /opt/homebrew/lib
    /usr/local/lib
)

if(NOT FFTW3_INCLUDE_DIR OR NOT FFTW3_LIBRARY)
  message(FATAL_ERROR "FFTW3 not found. Install fftw (e.g. brew install fftw) or adjust include/library hints.")
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
  MAC_ENV=1
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
target_include_directories(hiraco-xmp-core PRIVATE
  ${HIRACO_DNG_SDK_ROOT}/xmp/toolkit
  ${HIRACO_DNG_SDK_ROOT}/xmp/toolkit/public/include
  ${HIRACO_DNG_SDK_ROOT}/xmp/toolkit/XMPCore/third-party/expat/public/lib
  ${HIRACO_DNG_SDK_ROOT}/xmp/toolkit/XMPCore/resource/mac
)
target_link_libraries(hiraco-xmp-core PRIVATE
  ${COREFOUNDATION_LIBRARY}
  ${CORESERVICES_LIBRARY}
)

add_subdirectory(${HIRACO_DNG_SDK_ROOT}/libjxl/libjxl ${CMAKE_CURRENT_BINARY_DIR}/libjxl EXCLUDE_FROM_ALL)

file(GLOB HIRACO_DNG_SDK_SOURCES CONFIGURE_DEPENDS
  "${HIRACO_DNG_SDK_ROOT}/dng_sdk/source/*.cpp"
)
list(FILTER HIRACO_DNG_SDK_SOURCES EXCLUDE REGEX ".*/dng_validate\\.cpp$")
list(FILTER HIRACO_DNG_SDK_SOURCES EXCLUDE REGEX ".*/dng_update_meta\\.cpp$")

add_library(hiraco-dng-sdk STATIC
  ${HIRACO_DNG_SDK_SOURCES}
)
target_compile_definitions(hiraco-dng-sdk PRIVATE
  qMacOS=1
  qDNGUseXMP=1
  qDNGXMPFiles=0
  qDNGUseLibJPEG=0
)
target_include_directories(hiraco-dng-sdk PRIVATE
  ${HIRACO_DNG_SDK_ROOT}/dng_sdk/source
  ${HIRACO_DNG_SDK_ROOT}/libjxl/libjxl/lib/include
  ${HIRACO_DNG_SDK_ROOT}/libjpeg
  ${HIRACO_DNG_SDK_ROOT}/xmp/toolkit/public/include
)
target_link_libraries(hiraco-dng-sdk PRIVATE
  jxl
  jxl_threads
  hiraco-xmp-core
  ${COREFOUNDATION_LIBRARY}
  ${CORESERVICES_LIBRARY}
  ${ZLIB_LIBRARY}
)

add_executable(hiraco
  src/dng_sdk_bridge.cpp
  src/dng_writer_bridge.cpp
  src/main.cpp
  src/vendor_makernote.cpp
  src/stack_guidance.cpp
)

target_include_directories(hiraco PRIVATE ${LIBRAW_INCLUDE_DIR} ${FFTW3_INCLUDE_DIR})
target_link_libraries(hiraco PRIVATE ${LIBRAW_LIBRARY} ${FFTW3_LIBRARY})
target_compile_definitions(hiraco PRIVATE
  HIRACO_ENABLE_DNG_SDK=1
  HIRACO_DNG_SDK_ROOT="${HIRACO_DNG_SDK_ROOT}"
  qMacOS=1
  qDNGUseXMP=1
  qDNGXMPFiles=0
  qDNGUseLibJPEG=0
)
target_include_directories(hiraco PRIVATE
  ${HIRACO_DNG_SDK_ROOT}/dng_sdk/source
  ${HIRACO_DNG_SDK_ROOT}/libjxl/libjxl/lib/include
  ${HIRACO_DNG_SDK_ROOT}/libjpeg
  ${HIRACO_DNG_SDK_ROOT}/xmp/toolkit/public/include
)
target_link_libraries(hiraco PRIVATE
  hiraco-dng-sdk
  hiraco-xmp-core
  ${COREFOUNDATION_LIBRARY}
  ${CORESERVICES_LIBRARY}
  ${ZLIB_LIBRARY}
)