cmake_minimum_required(VERSION 3.16)
project(LanTalk LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT WIN32)
  message(FATAL_ERROR "LanTalk supports Windows x64 only.")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "LanTalk is 64-bit only. Please use a 64-bit toolchain.")
endif()

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets Network)

add_executable(lantalk WIN32
  main.cpp
  chat_window.cpp
  chat_window.h
  app_icon.rc
  resources.qrc
)
target_link_libraries(lantalk PRIVATE Qt6::Widgets Qt6::Network ws2_32)
target_compile_definitions(lantalk PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)

# Produce a runnable app directory after local builds by deploying Qt + MSVC runtime
# beside lantalk.exe. This avoids "msvcp140_1.dll missing" when running directly.
if(WIN32)
  get_target_property(_qt_qmake_exe Qt6::qmake IMPORTED_LOCATION)
  if(_qt_qmake_exe)
    get_filename_component(_qt_bin_dir "${_qt_qmake_exe}" DIRECTORY)
    find_program(WINDEPLOYQT_EXECUTABLE
      NAMES windeployqt windeployqt6
      HINTS "${_qt_bin_dir}"
      NO_DEFAULT_PATH
    )
    if(NOT WINDEPLOYQT_EXECUTABLE)
      find_program(WINDEPLOYQT_EXECUTABLE NAMES windeployqt windeployqt6)
    endif()
  endif()

  if(WINDEPLOYQT_EXECUTABLE)
    add_custom_command(TARGET lantalk POST_BUILD
      COMMAND "${WINDEPLOYQT_EXECUTABLE}"
              --release
              --no-translations
              --compiler-runtime
              --dir "$<TARGET_FILE_DIR:lantalk>"
              "$<TARGET_FILE:lantalk>"
      COMMAND "${CMAKE_COMMAND}"
              -DDEPLOY_DIR="$<TARGET_FILE_DIR:lantalk>"
              -P "${CMAKE_SOURCE_DIR}/cmake/finalize_windows_runtime.cmake"
      COMMENT "Deploying Qt/MSVC runtime beside lantalk.exe and stripping installer packages"
      VERBATIM
    )
  else()
    message(WARNING "windeployqt not found; built executable may miss runtime dependencies.")
  endif()
endif()

if(MSVC)
  target_compile_options(lantalk PRIVATE /W4 /permissive-)
  target_compile_options(lantalk PRIVATE $<$<CONFIG:Release>:/O2 /Oi /GL>)
  target_link_options(lantalk PRIVATE $<$<CONFIG:Release>:/LTCG>)
endif()
