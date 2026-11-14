# The preview page (tools/preview/www) and the master shader are embedded in the executable, so `alphaclip preview`
# needs nothing but the exe. One line per file: url|path below the repository root|content type.
get_filename_component(AC_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(AC_PREVIEW_ASSETS
  "/index.html|tools/preview/www/index.html|text/html"
  "/style.css|tools/preview/www/style.css|text/css"
  "/app.js|tools/preview/www/app.js|application/javascript"
  "/api.js|tools/preview/www/api.js|application/javascript"
  "/renderer.js|tools/preview/www/renderer.js|application/javascript"
  "/autotest.html|tools/preview/www/autotest.html|text/html"
  "/autotest.js|tools/preview/www/autotest.js|application/javascript"
  "/uitest.html|tools/preview/www/uitest.html|text/html"
  "/uitest.js|tools/preview/www/uitest.js|application/javascript"
  "/shaders/alphaclip_yuv_to_rgba.frag.glsl|shaders/alphaclip_yuv_to_rgba.frag.glsl|text/plain")
set(AC_PREVIEW_MANIFEST "${CMAKE_CURRENT_BINARY_DIR}/preview_assets.txt")
set(AC_PREVIEW_GENERATED "${CMAKE_CURRENT_BINARY_DIR}/preview_assets.cpp")
set(AC_PREVIEW_SOURCES "")
set(AC_PREVIEW_MANIFEST_TEXT "")
foreach(entry IN LISTS AC_PREVIEW_ASSETS)
  string(REPLACE "|" ";" parts "${entry}")
  list(GET parts 1 source)
  list(APPEND AC_PREVIEW_SOURCES "${AC_REPO_ROOT}/${source}")
  string(APPEND AC_PREVIEW_MANIFEST_TEXT "${entry}\n")
endforeach()
file(GENERATE OUTPUT "${AC_PREVIEW_MANIFEST}" CONTENT "${AC_PREVIEW_MANIFEST_TEXT}")
add_custom_command(
  OUTPUT "${AC_PREVIEW_GENERATED}"
  COMMAND "${CMAKE_COMMAND}" "-DROOT=${AC_REPO_ROOT}" "-DMANIFEST=${AC_PREVIEW_MANIFEST}" "-DOUT=${AC_PREVIEW_GENERATED}"
          -P "${CMAKE_CURRENT_LIST_DIR}/embed_assets.cmake"
  DEPENDS ${AC_PREVIEW_SOURCES} "${AC_PREVIEW_MANIFEST}" "${CMAKE_CURRENT_LIST_DIR}/embed_assets.cmake"
  COMMENT "Embedding the preview page"
  VERBATIM)

add_executable(alphaclip main.cpp util.cpp inspect.cpp verify.cpp encode.cpp preview.cpp http_server.cpp
  "${AC_PREVIEW_GENERATED}")
target_link_libraries(alphaclip PRIVATE alphaclip_core)
# inspect / verify use the core's internal container analysis (core/src/analyze.h), not only the C ABI.
target_include_directories(alphaclip PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../core/src" "${CMAKE_CURRENT_LIST_DIR}")
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
  # shell32: CommandLineToArgvW (UTF-8 arguments), ShellExecuteW (open the browser); ws2_32: the preview's sockets
  target_link_libraries(alphaclip PRIVATE shell32 ws2_32)
endif()
