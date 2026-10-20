cmake_minimum_required(VERSION 3.20)

project(MaceRecomp LANGUAGES C CXX)
set(CMAKE_C_STANDARD 17)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if (MSVC)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -D_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR")
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

# RT64 is built as a static library and expects an SDL window on Linux.
set(RT64_STATIC TRUE)
set(RT64_SDL_WINDOW_VULKAN TRUE)
add_compile_definitions(HLSL_CPU)

add_subdirectory(runtime/rt64 ${CMAKE_BINARY_DIR}/rt64)
add_subdirectory(tools/N64ModernRuntime)

find_package(SDL2 REQUIRED)

# Recompiled game code.
file(GLOB FUNC_C_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/generated/functions/*.c)
add_library(RecompiledFuncs STATIC ${FUNC_C_SOURCES})
target_compile_options(RecompiledFuncs PRIVATE -fno-strict-aliasing)
target_include_directories(RecompiledFuncs PUBLIC
    ${CMAKE_SOURCE_DIR}/generated/functions
    ${CMAKE_SOURCE_DIR}/tools/N64ModernRuntime/N64Recomp/include
    ${CMAKE_SOURCE_DIR}/tools/N64ModernRuntime/librecomp/include
    ${CMAKE_SOURCE_DIR}/tools/N64ModernRuntime/ultramodern/include
)

add_executable(mace-recomp
    src/main.cpp
    src/mace_rt64.cpp
    src/mace_overlays.cpp
)

target_compile_options(mace-recomp PRIVATE -fno-strict-aliasing)

target_include_directories(mace-recomp PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/tools/N64ModernRuntime/N64Recomp/include
    ${CMAKE_SOURCE_DIR}/tools/N64ModernRuntime/librecomp/include
    ${CMAKE_SOURCE_DIR}/tools/N64ModernRuntime/ultramodern/include
    ${CMAKE_SOURCE_DIR}/runtime/rt64/src
    ${CMAKE_SOURCE_DIR}/runtime/rt64/src/rhi
    ${CMAKE_SOURCE_DIR}/runtime/rt64/src/render
    ${CMAKE_SOURCE_DIR}/runtime/rt64/src/contrib
    ${CMAKE_SOURCE_DIR}/runtime/rt64/src/contrib/hlslpp/include
    ${CMAKE_BINARY_DIR}/rt64/src
)

target_link_libraries(mace-recomp PRIVATE
    RecompiledFuncs
    librecomp
    ultramodern
    rt64
    SDL2::SDL2
)
