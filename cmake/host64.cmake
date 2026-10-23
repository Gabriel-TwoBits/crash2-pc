# The 64-bit build (docs/ROADMAP.md, milestone 16): the game's C needs 32-bit pointers, which wasm32 keeps. The game's
# C and the host's but the GPU renderer become a wasm module (tools/wasm/module.py), which wasm2c turns into C; the
# program compiles it natively with SDL's side of the host (src/host64) and the GPU renderer (src/render.c).
# wasi-sdk and WABT come pinned, for the build's system.

if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$")
    set(WASI_SDK_ASSET wasi-sdk-34.0-x86_64-linux.tar.gz)
    set(WASI_SDK_SHA256 b761e3a0721dbae9c09a0059e5fdb2bf917d1b4a8a7b430fb3b5aafb0984b2c4)
    set(WABT_ASSET wabt-1.0.42-linux-x64.tar.gz)
    set(WABT_SHA256 84895407a6bbb80e918f33b16b2fb2206021c150b6bc9ff6f761263a745ab131)
elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
    set(WASI_SDK_ASSET wasi-sdk-34.0-arm64-linux.tar.gz)
    set(WASI_SDK_SHA256 f7e243dff54d60bcc576e94d6166b69f410f2500ae4a9ceef34315be10e77971)
    set(WABT_ASSET wabt-1.0.42-linux-arm64.tar.gz)
    set(WABT_SHA256 a7632c1cc66f70886441842ecf5a2ffa91a98c79e026059458451234ad013ea8)
elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin" AND CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
    set(WASI_SDK_ASSET wasi-sdk-34.0-arm64-macos.tar.gz)
    set(WASI_SDK_SHA256 9c59398106b417f8f14913380fdf0097a8cc0ff4af9eb3ce0065a859e88d49e9)
    set(WABT_ASSET wabt-1.0.42-macos-arm64.tar.gz)
    set(WABT_SHA256 3f654779b436c628db3ca4323c51538815f5fbc887a6d5f3bb1c18d36281e240)
elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows" AND CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^(AMD64|x86_64)$")
    set(WASI_SDK_ASSET wasi-sdk-34.0-x86_64-windows.tar.gz)
    set(WASI_SDK_SHA256 cccb5c323a9b34f0349a9b09e8804a0a7632c68c3310f4b5f437ed57d7e71d8f)
    set(WABT_ASSET wabt-1.0.42-windows-x64.tar.gz)
    set(WABT_SHA256 62f1eb2b51aa57cf0f0b8ba333bf2c72a049930aa9c5de01dcad271c4fe48c88)
else()
    # WABT publishes no build for an Intel Mac
    message(FATAL_ERROR "no pinned wasi-sdk and WABT for ${CMAKE_HOST_SYSTEM_NAME} on ${CMAKE_HOST_SYSTEM_PROCESSOR}")
endif()
FetchContent_Declare(wasi_sdk
    URL https://github.com/WebAssembly/wasi-sdk/releases/download/wasi-sdk-34/${WASI_SDK_ASSET}
    URL_HASH SHA256=${WASI_SDK_SHA256})
FetchContent_Declare(wabt
    URL https://github.com/WebAssembly/wabt/releases/download/1.0.42/${WABT_ASSET}
    URL_HASH SHA256=${WABT_SHA256})
# wasi-sdk makes the module; a module made elsewhere needs only wasm2c's runtime
if(CRASH2_MODULE_DIR)
    FetchContent_MakeAvailable(wabt)
else()
    FetchContent_MakeAvailable(wasi_sdk wabt)
endif()
set(WASM2C_RUNTIME ${wabt_SOURCE_DIR}/share/wabt/wasm2c)

