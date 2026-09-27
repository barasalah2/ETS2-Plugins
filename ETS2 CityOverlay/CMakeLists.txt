cmake_minimum_required(VERSION 3.24)
project(ets2_city_overlay LANGUAGES C CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "ETS2 is 64-bit only: configure with -A x64")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT: the DLL lives inside the game process and must not depend on a VC++ redist.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(FetchContent)
FetchContent_Declare(imgui
  URL https://github.com/ocornut/imgui/archive/refs/tags/v1.92.9b.zip)
# SOURCE_SUBDIR points nowhere so MinHook's own CMakeLists is not used; we compile its sources directly.
FetchContent_Declare(minhook
  URL https://github.com/TsudaKageyu/minhook/archive/refs/tags/v1.3.4.zip
  SOURCE_SUBDIR _unused)
FetchContent_MakeAvailable(imgui minhook)

add_library(ets2_city_overlay SHARED
  src/plugin.cpp
  src/alerts.cpp
  src/cities.cpp
  src/config.cpp
  src/overlay.cpp
  src/log.cpp
  src/csv.cpp
  src/pois.cpp
  src/route.cpp
  src/speech.cpp
  src/codriver.cpp
  src/gemini.cpp
  src/capture.cpp
  src/mic.cpp
  src/rules.cpp
  src/announce.cpp
  src/exports.def
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
  ${minhook_SOURCE_DIR}/src/buffer.c
  ${minhook_SOURCE_DIR}/src/hook.c
  ${minhook_SOURCE_DIR}/src/trampoline.c
  ${minhook_SOURCE_DIR}/src/hde/hde64.c
)

target_include_directories(ets2_city_overlay PRIVATE
  src
  third_party/scs_sdk/include
  third_party/scs_sdk/include/common
  third_party/scs_sdk/include/eurotrucks2
  ${imgui_SOURCE_DIR}
  ${imgui_SOURCE_DIR}/backends
  ${minhook_SOURCE_DIR}/include
)

target_compile_definitions(ets2_city_overlay PRIVATE
  WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS
  IMGUI_DISABLE_DEMO_WINDOWS IMGUI_DISABLE_DEBUG_TOOLS
  IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS)
target_compile_options(ets2_city_overlay PRIVATE /utf-8 /W3)
target_link_libraries(ets2_city_overlay PRIVATE d3d11 dxgi d3dcompiler sapi ole32 oleaut32 winmm winhttp windowscodecs)

# Put the data folder next to the built DLL so the build output can be copied into plugins/ as-is.
add_custom_command(TARGET ets2_city_overlay POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:ets2_city_overlay>/ets2_city_overlay"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_SOURCE_DIR}/data/cities.csv"
          "${CMAKE_SOURCE_DIR}/data/city_areas.csv"
          "${CMAKE_SOURCE_DIR}/data/pois.csv"
          "${CMAKE_SOURCE_DIR}/data/countries.csv"
          "${CMAKE_SOURCE_DIR}/data/route_graph.bin"
          "${CMAKE_SOURCE_DIR}/data/maneuvers.bin"
          "${CMAKE_SOURCE_DIR}/data/ets2_city_overlay.ini"
          "$<TARGET_FILE_DIR:ets2_city_overlay>/ets2_city_overlay/")

# Fake game for testing the plugin without ETS2 (see tools/test_host.cpp).
option(CITY_OVERLAY_TEST_HOST "Build the test host" ON)
if(CITY_OVERLAY_TEST_HOST)
  add_executable(test_host tools/test_host.cpp)
  target_include_directories(test_host PRIVATE third_party/scs_sdk/include third_party/scs_sdk/include/common)
  target_compile_definitions(test_host PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
  target_link_libraries(test_host PRIVATE d3d11 dxgi gdi32 user32)
endif()
