add_executable(alphaclip main.cpp util.cpp inspect.cpp verify.cpp encode.cpp)
target_link_libraries(alphaclip PRIVATE alphaclip_core)
# inspect / verify use the core's internal container analysis (core/src/analyze.h), not only the C ABI.
target_include_directories(alphaclip PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../core/src")
target_compile_features(alphaclip PRIVATE cxx_std_17)
if(MSVC)
  target_compile_options(alphaclip PRIVATE /W4)
else()
  target_compile_options(alphaclip PRIVATE -Wall -Wextra)
endif()
if(MINGW)
  # No stray runtime DLLs next to the exe.
  target_link_options(alphaclip PRIVATE -static)
endif()
if(WIN32)
  target_link_libraries(alphaclip PRIVATE shell32)  # CommandLineToArgvW: UTF-8 arguments
endif()
