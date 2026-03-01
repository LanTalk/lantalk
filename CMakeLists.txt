cmake_minimum_required(VERSION 3.24)
cmake_policy(SET CMP0091 NEW)
project(lantalk LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

if(WIN32 OR APPLE)
  set(LANTALK_ENABLE_DESKTOP_DEFAULT ON)
else()
  set(LANTALK_ENABLE_DESKTOP_DEFAULT OFF)
endif()
option(LANTALK_ENABLE_DESKTOP "Build desktop shell with embedded WebView" ${LANTALK_ENABLE_DESKTOP_DEFAULT})

add_library(lantalk_core
  src/app_core.cpp
)
target_include_directories(lantalk_core PUBLIC src)
if(WIN32)
  target_link_libraries(lantalk_core PUBLIC ws2_32)
endif()

if(LANTALK_ENABLE_DESKTOP)
  include(FetchContent)
  FetchContent_Declare(
    webview
    GIT_REPOSITORY https://github.com/webview/webview.git
    GIT_TAG 0.12.0
  )
  FetchContent_MakeAvailable(webview)

  add_executable(lantalk src/main.cpp)
  target_link_libraries(lantalk PRIVATE lantalk_core webview::core)

  if(WIN32)
    target_compile_definitions(lantalk PRIVATE UNICODE _UNICODE)
    set_target_properties(lantalk PROPERTIES WIN32_EXECUTABLE YES)
  endif()
else()
  add_executable(lantalk src/headless_main.cpp)
  target_link_libraries(lantalk PRIVATE lantalk_core)
endif()
