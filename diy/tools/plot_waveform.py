#!/usr/bin/env python3
# GeoShake DIY — waveform plotter
# Copyright (C) 2024-2026 Appflows Teknoloji A.S.
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Turns a trigger's CSV dump into an SVG showing the thing that matters: the
# alarm fires BEFORE the shaking peaks. That gap is early warning, and it is far
# more convincing as a picture than as a sentence.
#
# No dependencies. Pure Python, writes SVG. Deliberate: "pip install matplotlib"
# is a wall for a lot of people, and vector output stays sharp in a README, on a
# slide, and in a video frame.
#
# Usage:
#   # straight from a captured serial log — finds the dumps for you
#   python3 plot_waveform.py serial.log -o waveform.svg
#
#   # a specific dump, if the log has several (default: the strongest)
#   python3 plot_waveform.py serial.log --dump 2 -o waveform.svg
#
#   # or a bare CSV file / stdin
#   python3 plot_waveform.py dump.csv -o waveform.svg
#   arduino-cli monitor -p /dev/cu.usbmodemXXXX | python3 plot_waveform.py - -o w.svg
#
# Options:
#   --fs 103.6        sample rate used for the time axis (default 103.6)
#   --threshold 6.0   trigger threshold drawn as a reference line
#   --title "..."     override the title

import argparse
import math
import sys

# Categorical palette — validated for colour-vision deficiency separation
# (OKLab dE >= 8 between adjacent pairs) against both surfaces.
LIGHT = {
    "surface": "#fcfcfb", "ink": "#1d1d1f", "muted": "#6e6e73", "faint": "#c8c8c6",
    "grid": "#e6e6e4", "x": "#2E6FD9", "y": "#E8710A", "z": "#0C9C92",
    "ratio": "#1d1d1f", "trigger": "#C0392B", "peak": "#6e6e73", "gap": "#F4B942",
}
DARK = {
    "surface": "#1a1a19", "ink": "#f0f0ee", "muted": "#9a9a97", "faint": "#4a4a48",
    "grid": "#2c2c2a", "x": "#4A87E0", "y": "#D97316", "z": "#149E93",
    "ratio": "#f0f0ee", "trigger": "#E05B4A", "peak": "#9a9a97", "gap": "#F4B942",
}

W, H = 900, 560
PAD_L, PAD_R, PAD_T = 70, 30, 84
ACC_H, GAP_H, RATIO_H = 210, 54, 150


def parse_rows(text):
    """Pull (i, ax, ay, az, ratio) rows out of a log or bare CSV.

    In a serial log, only rows between the DUMP markers count — status lines
    and warm-up chatter must not leak in, or the ratios come out nonsense. If
    the input has no markers at all it is treated as one bare CSV.
    """
    dumps, cur, in_dump, saw_marker = [], [], False, False
    for line in text.splitlines():
        line = line.strip()
        if "WAVEFORM DUMP" in line:
            saw_marker, in_dump, cur = True, True, []
            continue
        if "END DUMP" in line:
            if cur:
                dumps.append(cur)
            in_dump, cur = False, []
            continue
        if saw_marker and not in_dump:
            continue
        parts = line.split(",")
        if len(parts) != 5:
            continue
        try:
            row = (int(parts[0]), float(parts[1]), float(parts[2]),
                   float(parts[3]), float(parts[4]))
        except ValueError:
            continue          # header line, or prose that happens to have commas
        cur.append(row)
    if cur:
        dumps.append(cur)
    return [d for d in dumps if len(d) > 10]


def pick_dump(dumps, index):
    if not dumps:
        sys.exit("No waveform data found. Expected CSV rows: i,ax,ay,az,ratio")
    if index is not None:
        if not 1 <= index <= len(dumps):
            sys.exit(f"--dump {index} out of range (found {len(dumps)})")
        return dumps[index - 1], index
    best = max(range(len(dumps)), key=lambda k: max(r[4] for r in dumps[k]))
    return dumps[best], best + 1


def nice_step(span, target_ticks=5):
    """Round a raw tick spacing up to 1/2/2.5/5 x a power of ten.

    Must handle sub-1 spans — accelerations here are hundredths of a m/s^2, and
    getting the magnitude wrong buries the axis under dozens of labels.
    """
    if span <= 0:
        return 1.0
    raw = span / target_ticks
    mag = 10.0 ** math.floor(math.log10(raw))
    for m in (1, 2, 2.5, 5):
        if raw <= mag * m:
            return mag * m
    return mag * 10


