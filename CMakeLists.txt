cmake_minimum_required(VERSION 4.0)
project(better-net VERSION 1.0.0 LANGUAGES CXX RC)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "better-net requires 64-bit Windows")
endif()

if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR NOT MSVC)
    message(FATAL_ERROR "Use clang-cl with the checked-in presets")
endif()

if(NOT CMAKE_CXX_COMPILER_VERSION VERSION_EQUAL "23.1.2")
    message(FATAL_ERROR "The validated compiler is clang-cl 23.1.2")
endif()

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX26_STANDARD_COMPILE_OPTION "/clang:-std=c++26")
set(CMAKE_CXX26_EXTENSION_COMPILE_OPTION "/clang:-std=c++26")
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

add_library(bnet_options INTERFACE)
target_compile_options(bnet_options INTERFACE
    /W4 /permissive- /EHsc /utf-8 /Zc:__cplusplus
    /clang:-Wextra /clang:-Wpedantic /clang:-Wno-language-extension-token)
target_compile_definitions(bnet_options INTERFACE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(bnet_options INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/src")

add_library(bnet_core STATIC src/catalog.cpp src/platform.cpp)
target_link_libraries(bnet_core PUBLIC bnet_options shell32 ole32 advapi32 version uuid)

add_executable(better-net WIN32 src/main.cpp src/app.rc)
set_source_files_properties(src/app.rc PROPERTIES OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/app.manifest")
target_link_libraries(better-net PRIVATE bnet_core d2d1 dwrite windowscodecs dwmapi comctl32)
target_link_options(better-net PRIVATE /MANIFEST:NO)

include(CTest)
if(BUILD_TESTING)
    add_executable(core_tests tests/core_tests.cpp)
    target_link_libraries(core_tests PRIVATE bnet_core)
    add_test(NAME core COMMAND core_tests)
endif()

install(TARGETS better-net RUNTIME DESTINATION .)
