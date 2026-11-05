cmake_minimum_required(VERSION 3.22)
project(ac_empty LANGUAGES CXX)

add_library(ac_empty SHARED ac_empty.cpp)
target_compile_features(ac_empty PRIVATE cxx_std_17)
set_target_properties(ac_empty PROPERTIES
    CXX_EXTENSIONS OFF
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)

if(MSVC)
    target_compile_options(ac_empty PRIVATE /W4)
else()
    target_compile_options(ac_empty PRIVATE -Wall -Wextra)
endif()