def esc(s):
    return (s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;"))


def build_svg(rows, fs, threshold, title):
    n = len(rows)
    t = [i / fs for i in range(n)]
    ax = [r[1] for r in rows]
    ay = [r[2] for r in rows]
    az = [r[3] for r in rows]
    ratio = [r[4] for r in rows]

    # Remove the DC (gravity) so all three axes share a readable scale — the
    # detector high-passes internally for exactly the same reason.
    def centred(v):
        m = sum(v) / len(v)
        return [x - m for x in v]
    ax, ay, az = centred(ax), centred(ay), centred(az)

    trig_i = next((i for i, v in enumerate(ratio) if v >= threshold), None)
    peak_i = max(range(n), key=lambda i: ratio[i])

    plot_w = W - PAD_L - PAD_R
    acc_top = PAD_T
    acc_bot = acc_top + ACC_H
    ratio_top = acc_bot + GAP_H
    ratio_bot = ratio_top + RATIO_H

    amax = max(max(abs(v) for v in ax), max(abs(v) for v in ay),
               max(abs(v) for v in az), 1e-6) * 1.15
    rmax = max(max(ratio), threshold) * 1.12

    def sx(i):
        return PAD_L + (i / max(n - 1, 1)) * plot_w

    def sy_acc(v):
        return acc_top + (1 - (v + amax) / (2 * amax)) * ACC_H

    def sy_ratio(v):
        return ratio_bot - (v / rmax) * RATIO_H

    def path(vals, mapper):
        return "M" + " L".join(f"{sx(i):.1f},{mapper(v):.1f}" for i, v in enumerate(vals))

    o = []
    add = o.append
    add(f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W} {H}" width="{W}" height="{H}" '
        f'font-family="-apple-system, BlinkMacSystemFont, Segoe UI, Roboto, sans-serif">')

    # Theme via CSS custom properties so one file serves light and dark.
    add("<style>")
    add(":root{" + ";".join(f"--{k}:{v}" for k, v in LIGHT.items()) + "}")
    add("@media (prefers-color-scheme: dark){:root{"
        + ";".join(f"--{k}:{v}" for k, v in DARK.items()) + "}}")
    add(".s{fill:var(--surface)}.ink{fill:var(--ink)}.mut{fill:var(--muted)}")
    add(".grid{stroke:var(--grid);stroke-width:1}")
    add(".axis{stroke:var(--faint);stroke-width:1}")
    add(".ln{fill:none;stroke-width:2;stroke-linejoin:round;stroke-linecap:round}")
    add(".lx{stroke:var(--x)}.ly{stroke:var(--y)}.lz{stroke:var(--z)}")
    add(".lr{stroke:var(--ratio);stroke-width:2.5}")
    add(".trig{stroke:var(--trigger);stroke-width:2}")
    add(".peak{stroke:var(--peak);stroke-width:1.5;stroke-dasharray:4 3}")
    add(".thr{stroke:var(--trigger);stroke-width:1;stroke-dasharray:5 4;opacity:.55}")
    add(".gapfill{fill:var(--gap);opacity:.16}")
    add(".t-title{font-size:17px;font-weight:700}")
    add(".t-sub{font-size:12.5px}.t-lbl{font-size:11.5px}.t-ax{font-size:11px}")
    add(".t-note{font-size:12px;font-weight:600}")
    add("</style>")

    add(f'<rect class="s" width="{W}" height="{H}"/>')
    add(f'<text class="ink t-title" x="{PAD_L}" y="28">{esc(title)}</text>')
    sub = f"{n} samples at {fs:.1f} Hz · trigger threshold {threshold:g}"
    add(f'<text class="mut t-sub" x="{PAD_L}" y="47">{esc(sub)}</text>')

    # early-warning band: trigger -> peak
    if trig_i is not None and peak_i > trig_i:
        x0, x1 = sx(trig_i), sx(peak_i)
        add(f'<rect class="gapfill" x="{x0:.1f}" y="{acc_top}" '
            f'width="{max(x1-x0,1):.1f}" height="{ratio_bot-acc_top}"/>')

    # ---- acceleration panel ----
    astep = nice_step(2 * amax)
    k = 0
    while k * astep <= amax:
        for sgn in ((1, -1) if k else (1,)):
            v = sgn * k * astep
            y = sy_acc(v)
            add(f'<line class="grid" x1="{PAD_L}" y1="{y:.1f}" x2="{W-PAD_R}" y2="{y:.1f}"/>')
            dec = 0 if astep >= 1 else (1 if astep >= 0.1 else 2)
            add(f'<text class="mut t-ax" x="{PAD_L-8}" y="{y+3.5:.1f}" text-anchor="end">{v:+.{dec}f}</text>')
        k += 1
    add(f'<text class="mut t-lbl" x="{PAD_L}" y="{acc_top-10}">acceleration, gravity removed (m/s²)</text>')
    add(f'<path class="ln lx" d="{path(ax, sy_acc)}"/>')
    add(f'<path class="ln ly" d="{path(ay, sy_acc)}"/>')
    add(f'<path class="ln lz" d="{path(az, sy_acc)}"/>')

    # legend (>=2 series: always present)
    lx = W - PAD_R - 170
    for idx, (lab, cls) in enumerate((("X", "x"), ("Y", "y"), ("Z", "z"))):
        cx = lx + idx * 56
        add(f'<line class="ln l{cls}" x1="{cx}" y1="{acc_top-14}" x2="{cx+16}" y2="{acc_top-14}"/>')
        add(f'<text class="mut t-ax" x="{cx+21}" y="{acc_top-10.5}">{lab}</text>')

    # ---- ratio panel ----
    rstep = nice_step(rmax)
    v = 0.0
    while v <= rmax:
        y = sy_ratio(v)
        add(f'<line class="grid" x1="{PAD_L}" y1="{y:.1f}" x2="{W-PAD_R}" y2="{y:.1f}"/>')
        add(f'<text class="mut t-ax" x="{PAD_L-8}" y="{y+3.5:.1f}" text-anchor="end">{v:g}</text>')
        v += rstep
    add(f'<text class="mut t-lbl" x="{PAD_L}" y="{ratio_top-10}">STA / LTA ratio</text>')
    yt = sy_ratio(threshold)
    add(f'<line class="thr" x1="{PAD_L}" y1="{yt:.1f}" x2="{W-PAD_R}" y2="{yt:.1f}"/>')
    add(f'<path class="ln lr" d="{path(ratio, sy_ratio)}"/>')

    # markers
    if trig_i is not None:
        x = sx(trig_i)
        add(f'<line class="trig" x1="{x:.1f}" y1="{acc_top}" x2="{x:.1f}" y2="{ratio_bot}"/>')
        add(f'<text class="t-note" x="{x+6:.1f}" y="{acc_top+15}" fill="var(--trigger)">TRIGGER</text>')
        add(f'<text class="mut t-ax" x="{x+6:.1f}" y="{acc_top+30}">ratio {ratio[trig_i]:.1f}</text>')
    xp = sx(peak_i)
    add(f'<line class="peak" x1="{xp:.1f}" y1="{acc_top}" x2="{xp:.1f}" y2="{ratio_bot}"/>')
    add(f'<text class="mut t-ax" x="{xp+6:.1f}" y="{acc_top+15}">peak {ratio[peak_i]:.1f}</text>')

    # the headline: the gap
    if trig_i is not None and peak_i > trig_i:
        lead = (peak_i - trig_i) / fs
        xm = (sx(trig_i) + sx(peak_i)) / 2
        ya = ratio_bot + 30
        add(f'<line class="trig" x1="{sx(trig_i):.1f}" y1="{ya}" x2="{sx(peak_i):.1f}" y2="{ya}"/>')
        for xx in (sx(trig_i), sx(peak_i)):
            add(f'<line class="trig" x1="{xx:.1f}" y1="{ya-4}" x2="{xx:.1f}" y2="{ya+4}"/>')
        add(f'<text class="t-note" x="{xm:.1f}" y="{ya-9}" text-anchor="middle" '
            f'fill="var(--trigger)">{lead:.2f} s of warning</text>')
        add(f'<text class="mut t-ax" x="{xm:.1f}" y="{ya+18}" text-anchor="middle">'
            f'alarm fired before the shaking peaked</text>')

    add(f'<line class="axis" x1="{PAD_L}" y1="{ratio_bot}" x2="{W-PAD_R}" y2="{ratio_bot}"/>')
    add(f'<text class="mut t-ax" x="{PAD_L}" y="{ratio_bot+16}">0 s</text>')
    add(f'<text class="mut t-ax" x="{W-PAD_R}" y="{ratio_bot+16}" text-anchor="end">{t[-1]:.2f} s</text>')
    add(f'<text class="mut t-ax" x="{PAD_L}" y="{H-10}">geoshake.org · DIY seismometer</text>')
    add("</svg>")
    return "\n".join(o)


