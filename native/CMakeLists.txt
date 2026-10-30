# The hakios native core -- stage 2 onward. Built for Android by the NDK
# through app/build.gradle.kts (stage 7) and on the desktop by
# tests/run_desktop_tests.sh with the host g++. Sources are added per stage.

cmake_minimum_required(VERSION 3.22.1)
project(hakios-core LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Warnings that matter: this core runs a kernel, not a webpage.
add_library(hakios_core STATIC
    memory/storage_memory.cpp
    cpu/arm7_cpu.cpp)

target_include_directories(hakios_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(hakios_core PRIVATE -Wall -Wextra)
