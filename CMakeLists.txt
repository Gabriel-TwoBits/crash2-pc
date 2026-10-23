cmake_minimum_required(VERSION 3.24)
project(Crash2PC LANGUAGES C CXX ASM)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "crash2-pc is a 32-bit build: configure with -DCMAKE_TOOLCHAIN_FILE=cmake/linux-i686.cmake")
else()
    message(STATUS "32-bit build")
endif()

find_package(Python3 REQUIRED COMPONENTS Interpreter)

# Optimized, with its debugging symbols, unless a build type is asked for: left unset, SDL3's build makes it Debug,
# which runs the game at half the speed
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "The build's type" FORCE)
else()
    message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
endif()

# SDL3 from its release, pinned, built for the 32-bit program: Debian's i386 package conflicts with backports' Mesa. A
# shared library beside the program, out of the game's 8 MB at 0x80000000 (src/host.h); it loads the windowing and
# audio libraries (X11, Wayland, PulseAudio, ALSA) at run time
include(FetchContent)
set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(SDL3
    URL https://github.com/libsdl-org/SDL/releases/download/release-3.2.10/SDL3-3.2.10.tar.gz
    URL_HASH SHA256=f87be7b4dec66db4098e9c167b2aa34e2ca10aeb5443bdde95ae03185ed513e0)
FetchContent_MakeAvailable(SDL3)

set(DECOMP ${CMAKE_SOURCE_DIR}/decomp)
set(HOST_ASM ${CMAKE_BINARY_DIR}/host-asm)
set(SPLIT_DATA ${DECOMP}/asm/data/data.data.s ${DECOMP}/asm/data/bss.bss.s ${DECOMP}/asm/data/rodata.rodata.s)
foreach(file ${SPLIT_DATA})
    if(NOT EXISTS ${file})
        message(FATAL_ERROR "${file} is missing: split the decompilation first (README.md, \"Building\")")
    else()
        message(STATUS "Split data: ${file}")
    endif()
endforeach()

# The split's data for the host's assembler, generated from the player's executable into the build directory
set(HOST_DATA ${HOST_ASM}/data.s ${HOST_ASM}/bss.s ${HOST_ASM}/rodata.s)
# 32-bit Windows' COFF names the C's symbols with a leading underscore and flags its sections its own way
if(WIN32)
    set(HOST_DATA_FORMAT --coff)
else()
    set(HOST_DATA_FORMAT)
endif()
add_custom_command(
    OUTPUT ${HOST_DATA} ${HOST_ASM}/macro.inc
    COMMAND Python3::Interpreter ${CMAKE_SOURCE_DIR}/tools/host_data.py ${DECOMP} ${HOST_ASM} ${HOST_DATA_FORMAT}
    DEPENDS ${CMAKE_SOURCE_DIR}/tools/host_data.py ${SPLIT_DATA}
    COMMENT "Rewriting the split's data for the host")
add_custom_target(host_data DEPENDS ${HOST_DATA} ${HOST_ASM}/macro.inc)