def main():
    p = argparse.ArgumentParser(description="Plot a GeoShake DIY trigger dump as SVG.")
    p.add_argument("input", help="serial log, CSV file, or - for stdin")
    p.add_argument("-o", "--output", default="waveform.svg")
    p.add_argument("--dump", type=int, help="which dump to plot (default: strongest)")
    p.add_argument("--fs", type=float, default=103.6, help="sample rate (Hz)")
    p.add_argument("--threshold", type=float, default=6.0)
    p.add_argument("--title", default="A knock, detected — and flagged before it peaked")
    a = p.parse_args()

    text = sys.stdin.read() if a.input == "-" else open(a.input, encoding="utf-8",
                                                        errors="replace").read()
    dumps = parse_rows(text)
    rows, which = pick_dump(dumps, a.dump)

    svg = build_svg(rows, a.fs, a.threshold, a.title)
    with open(a.output, "w", encoding="utf-8") as f:
        f.write(svg)

    peak = max(r[4] for r in rows)
    trig = next((i for i, r in enumerate(rows) if r[4] >= a.threshold), None)
    peak_i = max(range(len(rows)), key=lambda i: rows[i][4])
    print(f"wrote {a.output}")
    print(f"  dump {which} of {len(dumps)} · {len(rows)} samples · peak ratio {peak:.2f}")
    if trig is not None and peak_i > trig:
        print(f"  warning lead: {(peak_i-trig)/a.fs:.3f} s ({peak_i-trig} samples)")


if __name__ == "__main__":
    main()
