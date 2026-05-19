#!/usr/bin/env python3
"""Sweep the JIT code-cache size across multiple build configurations.

For each (config, cache size) pair, rebuild bench.elf with the matching
CFLAGS_EXTRA and `--defsym=_jit_cache_size=N`, run it in QEMU, parse the
RESULT rows, and plot:

  • one curve per build configuration (jit-on, "cold" — cache flushed
    before timing)
  • one "warm" curve for the default config (cache pre-populated;
    measures steady-state throughput)
  • an interpreter baseline as a horizontal line

x-axis: cache size in KiB (log2 with explicit ticks), per the task spec.
y-axis: relative throughput (1/t normalised so interp = 1.0).
"""
import csv
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt

HERE = Path(__file__).resolve().parent
M33_DIR = str(HERE.parent)
ROOT_DIR = str(HERE.parent.parent)

# Cache sizes (KiB). Densest at the low end where the JIT bottoms out.
SIZES_KB = [0.5, 1, 2, 4, 8, 16, 32, 64, 96, 128, 192, 256]

QEMU = "qemu-system-arm"
QEMU_ARGS = [
    "-M", "mps2-an505",
    "-cpu", "cortex-m33",
    "-nographic",
    "-semihosting-config", "enable=on,target=native",
]

# macOS-only: prefer performance cores by hinting tier 0 (low-latency,
# high-throughput). The kernel maps that to Apple-Silicon P-cores. No-op
# on other platforms.
TASKPOLICY = shutil.which("taskpolicy")

BENCH_FLAGS = "-O3 -DBENCH_SKIP_CROSSCHECK=1 -DBENCH_SKIP_JIT_OFF=1"

# Build configurations. Each value is the extra CFLAGS we pass to make,
# layered on top of the project defaults. The "default" config keeps all
# the optimizations turned on; the other configs flip large subsets off
# so we can see the contribution of each lever in isolation.
CONFIGS = {
    "default": BENCH_FLAGS,
    "no block-linking": (
        BENCH_FLAGS
        + " -DJIT_OPT_BLOCK_LINK=0"
        + " -DJIT_OPT_SELF_LOOP_DIRECT_BRANCH=0"
        + " -DJIT_OPT_CB_CHAIN_NOTAKEN=0"
        + " -DJIT_OPT_PATCH_CROSS_CHAIN=0"
        + " -DJIT_OPT_RTN_INLINE_CACHE=0"
    ),
    "no inline arith": (
        BENCH_FLAGS
        + " -DJIT_OPT_INLINE_FIELD_ADD=0"
        + " -DJIT_OPT_INLINE_FIELD_SUB=0"
        + " -DJIT_OPT_INLINE_INCDEC=0"
        + " -DJIT_OPT_INLINE_DAT=0"
        + " -DJIT_OPT_INLINE_RSTK=0"
        + " -DJIT_OPT_INLINE_LC=0"
        + " -DJIT_OPT_INLINE_AZERO_COPY=0"
        + " -DJIT_OPT_INLINE_ZEROTEST=0"
    ),
}

# Stable per-config plot colours so re-runs render the same.
CONFIG_COLORS = {
    "default":          "#1f77b4",   # blue
    "no block-linking": "#ff7f0e",   # orange
    "no inline arith":  "#2ca02c",   # green
}
WARM_COLOR    = "#9467bd"            # purple — default config, warm mode
INTERP_COLOR  = "#d62728"            # red — baseline hline

RESULT_RE = re.compile(
    r"RESULT,(\w+),(\w+(?:-\w+)?),ops=(\d+),ticks=(\d+)"
)
TICKFREQ_RE = re.compile(r"tickfreq=(\d+)")
ALL_WORKLOADS = ("arith", "memmix", "calltree", "countloop", "nqueens")
# WORKLOADS is the subset summed into the plotted curves. Can be
# overridden from the command line (--workload=name); defaults to the
# full bench so prior reruns reproduce the existing plot.
WORKLOADS = ALL_WORKLOADS
TRIALS = 2


def run_make(cache_bytes: int, cflags_extra: str) -> None:
    env = os.environ.copy()
    subprocess.run(
        ["make", "-C", M33_DIR, "clean"],
        check=True, stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL, env=env,
    )
    subprocess.run(
        ["make", "-C", M33_DIR,
         f"JIT_CACHE_SIZE={cache_bytes}",
         f"CFLAGS_EXTRA={cflags_extra}"],
        check=True, stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE, env=env,
    )


