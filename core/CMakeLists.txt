cmake_minimum_required(VERSION 3.22)
project(alphaclip_core LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

set(AC_WEBM_ROOT "${CMAKE_CURRENT_LIST_DIR}/third_party/libwebm" CACHE PATH "libwebm source dir")
set(AC_VPX_ROOT "${CMAKE_CURRENT_LIST_DIR}/third_party/libvpx" CACHE PATH "libvpx source dir (headers only)")
# libvpx has no CMake build: it is built separately (core/scripts/build_libvpx_*.sh).
set(AC_VPX_LIB "" CACHE FILEPATH "path to the built libvpx static library (libvpx.a)")
if(NOT AC_VPX_LIB)
  message(FATAL_ERROR "set -DAC_VPX_LIB=<path to libvpx.a> (see core/scripts/)")
endif()
option(AC_BUILD_CLI "Build the alphaclip command-line tool" ON)

# libwebm's webm_parser is off by default upstream and only built as an OBJECT library,
# so its sources are compiled directly. Third-party code: no warning flags of ours.
file(GLOB AC_WEBM_PARSER_SOURCES CONFIGURE_DEPENDS "${AC_WEBM_ROOT}/webm_parser/src/*.cc")
add_library(ac_webm_parser STATIC ${AC_WEBM_PARSER_SOURCES})
target_include_directories(ac_webm_parser
  PUBLIC "${AC_WEBM_ROOT}/webm_parser/include"
  PRIVATE "${AC_WEBM_ROOT}/webm_parser")

find_package(Threads REQUIRED)
add_library(alphaclip_core STATIC
  src/api.cpp
  src/clip.cpp
  src/demuxer.cpp
  src/player.cpp
  src/vp9_decoder.cpp)
target_include_directories(alphaclip_core
  PUBLIC include
  PRIVATE src "${AC_VPX_ROOT}")
target_link_libraries(alphaclip_core
  PRIVATE ac_webm_parser
  PUBLIC "${AC_VPX_LIB}" Threads::Threads)
if(MSVC)
  target_compile_options(alphaclip_core PRIVATE /W4)
else()
  target_compile_options(alphaclip_core PRIVATE -Wall -Wextra)
endif()
if(MINGW)
  # libvpx's threaded decode needs pthreads (static winpthreads on MinGW).
  target_link_libraries(alphaclip_core PUBLIC pthread)
endif()

if(AC_BUILD_CLI)
  add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../tools/cli" "${CMAKE_BINARY_DIR}/tools_cli")
endif()

# The shared library the engine bindings load (alphaclip.dll). It exports only the ac_* functions
# marked AC_API in include/alphaclip.h: everything else (libvpx, libwebm, libc++) stays hidden.
option(AC_BUILD_SHARED "Build the shared library (alphaclip.dll)" ON)
if(AC_BUILD_SHARED)
  add_library(alphaclip_shared SHARED
    src/api.cpp
    src/clip.cpp
    src/demuxer.cpp
    src/player.cpp
    src/vp9_decoder.cpp)
  target_compile_definitions(alphaclip_shared PRIVATE AC_BUILDING_SHARED)
  target_include_directories(alphaclip_shared
    PUBLIC include
    PRIVATE src "${AC_VPX_ROOT}")
  target_link_libraries(alphaclip_shared PRIVATE ac_webm_parser "${AC_VPX_LIB}" Threads::Threads)
  if(MSVC)
    target_compile_options(alphaclip_shared PRIVATE /W4)
  else()
    target_compile_options(alphaclip_shared PRIVATE -Wall -Wextra)
  endif()
  # alphaclip.dll on Windows, libalphaclip.so on Android / Linux (the CLI target already owns the name "alphaclip").
  set_target_properties(alphaclip_shared PROPERTIES OUTPUT_NAME "alphaclip")
  if(WIN32)
    set_target_properties(alphaclip_shared PROPERTIES PREFIX "")
  endif()
  if(MINGW)
    # Self-contained DLL: static libc++ / libgcc / winpthreads, so it needs no runtime DLLs next to it.
    target_link_libraries(alphaclip_shared PRIVATE pthread)
    target_link_options(alphaclip_shared PRIVATE -static)
  endif()
  if(ANDROID)
    # libalphaclip.so: libvpx.a and libwebm come in as static archives and would otherwise export every symbol
    # they define (vpx_*, webm::*), clashing with other copies in the same process; --exclude-libs hides them,
    # so the only exports are the ac_* functions. 16 KB LOAD-segment alignment is Google Play's page-size
    # requirement; NDK r30 does it by default, say it explicitly so an older NDK cannot silently regress.
    target_link_options(alphaclip_shared PRIVATE
      -Wl,--exclude-libs,ALL
      -Wl,-z,max-page-size=16384
      -Wl,-z,common-page-size=16384
      -Wl,--build-id=sha1)
  endif()
endif()
