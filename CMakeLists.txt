cmake_minimum_required(VERSION 3.28)

project(hiraco LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(HIRACO_BUILD_CLI "Build the hiraco-cli binary" ON)
option(HIRACO_BUILD_GUI "Build the hiraco-gui binary" ON)
option(HIRACO_BUILD_TESTS "Build hiraco core tests" ON)

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

# Find ZLIB first
find_package(ZLIB REQUIRED)

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
find_package(Halide REQUIRED)

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

if(APPLE)
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
target_include_directories(hiraco-xmp-core PRIVATE
  ${HIRACO_DNG_SDK_ROOT}/xmp/toolkit
  ${HIRACO_DNG_SDK_ROOT}/xmp/toolkit/public/include
  ${HIRACO_DNG_SDK_ROOT}/xmp/toolkit/XMPCore/third-party/expat/public/lib
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

# --- Halide AOT generators ---
add_halide_generator(hiraco_wavelet_gen
  SOURCES src/halide/wavelet_generator.cpp)
add_halide_generator(hiraco_guided_filter_gen
  SOURCES src/halide/guided_filter_generator.cpp)

add_halide_library(hiraco_atrous_wavelet FROM hiraco_wavelet_gen
  GENERATOR atrous_wavelet)
add_halide_library(hiraco_guided_filter FROM hiraco_guided_filter_gen
  GENERATOR guided_filter)

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
  ZLIB::ZLIB
)

if(APPLE)
  target_link_libraries(hiraco-dng-sdk PRIVATE
    ${COREFOUNDATION_LIBRARY}
    ${CORESERVICES_LIBRARY}
  )
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
