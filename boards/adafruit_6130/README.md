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
(serial log in `bench_sram.log`):

| Workload | Interp | JIT cold | JIT warm | Warm speedup (vs RP2350 interp) |
| --- | --- | --- | --- | --- |
| arith     | 153.8 ms | 26.0 ms | 25.4 ms | **6.06×** |
| memmix    | 159.3 ms | 34.0 ms | 33.6 ms | 4.74× |
| calltree  | 141.9 ms | 32.3 ms | 31.8 ms | 4.46× |
| countloop | 137.1 ms | 20.8 ms | 20.3 ms | **6.76×** |
| nqueens   | 137.4 ms | 25.6 ms | 24.9 ms | 5.51× |
| **total** | 729.6 ms | 138.7 ms | 136.0 ms | **5.37×** |

### Original HP 48GX comparison — N-queens

The same N-queens Saturn-assembly program (the one this repo's
`workload_nqueens` bytewise emulates, modulo the leading `SAVPTR` /
trailing `PUSHhxs+PUSH#ALOOP` ROM hooks that the bench replaces with a
restart) runs in **336 ms on a stock HP 48GX** (2 MHz Saturn, original
1990 hardware). Same Saturn ops, same algorithm, different host:

| Host | nqueens time | vs. HP 48GX | Notes |
| --- | --- | --- | --- |
| **HP 48GX** (2 MHz Saturn, native) | 336 ms     | 1.0×    | Original hardware baseline |
| RP2350 @ 150 MHz, interp           | 137.4 ms   | **2.4×** | C interpreter (`-O3`), no JIT |
| RP2350 @ 150 MHz, JIT cold         | 25.6 ms    | **13.1×**| First-pass translate + execute |
| RP2350 @ 150 MHz, JIT warm         | 24.9 ms    | **13.5×**| Cache pre-populated |

So this Saturn JIT on a single Cortex-M33 core at 150 MHz is about an
**order of magnitude faster** at running native HP-48 ROM code than
the original 48GX. The interpreter alone — no JIT — is already 2.4×
faster, which is what you'd expect from a 150 MHz general-purpose
core decoding a 2 MHz BCD CPU; the JIT then folds the per-op decode
overhead away.

### Overclock — N-queens scaling

The board runs comfortably above the SDK's stock 150 MHz operating
point. Configure the OC entirely at compile time — CMake passes the
sys-clock + PLL settings + requested VREG to the SDK and
`pico_set_binary_type(copy_to_ram)` is enabled automatically when
`SYS_CLOCK_KHZ != 0`, so flash XIP timing isn't a constraint on
`sys_clk`.

Build an overclocked image with:

```
cmake -B build_oc \
    -DSYS_CLOCK_KHZ=336000 \
    -DVREG_VOLTAGE=VREG_VOLTAGE_1_20 \
    -DPLL_SYS_VCO_HZ=1344000000 -DPLL_SYS_PD1=4 -DPLL_SYS_PD2=1
```

(PLL params come from
`python3 $PICO_SDK_PATH/src/rp2_common/hardware_clocks/scripts/vcocalc.py <freq_MHz>`;
prefer configurations that give an integer FBDIV with REFDIV=1 — the
chip on this board didn't survive any of the REFDIV=2 configurations
vcocalc proposes for non-integer multiples of 12 MHz.)

Per-frequency N-queens jit-warm result (same workload, same JIT cache
config, same image except for the clock/voltage compile-time defines):

| sys_clk | nqueens jit-warm time | vs. 150 MHz | vs. HP 48GX |
| ---     | ---                   | ---         | ---         |
| 150 MHz (SDK default) | 24.95 ms          | 1.0×        | 13.5× |
| 250 MHz | 14.97 ms              | 1.67×       | 22.4× |
| 300 MHz | 12.47 ms              | 2.00×       | 26.9× |
| 324 MHz | 11.55 ms              | 2.16×       | 29.1× |
| 336 MHz | **11.14 ms**          | **2.24×**   | **30.2×** |
| 342 MHz | (lockup)              | —           | — |
| 348 MHz | (lockup)              | —           | — |
| 375 / 400 MHz | (lockup)        | —           | — |

The tick count for `jit-warm` is essentially identical across these
frequencies — same emitted Thumb-2 code path — so the speedup is the
pure clock ratio. 336/150 = 2.24× and the measurement matches to
better than 0.1 %.

This particular Adafruit Feather RP2350 (8 MB PSRAM, P/N 6130) hits
its stable ceiling at **336 MHz** (between 336 and 342 — anything
≥342 wedges the CPU into LOCKUP before USB CDC enumerates).
Recovery from lockup requires a physical BOOT-button power-cycle.

