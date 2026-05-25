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
# SDK 2.2 ships adafruit_feather_rp2350.h (no _hstx variant) — the
# HSTX-variant board (Adafruit P/N 6130) is electrically the same
# RP2350A core for our purposes; PSRAM CS pin override is applied
# below via target_compile_definitions.
set(PICO_BOARD adafruit_feather_rp2350 CACHE STRING "Adafruit Feather RP2350 (also covers 6130 HSTX+PSRAM)")

include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(saturn_jit_rp2350 C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# --- options ---------------------------------------------------------

set(JIT_CACHE_REGION "SRAM" CACHE STRING "JIT code cache region: SRAM or PSRAM")
set_property(CACHE JIT_CACHE_REGION PROPERTY STRINGS SRAM PSRAM)

set(JIT_CACHE_SIZE "262144" CACHE STRING "JIT code cache size in bytes")
set(PSRAM_NOCACHE "0" CACHE STRING "Route the PSRAM-backed JIT cache through the uncached alias 0x15000000 to bypass the 16 KiB XIP cache")
set(PSRAM_CS_PIN "8" CACHE STRING "GPIO for PSRAM chip select (Adafruit Feather RP2350 6130 expected to be 8)")
set(SYS_CLOCK_KHZ "0" CACHE STRING "System clock in kHz (0 = SDK default 150 MHz)")
set(VREG_VOLTAGE "VREG_VOLTAGE_1_10" CACHE STRING "VDD core voltage enum (e.g. VREG_VOLTAGE_1_30) — passed to SDK's SYS_CLK_VREG_VOLTAGE_AUTO_ADJUST")
# PLL params accompany SYS_CLOCK_KHZ — get these from
#   python3 ${PICO_SDK_PATH}/src/rp2_common/hardware_clocks/scripts/vcocalc.py <freq_MHz>
# Some example combos (XOSC=12 MHz, REFDIV=1, integer FBDIV):
#   200 MHz  vco=1200 pd1=6 pd2=1     250 MHz  vco=1500 pd1=6 pd2=1
#   300 MHz  vco=1500 pd1=5 pd2=1     324 MHz  vco=1296 pd1=4 pd2=1
#   336 MHz  vco=1344 pd1=4 pd2=1     400 MHz  vco=1200 pd1=3 pd2=1
#   480 MHz  vco=1440 pd1=3 pd2=1     500 MHz  vco=1500 pd1=3 pd2=1
set(PLL_SYS_VCO_HZ "1500000000" CACHE STRING "PLL_SYS VCO frequency (Hz)")
set(PLL_SYS_PD1    "3"          CACHE STRING "PLL_SYS postdiv1")
set(PLL_SYS_PD2    "1"          CACHE STRING "PLL_SYS postdiv2")

# Pass SYS_CLK_KHZ + PLL_SYS_* + VREG settings to the SDK GLOBALLY (not
# via target_compile_definitions on bench) — runtime_init_clocks.c is
# compiled as part of the SDK library, and its #if for the auto-adjust
# is evaluated at SDK-library compile time, not at bench-link time.
if(NOT SYS_CLOCK_KHZ STREQUAL "0")
    add_compile_definitions(
        SYS_CLK_KHZ=${SYS_CLOCK_KHZ}
        PLL_SYS_VCO_FREQ_HZ=${PLL_SYS_VCO_HZ}
        PLL_SYS_POSTDIV1=${PLL_SYS_PD1}
        PLL_SYS_POSTDIV2=${PLL_SYS_PD2}
        SYS_CLK_VREG_VOLTAGE_AUTO_ADJUST=1
        SYS_CLK_VREG_VOLTAGE_MIN=${VREG_VOLTAGE}
        # Bump the QMI flash divider so flash XIP stays at ~50 MHz when
        # sys is pushed up — SDK default is /2 which exceeds the W25Q's
        # 133 MHz QSPI ceiling at sys ≥ 270 MHz.
        PICO_FLASH_SPI_CLKDIV=8
    )
endif()

pico_sdk_init()

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
    PSRAM_CS_PIN=${PSRAM_CS_PIN}
    $<$<NOT:$<STREQUAL:${PSRAM_NOCACHE},0>>:PSRAM_NOCACHE=1>
    SYS_CLOCK_KHZ=${SYS_CLOCK_KHZ}
    # Wait up to 10 s after boot for the host to open the USB CDC port,
    # otherwise the header + first RESULT lines are flushed before any
    # capture session attaches.
    PICO_STDIO_USB_CONNECT_WAIT_TIMEOUT_MS=10000
    PICO_STDIO_USB_POST_CONNECT_WAIT_DELAY_MS=200
)
# (SYS_CLK_KHZ, PLL_SYS_*, SYS_CLK_VREG_VOLTAGE_*, PICO_FLASH_SPI_CLKDIV
#  are set via add_compile_definitions() above so the SDK's
#  runtime_init_clocks.c sees them too.)

target_link_libraries(bench PRIVATE
    pico_stdlib
    hardware_dma
    hardware_timer
    hardware_clocks
)

pico_enable_stdio_uart(bench 0)
pico_enable_stdio_usb(bench 1)

# When overclocking, run the whole image from SRAM so flash-XIP
# timing isn't a constraint on sys_clk. SDK's copy_to_ram linker
# layout copies .text+.data from flash to SRAM at boot and the CPU
# never re-fetches from flash. Cost: ~75 KB extra SRAM occupied.
if(NOT SYS_CLOCK_KHZ STREQUAL "0")
    pico_set_binary_type(bench copy_to_ram)
    message(STATUS "  copy_to_ram: ON (image runs from SRAM)")
endif()

pico_add_extra_outputs(bench)

# Output names so the SRAM and PSRAM builds don't collide if put in
# the same directory.
string(TOLOWER "${JIT_CACHE_REGION}" _region_lc)
set_target_properties(bench PROPERTIES OUTPUT_NAME "bench_${_region_lc}")

message(STATUS "Saturn JIT RP2350 build:")
message(STATUS "  JIT cache region: ${JIT_CACHE_REGION}")
message(STATUS "  JIT cache size  : ${JIT_CACHE_SIZE} bytes")
