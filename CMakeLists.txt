cmake_minimum_required(VERSION 3.24)
project(Crash2PC LANGUAGES C ASM)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "crash2-pc is a 32-bit build: configure with -DCMAKE_TOOLCHAIN_FILE=cmake/linux-i686.cmake")
else()
    message(STATUS "32-bit build")
endif()

find_package(Python3 REQUIRED COMPONENTS Interpreter)

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
add_custom_command(
    OUTPUT ${HOST_DATA} ${HOST_ASM}/macro.inc
    COMMAND Python3::Interpreter ${CMAKE_SOURCE_DIR}/tools/host_data.py ${DECOMP} ${HOST_ASM}
    DEPENDS ${CMAKE_SOURCE_DIR}/tools/host_data.py ${SPLIT_DATA}
    COMMENT "Rewriting the split's data for the host")
add_custom_target(host_data DEPENDS ${HOST_DATA} ${HOST_ASM}/macro.inc)

# The game's C, as the decompilation's equivalent build has it, with the host's platform macros (src/host.h)
# The retail image's order, which the link keeps (tools/check_layout.py): the split's data, each game file's small
# data, the bss (tools/host_data.py puts it in .data, so the host's own data can't come between)
file(GLOB GAME_SOURCES CONFIGURE_DEPENDS ${DECOMP}/src/game/*.c)
add_library(game OBJECT ${HOST_ASM}/data.s ${GAME_SOURCES} ${HOST_ASM}/bss.s ${HOST_ASM}/rodata.s)
add_dependencies(game host_data)
target_include_directories(game PRIVATE ${DECOMP}/include)
target_compile_definitions(game PRIVATE NON_MATCHING GTE_MODEL main=Game_Main)
# The PlayStation's GCC 2.7.2 assumed no strict aliasing, wrapping arithmetic and common symbols; the decompilation's
# warnings are its own (-w). Each file's data stays in .data in its order, as the retail image's small data
# (tools/check_layout.py)
target_compile_options(game PRIVATE
    -std=gnu89 -w -fno-pie -fcommon -fno-strict-aliasing -fwrapv -fno-delete-null-pointer-checks
    -fno-zero-initialized-in-bss -fno-toplevel-reorder -fdata-sections
    -include ${CMAKE_SOURCE_DIR}/src/host.h "-Wa,-I${HOST_ASM}")

set_source_files_properties(${HOST_DATA} PROPERTIES GENERATED TRUE COMPILE_OPTIONS "-Wa,-I${HOST_ASM}")

add_executable(crash2
    src/main.c
    src/host_log.c
    src/gpu.c
    src/gte.c
    src/low_ram.c
    src/sampler.c
    src/native.c
    src/handwritten.c
    src/video.c
    src/psyq/events.c
    src/psyq/libcd.c
    src/psyq/libgpu.c
    src/psyq/libgte.c
    src/psyq/libspu.c
    src/psyq/pad.c
    src/psyq/stubs.c
    src/psyq/bios_files.c
    $<TARGET_OBJECTS:game>)
# Position-independent code: the C library's variables (stderr) stay in it, where a copy in the program's bss would sit
# among the game's
target_compile_options(crash2 PRIVATE $<$<COMPILE_LANGUAGE:C>:-std=gnu11 -Wall -Wextra -fPIC>)
target_include_directories(crash2 PRIVATE src)
target_link_libraries(crash2 PRIVATE SDL3::SDL3-shared m)
# libgte's functions run decomp's GTE model (include/gte.h): compiled as the game's C is
set_source_files_properties(src/psyq/libgte.c PROPERTIES
    INCLUDE_DIRECTORIES ${DECOMP}/include
    COMPILE_OPTIONS "-std=gnu89;-w;-DGTE_MODEL;-include;${CMAKE_SOURCE_DIR}/src/host.h")
set_target_properties(crash2 PROPERTIES BUILD_RPATH "\$ORIGIN/_deps/sdl3-build" INSTALL_RPATH "\$ORIGIN")
# At the PlayStation's RAM, 0x80000000 (src/host.h), and not position-independent: the game's data holds absolute
# addresses. The BIOS's file calls go to src/psyq/bios_files.c
target_link_options(crash2 PRIVATE -no-pie -Wl,-Ttext-segment=0x80000000 -Wl,-T,${CMAKE_SOURCE_DIR}/cmake/game_data.ld
    -Wl,--wrap=open -Wl,--wrap=read -Wl,--wrap=write -Wl,--wrap=close)
add_custom_command(TARGET crash2 POST_BUILD
    COMMAND Python3::Interpreter ${CMAKE_SOURCE_DIR}/tools/check_layout.py $<TARGET_FILE:crash2>
        ${DECOMP}/config/symbol_addrs.txt
    COMMENT "Checking the game's data layout")