# The licences of what the program carries beyond the port's and SDL's, which the packages carry (tools/*/build.sh,
# tools/linux/appimage.sh): wasm2c's runtime, WABT's Apache-2.0, and wasi-sdk's C library, wasi-libc at the commit
# wasi-sdk 34 builds, with the works it derives from. Its binary releases hold none of them
set(LICENSES ${CMAKE_BINARY_DIR}/licenses)
set(WASI_LIBC_SOURCE https://raw.githubusercontent.com/WebAssembly/wasi-libc/2e6fb9d8ee0cdf9e431fbcabe8af3115de000a13)
file(DOWNLOAD https://raw.githubusercontent.com/WebAssembly/wabt/1.0.42/LICENSE ${LICENSES}/LICENSE-wasm2c.txt
    EXPECTED_HASH SHA256=cfc7749b96f63bd31c3c42b5c471bf756814053e847c10f3eb003417bc523d30)
set(WASI_LIBC_LICENSE_PARTS
    LICENSE 2711a8b5a5cdfef0e639f96c1aca12ae23d7d64a02d0507f1bdf14d2b27bbc3a
    LICENSE-MIT 23f18e03dc49df91622fe2a76176497404e46ced8a715d9d2b67a7446571cca3
    libc-top-half/musl/COPYRIGHT b870108ec5e7790e9f9919064f1b9421d62d5f9b0e6c230c6adf7ea2da62e97b
    libc-bottom-half/cloudlibc/LICENSE c8b789cf5a746611e6300a0cc7750dbf92b61912a709d04e639245f7290656d0
    fts/musl-fts/COPYING 55af87e4017668f54467a3380e7ebbac5e672d8c763bfe95e6fc882a6fdc4046)
file(WRITE ${LICENSES}/LICENSE-wasi-libc.txt "")
while(WASI_LIBC_LICENSE_PARTS)
    list(POP_FRONT WASI_LIBC_LICENSE_PARTS part part_sha256)
    string(REPLACE "/" "_" part_file ${part})
    file(DOWNLOAD ${WASI_LIBC_SOURCE}/${part} ${LICENSES}/wasi-libc/${part_file} EXPECTED_HASH SHA256=${part_sha256})
    file(READ ${LICENSES}/wasi-libc/${part_file} part_text)
    file(APPEND ${LICENSES}/LICENSE-wasi-libc.txt "==== wasi-libc: ${part} ====\n\n${part_text}\n")
endwhile()

if(CRASH2_MODULE_DIR)
    set(MODULE ${CRASH2_MODULE_DIR})
    foreach(file crash2_module.c crash2_module.h)
        if(NOT EXISTS ${MODULE}/${file})
            message(FATAL_ERROR "${MODULE}/${file} is missing: CRASH2_MODULE_DIR is tools/wasm/module.py's --out")
        else()
            message(STATUS "Module: ${MODULE}/${file}")
        endif()
    endforeach()
else()
    # The module: every host source but the GPU renderer (native) and the 32-bit build's own
    set(MODULE ${CMAKE_BINARY_DIR}/module)
    set(MODULE_HOST_SOURCES
        src/main.c src/disc.c src/preferences.c src/audio.c src/game_clock.c src/gool.c src/menus.c src/settings.c
        src/host_log.c src/input.c src/gpu.c src/precise.c src/between.c src/gte.c src/low_ram.c src/sampler.c
        src/mips.c src/sdk.c src/retail_data.c src/native.c src/handwritten.c src/video.c src/psyq/events.c
        src/psyq/libc.c src/psyq/libcd.c src/psyq/libgpu.c src/psyq/libgte.c src/psyq/pad.c src/psyq/stubs.c
        src/psyq/card.c src/gpu_raster.cpp src/spu_host.cpp third_party/swanstation/core/gpu_sw_backend.cpp
        third_party/swanstation/core/spu.cpp)
    file(GLOB GAME_SOURCES CONFIGURE_DEPENDS ${DECOMP}/src/game/*.c)
    file(GLOB_RECURSE MODULE_HEADERS CONFIGURE_DEPENDS ${DECOMP}/include/*.h ${CMAKE_SOURCE_DIR}/src/*.h
        ${CMAKE_SOURCE_DIR}/third_party/swanstation/*.h)
    file(GLOB MODULE_SCRIPTS CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/tools/wasm/*.py)
    file(GLOB_RECURSE SDK_ASM CONFIGURE_DEPENDS ${DECOMP}/asm/sdk/*.s)
    add_custom_command(
        OUTPUT ${MODULE}/crash2_module.c ${MODULE}/crash2_module.h
        COMMAND Python3::Interpreter ${CMAKE_SOURCE_DIR}/tools/wasm/module.py --wasi-sdk ${wasi_sdk_SOURCE_DIR}
            --wasm2c ${wabt_SOURCE_DIR}/bin/wasm2c --sdl-include ${sdl3_SOURCE_DIR}/include --out ${MODULE}
            --host ${MODULE_HOST_SOURCES}
        DEPENDS ${MODULE_SCRIPTS} ${CMAKE_SOURCE_DIR}/tools/sdk_symbols.py ${MODULE_HOST_SOURCES} ${GAME_SOURCES}
            ${MODULE_HEADERS} ${SPLIT_DATA} ${SDK_ASM}
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Building the module: the game's C and the host's, wasm32, through wasm2c")
endif()

add_executable(crash2 ${MODULE}/crash2_module.c ${WASM2C_RUNTIME}/wasm-rt-impl.c ${WASM2C_RUNTIME}/wasm-rt-mem-impl.c
    ${WASM2C_RUNTIME}/wasm-rt-exceptions-impl.c src/host64/main.c src/host64/bridge.c src/host64/wasi.c src/render.c)
target_include_directories(crash2 PRIVATE src ${MODULE} ${wabt_SOURCE_DIR}/include ${WASM2C_RUNTIME}
    ${CMAKE_BINARY_DIR}/shaders)
# The generated C and wasm2c's runtime warn as they are written; the program's own as the rest of the port's
set_source_files_properties(${MODULE}/crash2_module.c ${WASM2C_RUNTIME}/wasm-rt-impl.c
    ${WASM2C_RUNTIME}/wasm-rt-mem-impl.c ${WASM2C_RUNTIME}/wasm-rt-exceptions-impl.c PROPERTIES COMPILE_OPTIONS "-w")
set_source_files_properties(src/host64/main.c src/host64/bridge.c src/host64/wasi.c src/render.c PROPERTIES
    COMPILE_OPTIONS "-std=gnu11;-Wall;-Wextra")
target_link_libraries(crash2 PRIVATE SDL3::SDL3-shared m)
if(WIN32)
    # As the 32-bit build's: 8 MB of stack, Linux's; a window's program, with no console of its own (an error shows in
    # a message box); SDL3.dll beside it
    target_link_options(crash2 PRIVATE -Wl,--stack,0x800000 -static-libgcc -mwindows)
    add_custom_command(TARGET crash2 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:SDL3::SDL3-shared> $<TARGET_FILE_DIR:crash2>
        COMMENT "Copying SDL3.dll beside the program")
elseif(APPLE)
    # SDL3's library beside the program in the build, in the Frameworks of the app's bundle (tools/macos/bundle.sh)
    set_target_properties(crash2 PROPERTIES BUILD_WITH_INSTALL_RPATH TRUE
        INSTALL_RPATH "@executable_path;@executable_path/../Frameworks")
    add_custom_command(TARGET crash2 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_SONAME_FILE:SDL3::SDL3-shared>
            $<TARGET_FILE_DIR:crash2>
        COMMENT "Copying SDL3's library beside the program, by the name it is loaded by")
else()
    set_target_properties(crash2 PROPERTIES BUILD_RPATH "\$ORIGIN/_deps/sdl3-build" INSTALL_RPATH "\$ORIGIN")
endif()
