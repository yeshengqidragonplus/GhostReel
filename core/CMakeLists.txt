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

add_library(alphaclip_core STATIC
  src/api.cpp
  src/clip.cpp
  src/demuxer.cpp
  src/vp9_decoder.cpp)
target_include_directories(alphaclip_core
  PUBLIC include
  PRIVATE src "${AC_VPX_ROOT}")
target_link_libraries(alphaclip_core PRIVATE ac_webm_parser PUBLIC "${AC_VPX_LIB}")
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
