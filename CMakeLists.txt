cmake_minimum_required(VERSION 3.24)
cmake_policy(SET CMP0091 NEW)

project(lantalk LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

if(WIN32)
  set(LANTALK_DESKTOP_DEFAULT ON)
else()
  set(LANTALK_DESKTOP_DEFAULT OFF)
endif()
option(LANTALK_DESKTOP "Build desktop shell with embedded WebView" ${LANTALK_DESKTOP_DEFAULT})

add_library(lantalk_core
  core.cpp
  monocypher.c
)

target_include_directories(lantalk_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_compile_definitions(lantalk_core PRIVATE _CRT_SECURE_NO_WARNINGS)

if(WIN32)
  target_link_libraries(lantalk_core PUBLIC ws2_32 bcrypt)
else()
  target_link_libraries(lantalk_core PUBLIC pthread)
endif()

if(LANTALK_DESKTOP)
  include(FetchContent)
  FetchContent_Declare(
    webview
    GIT_REPOSITORY https://github.com/webview/webview.git
    GIT_TAG 0.12.0
  )
  FetchContent_MakeAvailable(webview)

  if(WIN32)
    add_executable(lantalk WIN32 main.cpp)
    target_compile_definitions(lantalk PRIVATE UNICODE _UNICODE)
    target_link_libraries(lantalk PRIVATE ole32 shell32 comdlg32)
  else()
    add_executable(lantalk main.cpp)
  endif()

  target_link_libraries(lantalk PRIVATE lantalk_core webview::core)
else()
  add_executable(lantalk headless_main.cpp)
  target_link_libraries(lantalk PRIVATE lantalk_core)
endif()
