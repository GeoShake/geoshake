#!/usr/bin/env python3
# GeoShake DIY — wiring diagram generator
# Copyright (C) 2024-2026 Appflows Teknoloji A.S.
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Draws the SPI wiring as an SVG. No dependencies, no Fritzing, and it stays
# correct: the pin numbers live in one dict below, so if the wiring changes the
# diagram changes with it instead of quietly going stale.
#
#   python3 wiring_diagram.py -o ../assets/wiring.svg
#
# Every wire carries a label as well as a colour — colour alone would exclude
# anyone with a colour-vision deficiency, and these are exactly the details you
# cannot afford to guess while wiring.

import argparse

# --- the single source of truth for the wiring -------------------------------
SHARED = [
    # (esp pin label, sensor pin label, colour key, short note)
    ("3V3", "VDD",     "pwr",  "power"),
    ("GND", "GND",     "gnd",  "ground"),
    ("12",  "SCL/SPC", "sclk", "clock"),
    ("11",  "SDA/SDI", "mosi", "→ sensor"),
    ("13",  "SDO",     "miso", "← sensor"),
]
CHIP_SELECTS = [("9", "CS", "cs1", "select #1"), ("10", "CS", "cs2", "select #2")]

# Sensor pin rows: the five shared signals, then CS as the sixth row.
SENS_ROW_0 = 62
SENS_ROW_H = 17

LIGHT = {
    "surface": "#fcfcfb", "ink": "#1d1d1f", "muted": "#6e6e73", "faint": "#c8c8c6",
    "board": "#ececeb", "boardline": "#c8c8c6", "chip": "#e3f6f4", "chipline": "#0C9C92",
    "pwr": "#C0392B", "gnd": "#5a5a5e", "sclk": "#B8860B", "mosi": "#2E6FD9",
    "miso": "#0C9C92", "cs1": "#7C4DBE", "cs2": "#E8710A", "note": "#F4B942",
}
DARK = {
    "surface": "#1a1a19", "ink": "#f0f0ee", "muted": "#9a9a97", "faint": "#4a4a48",
    "board": "#242423", "boardline": "#3a3a38", "chip": "#12302e", "chipline": "#2ECFC3",
    "pwr": "#E05B4A", "gnd": "#9a9a97", "sclk": "#D9A521", "mosi": "#4A87E0",
    "miso": "#149E93", "cs1": "#9B72E0", "cs2": "#D97316", "note": "#F4B942",
}

W, H = 900, 610
ESP_X, ESP_Y, ESP_W, ESP_H = 60, 120, 170, 330
S1_X, S1_Y = 620, 116
S2_X, S2_Y = 620, 316
CHIP_W, CHIP_H = 210, 168
ROW_H = 30


