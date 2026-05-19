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

## Results — SRAM (real RP2350 @ 150 MHz)

Captured by flashing `bench_sram.uf2` over a Pico Debug Probe + USB
(serial log in `bench_sml.log`):

| Workload | Interp | JIT cold | JIT warm | Warm speedup |
| --- | --- | --- | --- | --- |
| arith     | 153.8 ms | 26.0 ms | 25.4 ms | **6.06×** |
| memmix    | 159.3 ms | 34.0 ms | 33.6 ms | 4.74× |
| calltree  | 141.9 ms | 32.3 ms | 31.8 ms | 4.46× |
| countloop | 137.1 ms | 20.8 ms | 20.3 ms | **6.76×** |
| nqueens   | 137.4 ms | 25.6 ms | 24.9 ms | 5.51× |
| **total** | 729.6 ms | 138.7 ms | 136.0 ms | **5.37×** |

A few things worth noting about the SRAM numbers vs. the QEMU-modelled
numbers:

- The real RP2350 at 150 MHz runs the interpreter in 137–160 ms per
  workload (~1.4 Mops/s) — slower per saturn-op than QEMU because QEMU
  was running the interp's translated host code at host speed (1 GHz
  TCG tickfreq). Cycles-per-Saturn-op on the real M33 ≈ 17–25, on
  QEMU TCG ≈ 9–13.
- The warm JIT speedup (4.5–6.8×) is markedly *lower* than the QEMU
  number (10–22×). That's expected: on real hardware the JIT-emitted
  Thumb-2 pays real memory-access cycles that QEMU's TCG translation
  collapses into single host ops. The interpreter pays them too, so
  the *ratio* shrinks even though the absolute JIT throughput
  improves.

## Results — PSRAM (BLOCKED — PSRAM not responding on this board)

The PSRAM build (`bench_psram.uf2`) flashes and runs the QMI direct-
mode bring-up successfully but the chip on QMI CS1 (GPIO 8 per the
Adafruit specification) is not echoing back what we write. Partial
serial log in `bench_psram_partial.log`:

```
psram init: CS pin GPIO8, base 0x11000000
psram: GPIO8 set to XIP_CS1 (func 9)
psram: pre-init direct_csr=0x00c10802
psram: direct-mode init done, M1 configured for QPI XIP
psram first read: 0x00000000              <-- expected 0x?? (uninitialised PSRAM contents)
PSRAM mismatch at 0x11000000: got 0x00000000, want 0xa5a5a500
... 1024/1024 mismatches ...
RESULT,arith,interp,...               <-- interp still works (doesn't touch PSRAM)
[then the JIT writes blocks into non-storing PSRAM, BXs into them,
 lands in zero-filled code, eventually HardFault → LOCKUP]
```

Two pieces of plumbing are in place and verified by the SRAM build:

1. The JIT-cache buffer placement: a fixed `uint8_t *` pointing into
   the PSRAM XIP window at `0x11000000`.
2. The QMI bring-up itself runs from SRAM (`.time_critical.*`
   section), avoids any flash function calls while
   `DIRECT_CSR.EN=1`, and successfully writes the M1 control
   registers. We can see the QMI clocking out commands.

The remaining unknowns are *physical*: either the CS pin is not in
fact GPIO 8 on this exact board variant, or the chip needs different
power-up timing, or some board-specific quirk we haven't seen. Both
SPI-mode (Fast Read `0x0B`) and QPI-mode (`0xEB`) configurations were
tried with the same result; the QPI mode reads garbage (0x55-ish
pattern, MISO drifting), the SPI mode reads consistent zeros (MISO
held low — suggestive of an inactive chip on the bus).

To unblock, the next debug step is to confirm with a logic analyzer
which GPIO actually goes low when the QMI tries a CS1 transaction;
or to compare against a working PSRAM init for this exact Adafruit
variant (the Pimoroni Pico Plus 2 uses GPIO 47, the WeAct Studio
RP2350B uses GPIO 0 — neither covers the Adafruit 6130 in the SDK
2.2 board files).

Once PSRAM is responding, the same `cmake --build build_psram` →
`picotool load` flow captures a `bench_psram.log` and
`compare.py` produces the side-by-side plot + table.
