# M0 spike: libwebm (webm_parser) + libvpx alpha probe. Throwaway.
#
# libvpx has no CMake build; it is built separately with its own configure/make
# into a build dir. Point VPX_ROOT at the libvpx *source* dir (for headers) and
# VPX_LIB at the resulting libvpx.a.
cmake_minimum_required(VERSION 3.16)
project(m0_alpha_probe CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(WEBM_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../third_party/libwebm" CACHE PATH "libwebm source dir")
set(VPX_ROOT  "${CMAKE_CURRENT_LIST_DIR}/../../third_party/libvpx"  CACHE PATH "libvpx source dir")
set(VPX_LIB   "" CACHE FILEPATH "path to built libvpx.a / vpx.lib")
if(NOT VPX_LIB)
  message(FATAL_ERROR "set -DVPX_LIB=<path to libvpx.a>")
endif()

# webm_parser is only an OBJECT library upstream and off by default, so compile
# its sources directly.
file(GLOB WEBM_PARSER_SOURCES CONFIGURE_DEPENDS "${WEBM_ROOT}/webm_parser/src/*.cc")

add_executable(alpha_probe alpha_probe.cpp ${WEBM_PARSER_SOURCES})
target_include_directories(alpha_probe PRIVATE
  "${WEBM_ROOT}/webm_parser/include"
  "${WEBM_ROOT}/webm_parser"
  "${VPX_ROOT}")
target_link_libraries(alpha_probe PRIVATE "${VPX_LIB}")
if(MINGW)
  # libvpx's multithreaded decode uses pthreads (winpthreads on MinGW).
  # -static: no stray runtime DLLs next to the exe.
  target_link_libraries(alpha_probe PRIVATE pthread)
  target_link_options(alpha_probe PRIVATE -static)
endif()