Caveat on `VREG_VOLTAGE`: I instrumented POWMAN at boot during this
work and the `SYS_CLK_VREG_VOLTAGE_AUTO_ADJUST` path does not drive
VSEL on this SDK 2.2 / Adafruit Feather build — `POWMAN_VREG` reads
back `0xb0` (VSEL=0x0b = 1.10 V default) regardless of the requested
`VREG_VOLTAGE_MIN`. Manually poking `POWMAN_VREG` from a pre-init
hook (with password + `DISABLE_VOLTAGE_LIMIT`) didn't change VSEL
either; `RST_N` in `vreg_ctrl` was load-bearing and never resolved.
So the table above is for the chip running at its default 1.10 V;
whether a higher VDD would unlock more headroom is unknown on this
hardware. The `VREG_VOLTAGE` build option still threads through to
the SDK in case the auto-adjust starts working on a later SDK
revision.

At the documented 336 MHz overclock the JIT runs N-queens **30.2×
faster than the original HP 48GX** (11.14 ms vs 336 ms) on a single
Cortex-M33 core.

### PSRAM (8 MB APS6404L over QMI CS1)

Adafruit ship the 6130 with an APS6404L on QMI CS1 (GPIO8) but the
Pico SDK 2.2 stock board file doesn't bring it up; we initialise it
ourselves in `psram_init.c`, closely following CircuitPython's
`setup_psram()` in `ports/raspberrypi/supervisor/port.c`:

1. Mux GPIO8 → `GPIO_FUNC_XIP_CS1`.
2. Drive QMI in direct mode at `clkdiv=30`, send `0xF5` as quad
   in case a previous boot left the chip in QPI mode.
3. Read JEDEC ID (`0x9F` + 6 dummies). The byte at offset 5 must be
   `0x5D` (APS6404 KGD); the EID at offset 6 encodes die size
   (`0x53` → 8 MB on this board).
4. Send `0x66` (RESETEN), `0x99` (RESET), `0x35` (ENTER_QPI) each in
   its own CS cycle.
5. Configure QMI M[1] for QPI fast read (`0xEB`, 24 dummy cycles) and
   QPI write (`0x38`). CLKDIV is computed from `clk_sys` so the
   PSRAM QSPI clock stays ≤ ~110 MHz (`sys/2` at the stock 150 MHz
   → 75 MHz; `sys/4` at the 336 MHz OC → 84 MHz). APS6404L data
   brief is 144 MHz QPI fast-read / 84 MHz standard.
6. Set `XIP_CTRL.WRITABLE_M1` so the XIP controller forwards writes
   to the PSRAM window instead of silently dropping them.

`psram_init_inner()` must live in SRAM (`__no_inline_not_in_flash_func`)
because flash XIP is unreachable while `QMI.DIRECT_CSR.EN=1`; inlining
or having `.rodata` lookup tables inside that critical window will
lock the chip up immediately.

After init, PSRAM is memory-mapped at:
* `0x11000000` — cached (use for code/data hot paths, JIT cache)
* `0x15000000` — uncached alias (skip the XIP cache for explicit
  read-after-write smoke tests)

Build with the JIT cache living in PSRAM via:

```
cmake -B build_psram -DJIT_CACHE_REGION=PSRAM
```

Combined with the 336 MHz overclock:

```
cmake -B build_psram_oc \
    -DJIT_CACHE_REGION=PSRAM \
    -DSYS_CLOCK_KHZ=336000 -DVREG_VOLTAGE=VREG_VOLTAGE_1_20 \
    -DPLL_SYS_VCO_HZ=1344000000 -DPLL_SYS_PD1=4 -DPLL_SYS_PD2=1
```

#### Per-workload SRAM vs PSRAM (full bench)

All five workloads, same image except for `JIT_CACHE_REGION`. Times
in milliseconds, derived from the captured `bench_*.log` files
(`ticks / tickfreq`).

At the stock **150 MHz**:

| workload   | interp   | SRAM jit-on | PSRAM jit-on | Δ      | SRAM jit-warm | PSRAM jit-warm | Δ      |
| ---        | ---      | ---         | ---          | ---    | ---           | ---            | ---    |
| arith      | 153.80   | 25.97       | 26.14        | +0.64% | 25.35         | 25.36          | +0.02% |
| memmix     | 159.30   | 34.04       | 34.12        | +0.24% | 33.61         | 33.61          | +0.01% |
| calltree   | 141.92   | 32.33       | 32.36        | +0.11% | 31.83         | 31.84          | +0.01% |
| countloop  | 137.11   | 20.76       | 20.83        | +0.35% | 20.28         | 20.28          | +0.00% |
| nqueens    | 137.42   | 25.64       | 25.76        | +0.47% | 24.95         | 24.95          | +0.01% |
| **total**  | **729.56** | **138.74** | **139.22** | **+0.35%** | **136.02** | **136.03** | **+0.01%** |

