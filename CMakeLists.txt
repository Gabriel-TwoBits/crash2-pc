cmake_minimum_required(VERSION 3.24)
project(Crash2PC LANGUAGES C ASM)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "crash2-pc is a 32-bit build: configure with -DCMAKE_TOOLCHAIN_FILE=cmake/linux-i686.cmake")
else()
    message(STATUS "32-bit build")
endif()

find_package(Python3 REQUIRED COMPONENTS Interpreter)

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
file(GLOB GAME_SOURCES CONFIGURE_DEPENDS ${DECOMP}/src/game/*.c)
add_library(game OBJECT ${GAME_SOURCES})
add_dependencies(game host_data)
target_include_directories(game PRIVATE ${DECOMP}/include)
target_compile_definitions(game PRIVATE NON_MATCHING GTE_MODEL main=Game_Main)
# The PlayStation's GCC 2.7.2 assumed no strict aliasing, wrapping arithmetic and common symbols; the decompilation's
# warnings are its own (-w)
target_compile_options(game PRIVATE
    -std=gnu89 -w -fno-pie -fcommon -fno-strict-aliasing -fwrapv -fno-delete-null-pointer-checks
    -include ${CMAKE_SOURCE_DIR}/src/host.h "-Wa,-I${HOST_ASM}")

set_source_files_properties(${HOST_DATA} PROPERTIES GENERATED TRUE COMPILE_OPTIONS "-Wa,-I${HOST_ASM}")

add_executable(crash2
    src/main.c
    src/host_log.c
    src/gte.c
    src/native.c
    src/handwritten.c
    src/psyq/stubs.c
    src/psyq/bios_files.c
    ${HOST_DATA}
    $<TARGET_OBJECTS:game>)
target_compile_options(crash2 PRIVATE $<$<COMPILE_LANGUAGE:C>:-std=gnu11 -Wall -Wextra -fno-pie>)
target_include_directories(crash2 PRIVATE src)
# Not position-independent: the program sits in src/host.h's window at 0x08000000, and the game's data holds absolute
# addresses. The BIOS's file calls go to src/psyq/bios_files.c
target_link_options(crash2 PRIVATE -no-pie
    -Wl,--wrap=open -Wl,--wrap=read -Wl,--wrap=write -Wl,--wrap=close)
