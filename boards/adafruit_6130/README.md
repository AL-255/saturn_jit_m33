# Adafruit Feather RP2350 (HSTX, 8 MB PSRAM, P/N 6130)

Runs the Saturn JIT bench on a real RP2350 with the JIT code cache
backed by either on-chip SRAM or external PSRAM (8 MB APS6404 over QSPI
CS1). A compile-time flag picks the region; the harness prints
`RESULT,*` lines over USB CDC, in the same shape as the QEMU bench, so
the existing CSV/plot scripts can ingest the data without changes.

## What this directory contains

| File | Purpose |
| --- | --- |
| `CMakeLists.txt` | Pico SDK 2.x build. Pulls in the platform-independent JIT/interp code from `m33/src/` and adds RP2350-specific glue. |
| `bench_main.c` | Board-specific bench entry point — adapts `m33/src/bench_main.c` to use Pico SDK stdio + DWT cycle counter, and allocates the JIT cache buffer in the chosen region. |
| `sh_impl.c` | Implements the `sh_puts`/`sh_elapsed`/`sh_tickfreq`/`sh_exit` helpers that the m33 bench expects (originally ARM-semihosting backed under QEMU) in terms of Pico SDK primitives. |
| `psram_init.c` | Brings up the PSRAM at boot: sends the QPI-enable command to the APS6404, configures QMI CS1 with conservative timing, optionally enables XIP for CS1 so the PSRAM is memory-mapped at `0x11000000`. |
| `cycles.h` | DWT CYCCNT cycle counter shim. |

The actual Saturn translator, interpreter, dispatcher, and workloads
are reused unchanged from `m33/src/` — that code only depends on
Cortex-M33 Thumb-2 + standard C, both of which the RP2350 toolchain
provides.

## Picking the JIT cache region

Two CMake options:

```bash
# JIT cache in on-chip SRAM (baseline; same as the QEMU build):
cmake -B build_sram -DJIT_CACHE_REGION=SRAM -DJIT_CACHE_SIZE=262144

# JIT cache in PSRAM (the comparison case):
cmake -B build_psram -DJIT_CACHE_REGION=PSRAM -DJIT_CACHE_SIZE=2097152
```

The PSRAM build places `_jit_cache_start[]` in a `.psram_bss` section
that the linker locates at `0x11000000`. The SRAM build places it in
ordinary `.bss`.

`JIT_CACHE_SIZE` defaults to 256 KiB; PSRAM allows up to ~8 MiB if you
want to stress-test the cache hit/miss/flush logic at sizes that won't
fit on-chip.

## Building

You need [Pico SDK 2.0+](https://github.com/raspberrypi/pico-sdk) on
your machine; export `PICO_SDK_PATH`:

```bash
export PICO_SDK_PATH=$HOME/pico-sdk
cd boards/adafruit_6130
cmake -B build_sram  -DJIT_CACHE_REGION=SRAM
cmake -B build_psram -DJIT_CACHE_REGION=PSRAM
cmake --build build_sram  -j
cmake --build build_psram -j
```

This produces `bench_sram.uf2` and `bench_psram.uf2`.

## Flashing

Hold the **BOOT** button on the Feather while plugging in USB-C; the
board mounts as `RP2350` mass storage. Drag-and-drop the `.uf2` file
on. The board reboots and starts streaming the bench output over USB
CDC at the standard `/dev/cu.usbmodem*` (macOS) or `/dev/ttyACM*`
(Linux).

```bash
# Capture the run (replace device path with whatever shows up after flash):
screen /dev/cu.usbmodem1101 115200 | tee bench_sram.log
# (Ctrl-A then K to exit screen)
```

The bench finishes when it prints `done.` after the last `RESULT` line.

## Comparing SRAM vs PSRAM

After capturing both `bench_sram.log` and `bench_psram.log`, run the
existing helper:

```bash
python3 boards/adafruit_6130/compare.py bench_sram.log bench_psram.log
```

That produces `psram_vs_sram.png` (a side-by-side bar chart) plus a
markdown table of `jit-on` and `jit-warm` times for each workload in
each cache region.

## Expected behavior

- **SRAM build**: Should match the QEMU numbers within ~10-30%. The
  RP2350's M33 actually runs the emitted Thumb-2 (vs QEMU's TCG
  re-translation), so per-instruction cost is different — typically
  fewer cycles per Saturn op for the simple ops, more for the ops
  that touch memory.
- **PSRAM build**: The first call into each JIT block pays a QSPI
  fetch + XIP cache fill (~10s of cycles per cache line). Subsequent
  calls hit the XIP cache and approach SRAM speed. So `jit-warm`
  should look close to SRAM; `jit-on` will be measurably slower
  because every translated block is fresh in the XIP cache. Larger
  `JIT_CACHE_SIZE` exposes more of this overhead because more blocks
  are unique to the XIP cache.

## What's untested

I (the assistant building this) have no physical access to the board,
so everything in this directory has only been compile-checked, not
run. Likely places to need adjustment after first flash:

1. **PSRAM CS pin / QMI timing**. `psram_init.c` assumes the
   Feather's PSRAM is on QMI CS1 and uses conservative timing. If the
   PSRAM doesn't respond, double-check the schematic for which GPIO
   is the PSRAM chip-select and adjust `PSRAM_CS_PIN`.
2. **XIP-cache invalidation after JIT emit**. After writing JIT code
   to PSRAM, the harness flushes the XIP cache before BX-ing into
   the freshly emitted code. If you see crashes (HardFault, runaway
   PC) after the first JIT block runs, the flush may be missing a
   barrier; check `jit_psram_flush_after_emit()` in `sh_impl.c`.
3. **clock_get_hz / DWT frequency**. The bench reports
   ticks-per-second so the host-side post-processing can convert; if
   the printed value disagrees with the actual system clock, the
   DWT cycle counter isn't running at `clk_sys` — adjust
   `sh_tickfreq()` accordingly.

When you do flash, please send me the raw `RESULT` lines from both
runs and I'll fill in the comparison table at the bottom of this
README.

## Comparison table (to be filled in after first physical run)

| Workload | SRAM jit-on | PSRAM jit-on | Δ | SRAM jit-warm | PSRAM jit-warm | Δ |
| --- | --- | --- | --- | --- | --- | --- |
| arith     | _pending_ | _pending_ | _pending_ | _pending_ | _pending_ | _pending_ |
| memmix    | _pending_ | _pending_ | _pending_ | _pending_ | _pending_ | _pending_ |
| calltree  | _pending_ | _pending_ | _pending_ | _pending_ | _pending_ | _pending_ |
| countloop | _pending_ | _pending_ | _pending_ | _pending_ | _pending_ | _pending_ |
| nqueens   | _pending_ | _pending_ | _pending_ | _pending_ | _pending_ | _pending_ |
