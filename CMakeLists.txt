cmake_minimum_required(VERSION 3.18)
project(EarRinging CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

file(GLOB AML_SOURCES ${CMAKE_SOURCE_DIR}/include/mod/*.cpp)

# --- Ear Ringing + death animations ---
add_library(EarRinging SHARED main.cpp ${AML_SOURCES})
target_include_directories(EarRinging PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party)
target_compile_options(EarRinging PRIVATE -O2 -Wno-unused-function)
target_link_libraries(EarRinging log dl m OpenSLES)

# --- My HUD (custom HUD layout) ---
add_library(MyHUD SHARED main_hud.cpp ${AML_SOURCES})
target_include_directories(MyHUD PRIVATE
    ${CMAKE_SOURCE_DIR}/include)
target_compile_options(MyHUD PRIVATE -O2 -Wno-unused-function)
target_link_libraries(MyHUD log dl m)

# --- SanVive (extra weapon animation groups) ---
add_library(SanVive SHARED main_sanvive.cpp ${AML_SOURCES})
target_include_directories(SanVive PRIVATE
    ${CMAKE_SOURCE_DIR}/include)
target_compile_options(SanVive PRIVATE -O2 -Wno-unused-function)
target_link_libraries(SanVive log dl m)