def run_bench(timeout_s: float = 90.0) -> str:
    elf = str(Path(M33_DIR) / "bench.elf")
    cmd = [QEMU, *QEMU_ARGS, "-kernel", elf]
    if TASKPOLICY is not None:
        cmd = [TASKPOLICY, "-t", "0", "-l", "0", *cmd]
    proc = subprocess.Popen(
        cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
    )
    lines = []
    try:
        import select
        import time
        deadline = time.monotonic() + timeout_s
        while True:
            if time.monotonic() > deadline:
                lines.append("\n[bench timed out]\n")
                break
            r, _, _ = select.select([proc.stdout], [], [], 1.0)
            if not r:
                continue
            line = proc.stdout.readline()
            if not line:
                break
            lines.append(line)
            if "done." in line or "CROSSCHK FAIL" in line:
                break
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=2)
        except subprocess.TimeoutExpired:
            proc.kill()
    return "".join(lines)


def parse(output: str):
    """Return (tickfreq, {(workload, mode): ticks})."""
    freq = None
    m = TICKFREQ_RE.search(output)
    if m:
        freq = int(m.group(1))
    runs = {}
    for line in output.splitlines():
        m = RESULT_RE.match(line)
        if m:
            wl = m.group(1)
            mode = m.group(2)
            ticks = int(m.group(4))
            runs[(wl, mode)] = ticks
    return freq, runs


def sum_modes(runs, mode, freq):
    try:
        return sum(runs[(wl, mode)] for wl in WORKLOADS) / freq * 1000.0
    except KeyError:
        return None


def main():
    global WORKLOADS
    # Allow `--workload=name` (or just `--workload name`) to narrow the
    # plotted curves to one bench workload. Defaults to summing over all
    # five workloads.
    args = sys.argv[1:]
    i = 0
    while i < len(args):
        a = args[i]
        if a.startswith("--workload="):
            WORKLOADS = (a.split("=", 1)[1],)
        elif a == "--workload" and i + 1 < len(args):
            WORKLOADS = (args[i + 1],)
            i += 1
        i += 1
    for wl in WORKLOADS:
        if wl not in ALL_WORKLOADS:
            sys.exit(f"unknown workload: {wl}")
    print(f"[sweep] workloads in plot: {', '.join(WORKLOADS)}", flush=True)

    if shutil.which(QEMU) is None:
        sys.exit(f"qemu not found: {QEMU}")
    if TASKPOLICY is None:
        print("[sweep] note: taskpolicy not found; QEMU won't be hinted to "
              "P-cores", flush=True)

    csv_rows = ["config,cache_kb,trial,workload,mode,ticks,ms"]
    # results[config][kb] -> {"jit-on": ms, "jit-warm": ms}
    results = {cfg: {kb: {} for kb in SIZES_KB} for cfg in CONFIGS}
    interp_min_ms = None
    tickfreq = None

    for cfg_name, cflags in CONFIGS.items():
        print(f"\n[sweep] === config: {cfg_name} ===", flush=True)
        for kb in SIZES_KB:
            bytes_ = int(round(kb * 1024))
            print(f"[sweep] {cfg_name} @ {kb} KiB ({bytes_} bytes)", flush=True)
            run_make(bytes_, cflags)
            jit_on_trials, jit_warm_trials = [], []
            for trial in range(TRIALS):
                t_out = 240.0 if kb < 4 else 90.0
                output = run_bench(t_out)
                freq, runs = parse(output)
                if freq is None:
                    print(output)
                    sys.exit(f"could not parse tickfreq at {cfg_name}/{kb}")
                if tickfreq is None:
                    tickfreq = freq
                on_ms = sum_modes(runs, "jit-on", freq)
                warm_ms = sum_modes(runs, "jit-warm", freq)
                interp_ms = sum_modes(runs, "interp", freq)
                if on_ms is None or interp_ms is None:
                    print(output)
                    sys.exit(f"missing RESULT row at {cfg_name}/{kb}")
                jit_on_trials.append(on_ms)
                if warm_ms is not None:
                    jit_warm_trials.append(warm_ms)
                if interp_min_ms is None or interp_ms < interp_min_ms:
                    interp_min_ms = interp_ms
                for wl in WORKLOADS:
                    for mode in ("interp", "jit-on", "jit-warm"):
                        if (wl, mode) in runs:
                            t = runs[(wl, mode)]
                            csv_rows.append(
                                f"{cfg_name},{kb},{trial},{wl},{mode},{t},"
                                f"{t / freq * 1000.0:.4f}"
                            )
            results[cfg_name][kb]["jit-on"]   = min(jit_on_trials)
            results[cfg_name][kb]["jit-warm"] = (
                min(jit_warm_trials) if jit_warm_trials else None
            )
            warm_str = (
                f"{results[cfg_name][kb]['jit-warm']:.2f} ms"
                if results[cfg_name][kb]["jit-warm"] is not None
                else "(skipped — cache too small)"
            )
            print(
                f"[sweep]   jit-on  = {results[cfg_name][kb]['jit-on']:.2f} ms"
                f"  jit-warm = {warm_str}",
                flush=True,
            )

    csv_path = Path(M33_DIR) / "bench_jit_cache_sweep.csv"
    csv_path.write_text("\n".join(csv_rows) + "\n")
    print(f"\n[sweep] wrote {csv_path}", flush=True)

    plot(results, interp_min_ms)


