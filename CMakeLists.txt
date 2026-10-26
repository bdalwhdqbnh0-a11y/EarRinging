cmake_minimum_required(VERSION 3.18)
project(EarRinging CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(EarRinging SHARED main.cpp)

target_include_directories(EarRinging PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party)

target_compile_options(EarRinging PRIVATE -O2 -fvisibility=hidden -Wno-unused-function)
target_link_libraries(EarRinging log dl m OpenSLES)