# The libraries' functions at their retail addresses, for the interpreter to run them from the disc (src/sdk.c)
file(GLOB_RECURSE SDK_ASM CONFIGURE_DEPENDS ${DECOMP}/asm/sdk/*.s)
set(SDK_SYMBOLS ${CMAKE_BINARY_DIR}/sdk/sdk_symbols.h)
add_custom_command(
    OUTPUT ${SDK_SYMBOLS}
    COMMAND Python3::Interpreter ${CMAKE_SOURCE_DIR}/tools/sdk_symbols.py ${DECOMP} ${SDK_SYMBOLS}
    DEPENDS ${CMAKE_SOURCE_DIR}/tools/sdk_symbols.py ${SDK_ASM}
    COMMENT "Listing the libraries' retail addresses")
set_source_files_properties(src/sdk.c PROPERTIES OBJECT_DEPENDS ${SDK_SYMBOLS})

# The GPU renderer's shaders (shaders/), GLSL compiled to SPIR-V for SDL_GPU's Vulkan, each a C array in a header
find_program(GLSLANG glslangValidator REQUIRED)
set(SHADER_HEADERS)
foreach(shader draw.vert draw.frag present.vert present.frag)
    string(REPLACE "." "_" shader_name ${shader})
    set(header ${CMAKE_BINARY_DIR}/shaders/${shader_name}.h)
    add_custom_command(
        OUTPUT ${header}
        COMMAND ${GLSLANG} -V --quiet --vn ${shader_name} -o ${header} ${CMAKE_SOURCE_DIR}/shaders/${shader}
        DEPENDS ${CMAKE_SOURCE_DIR}/shaders/${shader}
        COMMENT "Compiling the shader ${shader}")
    list(APPEND SHADER_HEADERS ${header})
endforeach()
set_source_files_properties(src/render.c PROPERTIES OBJECT_DEPENDS "${SHADER_HEADERS}")

# The game's C, as the decompilation's equivalent build has it, with the host's platform macros (src/host.h)
# The retail image's order, which the link keeps (tools/check_layout.py): the split's data, each game file's small
# data, the bss (tools/host_data.py puts it in .data, so the host's own data can't come between)
file(GLOB GAME_SOURCES CONFIGURE_DEPENDS ${DECOMP}/src/game/*.c)
add_library(game OBJECT ${HOST_ASM}/data.s ${GAME_SOURCES} ${HOST_ASM}/bss.s ${HOST_ASM}/rodata.s)
add_dependencies(game host_data)
# The split's data includes macro.inc: the host's (tools/host_data.py) before the decompilation's, which is MIPS ELF's
target_include_directories(game BEFORE PRIVATE ${HOST_ASM})
target_include_directories(game PRIVATE ${DECOMP}/include)
target_compile_definitions(game PRIVATE NON_MATCHING GTE_MODEL main=Game_Main)
# The PlayStation's GCC 2.7.2 assumed no strict aliasing, wrapping arithmetic and common symbols; the decompilation's
# warnings are its own (-w). Each file's data stays in .data in its order, as the retail image's small data
# (tools/check_layout.py). Unoptimized whatever the build's type: at -O2 the game's C plays otherwise (the demo's run
# parts from the debug build's and stops on a polyline of over 256 vertices)
target_compile_options(game PRIVATE
    -O0 -std=gnu89 -w -fno-pie -fcommon -fno-strict-aliasing -fwrapv -fno-delete-null-pointer-checks
    -fno-zero-initialized-in-bss -fno-toplevel-reorder -fdata-sections
    -include ${CMAKE_SOURCE_DIR}/src/host.h "-Wa,-I${HOST_ASM}")

set_source_files_properties(${HOST_DATA} PROPERTIES GENERATED TRUE COMPILE_OPTIONS "-Wa,-I${HOST_ASM}")

# The port: on Linux the program, crash2. On Windows a DLL, crash2-game.dll, which crash2.exe loads: Wine reserves a
# 32-bit process's address space from 0x80000000 until it is running, so the program can't be mapped there, and a
# library loaded then can (docs/ROADMAP.md, milestone 8)
set(PORT_SOURCES
    src/main.c
    src/disc.c
    src/preferences.c
    src/audio.c
    src/game_clock.c
    src/gool.c
    src/menus.c
    src/settings.c
    src/host_log.c
    src/input.c
    src/gpu.c
    src/gpu_raster.cpp
    src/render.c
    src/precise.c
    src/between.c
    third_party/swanstation/core/gpu_sw_backend.cpp
    src/spu_host.cpp
    third_party/swanstation/core/spu.cpp
    src/gte.c
    src/low_ram.c
    src/sampler.c
    src/mips.c
    src/sdk.c
    src/native.c
    src/handwritten.c
    src/video.c
    src/psyq/events.c
    src/psyq/libc.c
    src/psyq/libcd.c
    src/psyq/libgpu.c
    src/psyq/libgte.c
    src/psyq/pad.c
    src/psyq/stubs.c
    src/psyq/card.c
    $<TARGET_OBJECTS:game>)
if(WIN32)
    set(PORT crash2-game)
    add_library(${PORT} SHARED ${PORT_SOURCES})
    set_target_properties(${PORT} PROPERTIES PREFIX "")
    add_executable(crash2 src/launcher.c)
    target_compile_options(crash2 PRIVATE -std=gnu11 -Wall -Wextra)
    # Large-address-aware, to reach 0x80000000; Linux's 8 MB of stack for the game's thread; a window's program, with
    # no console of its own (an error shows in a message box)
    target_link_options(crash2 PRIVATE -Wl,--large-address-aware -Wl,--stack,0x800000 -static-libgcc -mwindows)
    add_dependencies(crash2 ${PORT})
else()
    set(PORT crash2)
    add_executable(${PORT} ${PORT_SOURCES})
endif()
# On Linux, position-independent code: the C library's variables (stderr) stay in it, where a copy in the program's bss
# would sit among the game's
if(WIN32)
    set(HOST_PIC)
else()
    set(HOST_PIC -fPIC)
endif()
target_compile_options(${PORT} PRIVATE $<$<COMPILE_LANGUAGE:C>:-std=gnu11 -Wall -Wextra ${HOST_PIC}>)
target_include_directories(${PORT} PRIVATE src ${CMAKE_BINARY_DIR}/sdk ${CMAKE_BINARY_DIR}/shaders)
# SwanStation's rasterizer (third_party/swanstation/README.md), C++17 as it is written
target_include_directories(${PORT} PRIVATE src/spu_shim third_party/swanstation third_party/swanstation/core)
target_compile_options(${PORT} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-std=c++17 ${HOST_PIC}>)
target_link_libraries(${PORT} PRIVATE SDL3::SDL3-shared m)
# libgte's functions run decomp's GTE model (include/gte.h): compiled as the game's C is
set_source_files_properties(src/psyq/libgte.c PROPERTIES
    INCLUDE_DIRECTORIES "${HOST_ASM};${DECOMP}/include"
    COMPILE_OPTIONS "-std=gnu89;-w;-DGTE_MODEL;-include;${CMAKE_SOURCE_DIR}/src/host.h")
set_target_properties(${PORT} PROPERTIES BUILD_RPATH "\$ORIGIN/_deps/sdl3-build" INSTALL_RPATH "\$ORIGIN")
# At the PlayStation's RAM, 0x80000000 (src/host.h), and not position-independent: the game's data holds absolute
# addresses. The BIOS's file calls go to src/psyq/card.c. On Windows the library is based there without ASLR, the
# compiler's runtime linked in, SDL3.dll beside it; moved elsewhere (0x80000000 taken), main.c's check stops it
target_link_options(${PORT} PRIVATE -Wl,--wrap=open -Wl,--wrap=read -Wl,--wrap=write -Wl,--wrap=close)
if(WIN32)
    target_link_options(${PORT} PRIVATE -Wl,--image-base=0x80000000 -Wl,--disable-dynamicbase -static-libgcc
        -static-libstdc++ -Wl,-T,${CMAKE_SOURCE_DIR}/cmake/game_data_pe.ld)
    add_custom_command(TARGET ${PORT} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:SDL3::SDL3-shared> $<TARGET_FILE_DIR:${PORT}>
        COMMENT "Copying SDL3.dll beside the program")
    set(LAYOUT_SYMBOLS --nm ${CMAKE_NM} --underscore)
else()
    target_link_options(${PORT} PRIVATE -no-pie -Wl,-Ttext-segment=0x80000000
        -Wl,-T,${CMAKE_SOURCE_DIR}/cmake/game_data.ld)
    set(LAYOUT_SYMBOLS)
endif()
add_custom_command(TARGET ${PORT} POST_BUILD
    COMMAND Python3::Interpreter ${CMAKE_SOURCE_DIR}/tools/check_layout.py $<TARGET_FILE:${PORT}>
        ${DECOMP}/config/symbol_addrs.txt ${LAYOUT_SYMBOLS}
    COMMENT "Checking the game's data layout")
