cmake_minimum_required(VERSION 3.24)
project(Crash2PC LANGUAGES C CXX ASM)

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
# The 64-bit build's module made on another system (cmake/host64.cmake), for one without the split: macOS on GitHub's
# runners (.github/workflows/macos.yml)
set(CRASH2_MODULE_DIR "" CACHE PATH "A module made elsewhere: the folder of its crash2_module.c and crash2_module.h")
if(CRASH2_MODULE_DIR)
    message(STATUS "The module from ${CRASH2_MODULE_DIR}: no split needed")
else()
    foreach(file ${SPLIT_DATA})
        if(NOT EXISTS ${file})
            message(FATAL_ERROR "${file} is missing: split the decompilation first (README.md, \"Building\")")
        else()
            message(STATUS "Split data: ${file}")
        endif()
    endforeach()
endif()

# The split's data for the host's assembler, generated from the player's executable into the build directory
set(HOST_DATA ${HOST_ASM}/data.s ${HOST_ASM}/bss.s ${HOST_ASM}/rodata.s)
# 32-bit Windows' COFF names the C's symbols with a leading underscore and flags its sections its own way
if(WIN32)
    set(HOST_DATA_FORMAT --coff)
else()
    set(HOST_DATA_FORMAT)
endif()
# The read-only blocks it writes are those the decompilation's C names (INCLUDE_RODATA), which the compiler's own
# dependencies don't follow: it runs again on a change to that C
file(GLOB_RECURSE DECOMP_SOURCES CONFIGURE_DEPENDS ${DECOMP}/src/*.c)
add_custom_command(
    OUTPUT ${HOST_DATA} ${HOST_ASM}/macro.inc ${HOST_ASM}/retail_bytes.s
    COMMAND Python3::Interpreter ${CMAKE_SOURCE_DIR}/tools/host_data.py ${DECOMP} ${HOST_ASM} ${HOST_DATA_FORMAT}
    DEPENDS ${CMAKE_SOURCE_DIR}/tools/host_data.py ${SPLIT_DATA} ${DECOMP_SOURCES}
    COMMENT "Rewriting the split's data for the host")
add_custom_target(host_data DEPENDS ${HOST_DATA} ${HOST_ASM}/macro.inc ${HOST_ASM}/retail_bytes.s)

# The libraries' functions at their retail addresses, for the interpreter to run them from the disc (src/sdk.c)
file(GLOB_RECURSE SDK_ASM CONFIGURE_DEPENDS ${DECOMP}/asm/sdk/*.s)
set(SDK_SYMBOLS ${CMAKE_BINARY_DIR}/sdk/sdk_symbols.h)
add_custom_command(
    OUTPUT ${SDK_SYMBOLS}
    COMMAND Python3::Interpreter ${CMAKE_SOURCE_DIR}/tools/sdk_symbols.py ${DECOMP} ${SDK_SYMBOLS}
    DEPENDS ${CMAKE_SOURCE_DIR}/tools/sdk_symbols.py ${SDK_ASM}
    COMMENT "Listing the libraries' retail addresses")
set_source_files_properties(src/sdk.c PROPERTIES OBJECT_DEPENDS ${SDK_SYMBOLS})

# The GPU renderer's shaders (shaders/), GLSL compiled to SPIR-V for SDL_GPU's Vulkan, each a C array in a header; on
# macOS, whose SDL_GPU is Metal's, each also as MSL (tools/msl_shader.py)
find_program(GLSLANG glslangValidator REQUIRED)
if(APPLE)
    find_program(SPIRV_CROSS spirv-cross REQUIRED)
else()
    set(SPIRV_CROSS)
endif()
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
    if(APPLE)
        set(msl_header ${CMAKE_BINARY_DIR}/shaders/${shader_name}_msl.h)
        add_custom_command(
            OUTPUT ${msl_header}
            COMMAND Python3::Interpreter ${CMAKE_SOURCE_DIR}/tools/msl_shader.py ${GLSLANG} ${SPIRV_CROSS}
                ${CMAKE_SOURCE_DIR}/shaders/${shader} ${shader_name} ${msl_header}
            DEPENDS ${CMAKE_SOURCE_DIR}/tools/msl_shader.py ${CMAKE_SOURCE_DIR}/shaders/${shader}
            COMMENT "Translating the shader ${shader} to MSL")
        list(APPEND SHADER_HEADERS ${msl_header})
    else()
        # SPIR-V only
    endif()
endforeach()
set_source_files_properties(src/render.c PROPERTIES OBJECT_DEPENDS "${SHADER_HEADERS}")

# A 64-bit system builds the game's C through wasm32 (cmake/host64.cmake); a 32-bit one as it is, below
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(STATUS "64-bit build, through wasm32")
    include(cmake/host64.cmake)
    return()
elseif(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "a ${CMAKE_SIZEOF_VOID_P}-byte pointer: crash2-pc builds for 32-bit or 64-bit systems")
elseif(CRASH2_MODULE_DIR)
    message(FATAL_ERROR "CRASH2_MODULE_DIR is the 64-bit build's: the 32-bit build compiles the game's C itself")
else()
    message(STATUS "32-bit build")
endif()

# The game's C, as the decompilation's equivalent build has it, with the host's platform macros (src/host.h)
# The retail image's order, which the link keeps (tools/check_layout.py): the split's data, each game file's small
# data, the bss (tools/host_data.py puts it in .data, so the host's own data can't come between)
file(GLOB GAME_SOURCES CONFIGURE_DEPENDS ${DECOMP}/src/game/*.c)
add_library(game OBJECT ${HOST_ASM}/data.s ${GAME_SOURCES} ${HOST_ASM}/bss.s ${HOST_ASM}/rodata.s)
# A file's read-only blocks come in through `.include` (INCLUDE_RODATA), which its dependencies don't list: the game's
# files build again when tools/host_data.py writes them (in under a second)
set_source_files_properties(${GAME_SOURCES} PROPERTIES OBJECT_DEPENDS ${HOST_ASM}/retail_bytes.s)
add_dependencies(game host_data)
# The split's data includes macro.inc: the host's (tools/host_data.py) before the decompilation's, which is MIPS ELF's
target_include_directories(game BEFORE PRIVATE ${HOST_ASM})
target_include_directories(game PRIVATE ${DECOMP}/include)
target_compile_definitions(game PRIVATE NON_MATCHING GTE_MODEL main=Game_Main)
# The PlayStation's GCC 2.7.2 assumed no strict aliasing, wrapping arithmetic and common symbols, nor loops bounded by
# an array's declared size: the C indexes structures' one-element last arrays past their end (docs/ROADMAP.md,
# milestone 14). The decompilation's warnings are its own (-w). Each file's data stays in .data in its order, as the
# retail image's small data (tools/check_layout.py). Optimized as the build's type says
set(GAME_SEMANTICS -fcommon -fno-strict-aliasing -fwrapv -fno-delete-null-pointer-checks
    -fno-aggressive-loop-optimizations)
target_compile_options(game PRIVATE
    -std=gnu89 -w -fno-pie ${GAME_SEMANTICS}
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
    src/retail_data.c
    ${HOST_ASM}/retail_bytes.s
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
# The port's own C++ warns as its C does; SwanStation's stays as it is written. Its draw commands end in a zero-length
# array, which src/gpu_raster.cpp's buffers size and GCC can't see
set_source_files_properties(src/spu_host.cpp PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra")
set_source_files_properties(src/gpu_raster.cpp PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra;-Wno-array-bounds")
target_link_libraries(${PORT} PRIVATE SDL3::SDL3-shared m)
# libgte's functions run decomp's GTE model (include/gte.h): compiled with the game's C's semantics, optimized
set_source_files_properties(src/psyq/libgte.c PROPERTIES
    INCLUDE_DIRECTORIES "${HOST_ASM};${DECOMP}/include"
    COMPILE_OPTIONS "-std=gnu89;-w;${GAME_SEMANTICS};-DGTE_MODEL;-include;${CMAKE_SOURCE_DIR}/src/host.h")
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
