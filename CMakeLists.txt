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

find_package(Qt6 REQUIRED COMPONENTS Widgets)

add_executable(lantalk WIN32 main.cpp)
target_link_libraries(lantalk PRIVATE Qt6::Widgets ws2_32)
target_compile_definitions(lantalk PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)

if(MSVC)
  target_compile_options(lantalk PRIVATE /W4 /permissive-)
  target_compile_options(lantalk PRIVATE $<$<CONFIG:Release>:/O2 /Oi /GL>)
  target_link_options(lantalk PRIVATE $<$<CONFIG:Release>:/LTCG>)
endif()
