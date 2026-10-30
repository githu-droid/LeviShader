cmake_minimum_required(VERSION 3.22)
project(levishaders LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Path to a checkout of https://github.com/LiteLDev/preloader-android
set(PRELOADER_ANDROID_ROOT "${CMAKE_CURRENT_LIST_DIR}/preloader-android" CACHE PATH "preloader-android checkout")
if(NOT EXISTS "${PRELOADER_ANDROID_ROOT}/include/pl/ModMenu.hpp")
  message(FATAL_ERROR "Set -DPRELOADER_ANDROID_ROOT=<preloader-android checkout>")
endif()
if(NOT TARGET preloader)
  add_subdirectory("${PRELOADER_ANDROID_ROOT}" "${CMAKE_BINARY_DIR}/_preloader")
endif()

add_library(LeviShaders SHARED
  src/main.cpp
  src/PostFx.cpp
  src/ShaderPack.cpp)

target_include_directories(LeviShaders PRIVATE "${PRELOADER_ANDROID_ROOT}/include" src)
target_compile_options(LeviShaders PRIVATE -O2 -fvisibility=hidden -fvisibility-inlines-hidden
                       -ffunction-sections -fdata-sections -Wall -Wextra)
target_link_options(LeviShaders PRIVATE -Wl,--gc-sections -Wl,-z,max-page-size=16384)
target_link_libraries(LeviShaders PRIVATE preloader android log EGL GLESv3 dl)

find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
  add_custom_command(TARGET LeviShaders POST_BUILD
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/scripts/package_levipack.py
            --library $<TARGET_FILE:LeviShaders> --output $<TARGET_FILE_DIR:LeviShaders>/LeviShaders.levipack
    COMMENT "Packaging LeviShaders.levipack")
endif()
