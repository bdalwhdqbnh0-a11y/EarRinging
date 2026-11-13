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

# --- SleekMove (shoulder camera + shake) ---
add_library(SleekMove SHARED main_sleek.cpp ${AML_SOURCES})
target_include_directories(SleekMove PRIVATE
    ${CMAKE_SOURCE_DIR}/include)
target_compile_options(SleekMove PRIVATE -O2 -Wno-unused-function)
target_link_libraries(SleekMove log dl m)

# --- EarShot (weapon sounds, phase 1: AK-47) ---
add_library(EarShot SHARED main_earshot.cpp ${AML_SOURCES})
target_include_directories(EarShot PRIVATE
    ${CMAKE_SOURCE_DIR}/include)
target_compile_options(EarShot PRIVATE -O2 -Wno-unused-function)
target_link_libraries(EarShot log dl m)

# --- BulletTime (slow motion + adrenaline bar, phase 1) ---
add_library(BulletTime SHARED main_bullettime.cpp ${AML_SOURCES})
target_include_directories(BulletTime PRIVATE
    ${CMAKE_SOURCE_DIR}/include)
target_compile_options(BulletTime PRIVATE -O2 -Wno-unused-function)
target_link_libraries(BulletTime log dl m)