def esc(s):
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def build():
    o = []
    add = o.append
    add(f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W} {H}" width="{W}" height="{H}" '
        f'font-family="-apple-system, BlinkMacSystemFont, Segoe UI, Roboto, sans-serif">')

    add("<style>")
    add(":root{" + ";".join(f"--{k}:{v}" for k, v in LIGHT.items()) + "}")
    add("@media (prefers-color-scheme: dark){:root{"
        + ";".join(f"--{k}:{v}" for k, v in DARK.items()) + "}}")
    add(".bg{fill:var(--surface)}")
    add(".board{fill:var(--board);stroke:var(--boardline);stroke-width:1.5}")
    add(".chip{fill:var(--chip);stroke:var(--chipline);stroke-width:1.5}")
    add(".ink{fill:var(--ink)}.mut{fill:var(--muted)}")
    add(".w{fill:none;stroke-width:2.5;stroke-linecap:round}")
    add(".pad{stroke-width:1.5}")
    add(".t-title{font-size:16px;font-weight:700}")
    add(".t-sub{font-size:12.5px}")
    add(".t-board{font-size:13px;font-weight:700}")
    add(".t-pin{font-size:11.5px;font-weight:600;font-family:ui-monospace,'SF Mono',Menlo,monospace}")
    add(".t-lbl{font-size:10.5px;font-weight:600}")
    add(".t-note{font-size:11.5px}")
    for k in ("pwr", "gnd", "sclk", "mosi", "miso", "cs1", "cs2"):
        add(f".{k}{{stroke:var(--{k})}} .{k}f{{fill:var(--{k})}}")
    add("</style>")

    add(f'<rect class="bg" width="{W}" height="{H}"/>')
    add(f'<text class="ink t-title" x="40" y="42">Wiring — two LSM6DS3 sensors on one SPI bus</text>')
    add('<text class="mut t-sub" x="40" y="62">Both sensors share power, clock and data. '
        'Only the chip-select line is unique to each.</text>')

    # --- boards ---
    add(f'<rect class="board" x="{ESP_X}" y="{ESP_Y}" width="{ESP_W}" height="{ESP_H}" rx="10"/>')
    add(f'<text class="ink t-board" x="{ESP_X + 14}" y="{ESP_Y + 26}">ESP32-S3</text>')
    add(f'<text class="mut t-lbl" x="{ESP_X + 14}" y="{ESP_Y + 43}">DevKitC (any S3 board)</text>')

    for idx, (x, y, name) in enumerate(((S1_X, S1_Y, "LSM6DS3  #1"), (S2_X, S2_Y, "LSM6DS3  #2"))):
        add(f'<rect class="chip" x="{x}" y="{y}" width="{CHIP_W}" height="{CHIP_H}" rx="10"/>')
        add(f'<text class="ink t-board" x="{x + 14}" y="{y + 25}">{name}</text>')
        add(f'<text class="mut t-lbl" x="{x + 14}" y="{y + 41}">or LSM6DS3TR-C / LSM6DSO</text>')

    # --- shared wires: ESP pin -> vertical bus -> both sensors ---
    bus_x0 = ESP_X + ESP_W
    first_y = ESP_Y + 70
    for i, (esp_pin, sens_pin, key, note) in enumerate(SHARED):
        y = first_y + i * ROW_H
        bus_x = 300 + i * 26                          # each signal gets its own lane
        y1 = S1_Y + SENS_ROW_0 + i * SENS_ROW_H       # landing rows on sensor 1
        y2 = S2_Y + SENS_ROW_0 + i * SENS_ROW_H

        # ESP pad + label
        add(f'<circle class="{key}f" cx="{bus_x0}" cy="{y}" r="4"/>')
        add(f'<text class="ink t-pin" x="{bus_x0 - 12}" y="{y + 4}" text-anchor="end">{esc(esp_pin)}</text>')

        # out, down the lane, then to each sensor
        add(f'<path class="w {key}" d="M{bus_x0},{y} H{bus_x} V{y1} H{S1_X}"/>')
        add(f'<path class="w {key}" d="M{bus_x},{y1} V{y2} H{S2_X}"/>')
        add(f'<circle class="{key}f" cx="{bus_x}" cy="{y1}" r="3"/>')

        for (sx, sy) in ((S1_X, y1), (S2_X, y2)):
            add(f'<circle class="{key}f" cx="{sx}" cy="{sy}" r="3.5"/>')
            add(f'<text class="ink t-pin" x="{sx + 10}" y="{sy + 4}">{esc(sens_pin)}</text>')

        # signal name on the horizontal run out of the ESP
        add(f'<text class="{key}f t-lbl" x="{bus_x0 + 8}" y="{y - 7}">{esc(note)}</text>')

    # --- chip selects: one per sensor, landing on the sixth pin row ---
    cs_row_y = SENS_ROW_0 + len(SHARED) * SENS_ROW_H
    for i, (esp_pin, sens_pin, key, note) in enumerate(CHIP_SELECTS):
        y = first_y + (len(SHARED) + i) * ROW_H
        target_y = (S1_Y if i == 0 else S2_Y) + cs_row_y
        lane = 300 + (len(SHARED) + i) * 26

        add(f'<circle class="{key}f" cx="{bus_x0}" cy="{y}" r="4"/>')
        add(f'<text class="ink t-pin" x="{bus_x0 - 12}" y="{y + 4}" text-anchor="end">{esc(esp_pin)}</text>')
        add(f'<path class="w {key}" d="M{bus_x0},{y} H{lane} V{target_y} H{S1_X}"/>')
        add(f'<circle class="{key}f" cx="{S1_X}" cy="{target_y}" r="3.5"/>')
        add(f'<text class="ink t-pin" x="{S1_X + 10}" y="{target_y + 4}">{esc(sens_pin)}</text>')
        add(f'<text class="{key}f t-lbl" x="{bus_x0 + 8}" y="{y - 8}">{esc(note)}</text>')

    # --- the warning that saves an hour ---
    ny = 512
    add(f'<rect x="40" y="{ny}" width="{W - 80}" height="58" rx="10" fill="none" '
        f'stroke="var(--note)" stroke-width="1.5"/>')
    add(f'<text class="ink t-note" x="58" y="{ny + 21}" font-weight="700">CS also picks the protocol.</text>')
    add(f'<text class="mut t-note" x="58" y="{ny + 38}">Pulled low these parts speak SPI; held high they speak I²C.</text>')
    add(f'<text class="mut t-note" x="58" y="{ny + 52}">That is why an I²C scan finds nothing on a correctly wired SPI build — run spi_probe if in doubt.</text>')

    add(f'<text class="mut t-lbl" x="40" y="{H - 14}">geoshake.org · DIY seismometer · pin numbers are GPIO</text>')
    add("</svg>")
    return "\n".join(o)


def main():
    p = argparse.ArgumentParser(description="Generate the DIY wiring diagram as SVG.")
    p.add_argument("-o", "--output", default="wiring.svg")
    a = p.parse_args()
    with open(a.output, "w", encoding="utf-8") as f:
        f.write(build())
    print(f"wrote {a.output}")
    print(f"  {len(SHARED)} shared signals + {len(CHIP_SELECTS)} chip selects")


if __name__ == "__main__":
    main()