At the **336 MHz** overclock:

| workload   | interp   | SRAM jit-on | PSRAM jit-on | Δ      | SRAM jit-warm | PSRAM jit-warm | Δ      |
| ---        | ---      | ---         | ---          | ---    | ---           | ---            | ---    |
| arith      | 68.17    | 11.40       | 11.48        | +0.64% | 11.31         | 11.31          | +0.00% |
| memmix     | 70.95    | 15.06       | 15.12        | +0.41% | 14.99         | 15.05          | +0.40% |
| calltree   | 63.01    | 14.29       | 14.29        | +0.02% | 14.21         | 14.21          | +0.01% |
| countloop  | 60.85    | 9.14        | 9.15         | +0.12% | 9.05          | 9.06           | +0.09% |
| nqueens    | 61.23    | 11.24       | 11.35        | +0.99% | 11.14         | 11.25          | +0.99% |
| **total**  | **324.21** | **61.13** | **61.39** | **+0.43%** | **60.71** | **60.89** | **+0.30%** |

**PSRAM overhead is essentially zero** — even for `jit-on` (where
the JIT actively writes emitted code into the cache, so the QSPI
write cost is on the critical path) the slowdown is under 1 % per
workload. The cached XIP window at `0x11000000` hides the per-access
cost almost completely: the 16 KiB XIP cache fits the hot working set
between bench iterations and only cold misses pay the round-trip to
QSPI.

#### How much is the XIP cache hiding? — PSRAM with cache bypassed

Same PSRAM build, but with `-DPSRAM_NOCACHE=1` the JIT cache base
flips from `0x11000000` (cached) to `0x15000000` (uncached alias),
so every Thumb-2 instruction fetch *and* every JIT-emitted store
round-trips to PSRAM over QSPI:

| workload   | cached jit-on | no-cache jit-on | ×       | cached jit-warm | no-cache jit-warm | ×       |
| ---        | ---           | ---             | ---     | ---             | ---               | ---     |
| arith      | 26.14         | 373.21          | 14.28×  | 25.36           | 372.44            | 14.69×  |
| memmix     | 34.12         | 322.25          | 9.44×   | 33.61           | 321.75            | 9.57×   |
| calltree   | 32.36         | 510.10          | 15.76×  | 31.84           | 509.50            | 16.00×  |
| countloop  | 20.83         | 300.49          | 14.42×  | 20.28           | 299.92            | 14.79×  |
| nqueens    | 25.76         | 211.59          | 8.21×   | 24.95           | 210.79            | 8.45×   |
| **total**  | **139.22**    | **1717.64**     | **12.34×** | **136.03**  | **1714.40**       | **12.60×** |

So the 16 KiB XIP cache is doing **all** of the lifting — bypassed,
PSRAM-resident JIT code runs ~13× slower than cached, and is in fact
*slower than the interpreter from SRAM* (interp total ≈ 730 ms at
150 MHz). `calltree` widens to ~16× because its many cross-block
branches blow out cache locality faster than the other workloads;
`nqueens` only widens to ~8× because its inner loop fits inside
one or two cache lines, so re-fetching the same hot bytes from
PSRAM is the dominant cost rather than missing across blocks.

At the 336 MHz overclock the no-cache build doesn't complete: the
uncached round-trip path stops being reliable (the same write
corruption that fails the boot-time verify in the 336 MHz row above)
and the JIT branches to garbage on its first call into PSRAM,
lockup-faulting before the first `RESULT` line. The cached path
still works because the XIP cache absorbs the bad uncached writes
into its line buffer before they reach PSRAM.

The takeaway: keep the JIT cache on `0x11000000`. The "no-cache"
mode is a measurement tool, not an operating point.

That makes PSRAM essentially free to use for the JIT cache here.
The headline benefit isn't speed — it's capacity: the SRAM build
caps the JIT cache at 256 KiB without eating into the bench's other
SRAM allocations (ROM/RAM buffers, stack, USB CDC), while PSRAM
can go up to the full 8 MiB:

```
cmake -B build_psram_big \
    -DJIT_CACHE_REGION=PSRAM \
    -DJIT_CACHE_SIZE=$((8 * 1024 * 1024))
```

The PSRAM smoke-test (`psram: verify ok …`) walks 256 KiB through the
uncached alias. At the stock 150 MHz the test passes cleanly; at the
336 MHz overclock the uncached-alias verify writes don't survive
read-back (likely a sub-cycle RXDELAY/timing issue specific to the
no-cache path) — but the cached-path JIT cache works fine, since
all bench output and tick counts come back correct. So PSRAM is
usable for JIT code at the OC, just not validated by the boot-time
smoke test.

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
