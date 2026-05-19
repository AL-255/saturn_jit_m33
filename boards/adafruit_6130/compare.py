#!/usr/bin/env python3
"""Compare two bench captures from the RP2350 board (SRAM vs PSRAM
JIT cache). Each input is the raw serial-capture of a single board
run.

Usage:
    python3 compare.py bench_sram.log bench_psram.log [-o psram_vs_sram.png]

Produces:
    - a markdown table on stdout (suitable for pasting into the board
      README's "Comparison table" section)
    - a side-by-side bar chart PNG
"""
import argparse
import re
import sys
from pathlib import Path

RESULT_RE = re.compile(
    r"RESULT,(?P<wl>\w+),(?P<mode>\w+(?:-\w+)?),ops=(?P<ops>\d+),ticks=(?P<ticks>\d+)"
)
TICKFREQ_RE = re.compile(r"tickfreq=(\d+)")
REGION_RE   = re.compile(r"jit_cache_region=(\w+)")

WORKLOADS = ("arith", "memmix", "calltree", "countloop", "nqueens")
MODES     = ("interp", "jit-on", "jit-warm")


def parse(path: Path):
    text = path.read_text()
    freq = None
    region = None
    runs = {}  # (wl, mode) -> ms
    m = TICKFREQ_RE.search(text)
    if m:
        freq = int(m.group(1))
    m = REGION_RE.search(text)
    if m:
        region = m.group(1)
    for line in text.splitlines():
        m = RESULT_RE.match(line)
        if m:
            wl = m.group("wl")
            mode = m.group("mode")
            ticks = int(m.group("ticks"))
            if freq:
                runs[(wl, mode)] = ticks / freq * 1000.0  # ms
    if freq is None:
        sys.exit(f"could not find tickfreq in {path}")
    return region, runs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("sram_log", type=Path)
    ap.add_argument("psram_log", type=Path)
    ap.add_argument("-o", "--out", type=Path, default=Path("psram_vs_sram.png"))
    args = ap.parse_args()

    sram_region, sram = parse(args.sram_log)
    psram_region, psram = parse(args.psram_log)

    if sram_region != "sram":
        print(f"warning: {args.sram_log} reports region={sram_region!r}, not sram")
    if psram_region != "psram":
        print(f"warning: {args.psram_log} reports region={psram_region!r}, not psram")

    # --- markdown table ---
    print("| Workload | SRAM jit-on | PSRAM jit-on | Δ | SRAM jit-warm | PSRAM jit-warm | Δ |")
    print("| --- | --- | --- | --- | --- | --- | --- |")
    for wl in WORKLOADS:
        cells = [wl]
        for mode in ("jit-on", "jit-warm"):
            s = sram.get((wl, mode))
            p = psram.get((wl, mode))
            cells.append(f"{s:.2f} ms" if s is not None else "—")
            cells.append(f"{p:.2f} ms" if p is not None else "—")
            if s and p:
                delta = (p - s) / s * 100.0
                cells.append(f"{delta:+.1f}%")
            else:
                cells.append("—")
        print("| " + " | ".join(cells) + " |")

    # --- bar chart ---
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        print("(matplotlib not installed; skipping plot)")
        return

    import numpy as np
    fig, ax = plt.subplots(figsize=(10.5, 5.4))
    n = len(WORKLOADS)
    x = np.arange(n)
    w = 0.2

    series = [
        ("SRAM jit-on",   [sram.get((wl, "jit-on"), 0)   for wl in WORKLOADS], "#1f77b4"),
        ("PSRAM jit-on",  [psram.get((wl, "jit-on"), 0)  for wl in WORKLOADS], "#aec7e8"),
        ("SRAM jit-warm", [sram.get((wl, "jit-warm"), 0) for wl in WORKLOADS], "#2ca02c"),
        ("PSRAM jit-warm",[psram.get((wl, "jit-warm"), 0)for wl in WORKLOADS], "#98df8a"),
    ]
    for i, (label, ys, color) in enumerate(series):
        ax.bar(x + (i - 1.5) * w, ys, width=w, label=label, color=color)

    ax.set_xticks(x)
    ax.set_xticklabels(WORKLOADS)
    ax.set_ylabel("Wall time (ms, lower is better)")
    ax.set_title("Saturn JIT on Adafruit Feather RP2350 (6130)\n"
                 "JIT code cache in on-chip SRAM vs external 8 MB PSRAM")
    ax.grid(True, axis="y", alpha=0.3)
    ax.legend(loc="upper left", fontsize=9)
    fig.tight_layout()
    fig.savefig(args.out, dpi=130)
    print(f"\nwrote {args.out}")


if __name__ == "__main__":
    main()