def plot(results, interp_min_ms):
    fig, ax = plt.subplots(figsize=(10.0, 5.8))

    # Plot one cold-jit curve per config.
    for cfg_name in CONFIGS:
        ys = [interp_min_ms / results[cfg_name][kb]["jit-on"] for kb in SIZES_KB]
        ax.plot(
            SIZES_KB, ys,
            marker="o", linewidth=1.7, markersize=6.0,
            color=CONFIG_COLORS[cfg_name],
            label=f"JIT cold — {cfg_name}",
        )

    # Plus a warm curve for the "default" config only — at sub-2 KiB caches
    # the warm-mode warmup races a known IC churn corner case and the bench
    # skips warm timing, so the curve only spans the meaningful range.
    warm_xs = [kb for kb in SIZES_KB if results["default"][kb]["jit-warm"] is not None]
    warm_ys = [interp_min_ms / results["default"][kb]["jit-warm"] for kb in warm_xs]
    ax.plot(
        warm_xs, warm_ys,
        marker="s", linewidth=1.7, markersize=6.0,
        color=WARM_COLOR, linestyle="-",
        label="JIT warm — default (cache pre-populated)",
    )

    ax.axhline(
        1.0, linestyle="--", linewidth=1.4, color=INTERP_COLOR,
        label="Interpreter baseline (host -O3)",
    )

    ax.set_xscale("log", base=2)
    ax.set_yscale("linear")

    all_ys = [
        interp_min_ms / results[cfg][kb]["jit-on"]
        for cfg in CONFIGS for kb in SIZES_KB
    ] + warm_ys
    y_top = max(max(all_ys), 1.0) * 1.10
    ax.set_xlim(min(SIZES_KB) * 0.85, max(SIZES_KB) * 1.18)
    ax.set_ylim(0, y_top)
    ax.set_xticks(SIZES_KB)
    ax.set_xticklabels([f"{kb:g}" for kb in SIZES_KB])
    ax.minorticks_off()
    ax.set_xlabel("JIT code cache size (KiB, log2 scale)")
    ax.set_ylabel("Relative throughput, 1/t  (interpreter = 1.0)")
    if WORKLOADS == ALL_WORKLOADS:
        wl_desc = f"{len(ALL_WORKLOADS)} workloads"
    elif len(WORKLOADS) == 1:
        wl_desc = f"workload: {WORKLOADS[0]}"
    else:
        wl_desc = "workloads: " + ", ".join(WORKLOADS)
    ax.set_title(
        "Saturn JIT throughput vs. code-cache size, by configuration\n"
        "QEMU mps2-an505 on Apple-Silicon P-cores, host -O3, "
        f"{wl_desc} × 200 000 Saturn ops (best of 2)"
    )
    ax.grid(True, which="both", alpha=0.3)
    ax.legend(loc="center right", fontsize=9)
    fig.tight_layout()
    suffix = ""
    if WORKLOADS != ALL_WORKLOADS:
        suffix = "_" + "_".join(WORKLOADS)
    png = Path(ROOT_DIR) / f"bench_jit_cache_sweep{suffix}.png"
    fig.savefig(png, dpi=130)
    print(f"[sweep] wrote {png}", flush=True)


if __name__ == "__main__":
    main()
