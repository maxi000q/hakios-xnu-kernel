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
    cpu/arm7_cpu.cpp
    image/aes256.cpp
    image/img3.cpp
    image/kernel_lzss.cpp
    image/macho_arm.cpp
    image/kernel_loader.cpp
    image/dyld_cache.cpp
    gpu/frame_renderer.cpp
    gpu/egl_backend.cpp    # compiles to nothing off Android
    core/emulator.cpp)

target_include_directories(hakios_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(hakios_core PRIVATE -Wall -Wextra)

if(ANDROID)
    # the stage-5 display path: EGL + GLESv2, the floor on every Adreno 619 phone
    target_link_libraries(hakios_core PUBLIC EGL GLESv2 android)
endif()

# the JNI bridge is the app-facing surface of the core (stage 6)
if(ANDROID)
    add_library(hakios_jni STATIC jni/hakios_jni.cpp)
    target_link_libraries(hakios_jni PUBLIC hakios_core)
endif()
