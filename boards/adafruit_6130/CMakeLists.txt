# Adafruit Feather RP2350 (HSTX, 8 MB PSRAM) — board build for the
# Saturn JIT bench. Reuses the platform-independent translator from
# m33/src and adds RP2350-specific glue:
#   - bench_main.c       — bench harness (replaces m33/src/bench_main.c)
#   - sh_impl.c          — sh_* helpers backed by Pico SDK stdio + DWT
#   - psram_init.c       — APS6404 PSRAM bring-up via QMI CS1
#
# Pick JIT cache region with:
#   cmake -B build -DJIT_CACHE_REGION=SRAM   (default, 256 KiB)
#   cmake -B build -DJIT_CACHE_REGION=PSRAM  (default, 256 KiB; up to ~8 MiB)
# and optionally:
#   cmake -B build -DJIT_CACHE_SIZE=524288   (bytes)

cmake_minimum_required(VERSION 3.13)

# Pico SDK 2.x targets RP2350 with `set(PICO_PLATFORM rp2350-arm-s)`.
set(PICO_PLATFORM rp2350-arm-s)
set(PICO_BOARD adafruit_feather_rp2350_hstx CACHE STRING "Adafruit Feather RP2350 HSTX (6130)")

include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(saturn_jit_rp2350 C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

# --- options ---------------------------------------------------------

set(JIT_CACHE_REGION "SRAM" CACHE STRING "JIT code cache region: SRAM or PSRAM")
set_property(CACHE JIT_CACHE_REGION PROPERTY STRINGS SRAM PSRAM)

set(JIT_CACHE_SIZE "262144" CACHE STRING "JIT code cache size in bytes")

# --- shared sources --------------------------------------------------

set(M33_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../../m33/src)

add_executable(bench
    bench_main.c
    sh_impl.c
    psram_init.c
    ${M33_SRC}/saturn_registers.c
    ${M33_SRC}/saturn_interp.c
    ${M33_SRC}/workload.c
    ${M33_SRC}/workload_nqueens.c
    ${M33_SRC}/keyboard.c
    ${M33_SRC}/thumb2_emit.c
    ${M33_SRC}/thumb2_selftest.c
    ${M33_SRC}/saturn_jit.c
    ${M33_SRC}/jit_helpers.c
    ${M33_SRC}/jit_dispatch.c
)

target_include_directories(bench PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${M33_SRC}
)

target_compile_options(bench PRIVATE -O3 -ffunction-sections -fdata-sections)

target_compile_definitions(bench PRIVATE
    BENCH_SKIP_CROSSCHECK=1
    BENCH_SKIP_JIT_OFF=1
    JIT_CACHE_REGION_${JIT_CACHE_REGION}=1
    JIT_CACHE_BYTES=${JIT_CACHE_SIZE}
)

target_link_libraries(bench PRIVATE
    pico_stdlib
    hardware_dma
    hardware_timer
    hardware_xip_cache
)

pico_enable_stdio_uart(bench 0)
pico_enable_stdio_usb(bench 1)

pico_add_extra_outputs(bench)

# Output names so the SRAM and PSRAM builds don't collide if put in
# the same directory.
string(TOLOWER "${JIT_CACHE_REGION}" _region_lc)
set_target_properties(bench PROPERTIES OUTPUT_NAME "bench_${_region_lc}")

message(STATUS "Saturn JIT RP2350 build:")
message(STATUS "  JIT cache region: ${JIT_CACHE_REGION}")
message(STATUS "  JIT cache size  : ${JIT_CACHE_SIZE} bytes")
