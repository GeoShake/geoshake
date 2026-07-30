<!--
GeoShake DIY seismometer — write-up
Copyright (C) 2024-2026 Appflows Teknoloji A.S.
Text: CC BY-SA 4.0 · Code in this directory: GPL-3.0-or-later

Every measurement in this file was taken on real hardware. A photograph of the
breadboard is still to come — the wiring diagram covers it in the meantime.
-->

# Build a seismometer that actually detects earthquakes

**~$25, one evening, no soldering.** And when you're done, it runs the same
detection algorithm as a real seismic station — verified against that station's
own test vectors, on your hardware.

That last part is the difference between this and most "earthquake sensor"
projects. Plenty of tutorials read an accelerometer and print numbers. Detecting
an *earthquake* is a different problem, and the answer is a specific algorithm
called STA/LTA. We publish ours under the GPL, so you can run the real thing
instead of an approximation — and prove you did.

## Let's be straight about what this is

**What you get:** a device that feels the ground, decides when the shaking is
real, and shows you the waveform around each detection — including the moment
*before* it fired.

**What you don't get:** a calibrated instrument, a validated event, or a warning
you can rely on. One sensor on a breadboard cannot tell a truck from a tremor
with any confidence, and nothing here is checked against other stations. If a
notification wakes your family at 3am, it should come from something better than
this. (More on that at the end — no bait, it's a real distinction.)

Build it to understand how early warning works. That is a genuinely good reason.

## The physics, in 90 seconds

An earthquake sends out two waves that travel at different speeds:

- **P-wave** — fast (~6 km/s), arrives first, usually harmless. A sharp little jolt.
- **S-wave** — slower (~3.5 km/s), arrives second, does the damage.

Both start at the same instant from the same place. Because one is faster, a gap
opens up between them that grows with distance. 50 km out, that gap is around
7 seconds. **That gap is the entire concept of earthquake early warning.** Detect
the P-wave, and you have until the S-wave arrives to do something about it.

So the job is: notice the P-wave, fast, without crying wolf every time a door
slams.

### STA/LTA: how the decision gets made

The classic answer, used by real seismic networks, is a ratio of two moving
averages of signal energy:

- **STA** — Short Term Average, ~0.5 s. Reacts fast. "What's happening right now?"
- **LTA** — Long Term Average, ~30 s. Reacts slowly. "What's normal around here?"

Divide one by the other. Quiet room: both similar, ratio ≈ 1. Something starts
shaking: STA shoots up while LTA barely moves, and the ratio spikes. Cross a
threshold — we use 6 — and you have a detection.

The beauty is that it's **self-calibrating**. A noisy workshop and a quiet
basement have wildly different absolute vibration levels, but in both places the
ratio sits near 1 when nothing is happening. You don't tune it per room; the LTA
learns the room for you.

Three details separate a working implementation from a toy:

1. **Warm-up.** The LTA needs ~30 s of data before it means anything. Trigger
   before that and you'll fire on your own power-up transient.
2. **LTA freeze.** Once triggered, stop updating the LTA. Otherwise a long
   earthquake raises its own baseline and de-triggers itself mid-shaking.
3. **Hysteresis.** Re-arm at a lower ratio (2) than you trigger at (6), or one
   event produces fifty detections.

All three are in the code you're about to flash.

## What you need

| Part | Notes | ~Price |
|---|---|---|
| ESP32-S3 dev board | Any S3 board. DevKitC, XIAO ESP32-S3, etc. | $8–12 |
| LSM6DS3 breakout | Or LSM6DS3TR-C, or LSM6DSO. SparkFun, Seeed Grove, generic clones | $10–13 |
| Jumper wires | 4 per sensor | $1 |
| Breadboard | Optional but easier | $3 |
| **Second sensor** | **Optional — but it measurably lowers your noise floor (1.40× in our test). Best $12 in the build.** | +$10–13 |

Any 6-axis IMU from ST's LSM6 family works: the driver accepts WHO_AM_I values
`0x69` (LSM6DS3), `0x6A` (LSM6DS3TR-C) and `0x6C` (LSM6DSO). We used two LSM6DS3.

> **Why an accelerometer and not a "real" geophone?** Cost, mostly. A research
> seismometer resolves ground motion a thousand times finer than this. But
> dense beats precise for early warning: twenty cheap sensors spread across a
> city catch a P-wave sooner than one expensive one across town, because the
> race is against distance, not against noise.

## Wiring

We use **SPI**, not I²C. Two reasons: multiple sensors just need one extra
chip-select line each (instead of fighting over I²C addresses), and the timing
is deterministic enough to sample several sensors inside one sample period.

| Sensor pin | ESP32-S3 | Note |
|---|---|---|
| VDD / VIN | 3V3 | not 5V |
| GND | GND | the single most common failure is a missing ground |
| SCL / SPC | GPIO 12 | SPI clock |
| SDA / SDI | GPIO 11 | MOSI — yes, the pin says SDA; in SPI mode it's data-in |
| SDO / SA0 | GPIO 13 | MISO |
| CS | GPIO 9 | sensor 1 |
| CS | GPIO 10 | sensor 2 (if you have one) |

Both sensors share SCLK, MOSI and MISO. Only CS is unique per sensor.

![Wiring diagram: two LSM6DS3 sensors sharing power, clock and data with an ESP32-S3, each with its own chip-select line](assets/wiring.svg)

Running a single sensor? Drop everything in the #2 column and change one line in
`geoshake_diy.ino`:

```cpp
static const int CS_PINS[] = {9};   // was {9, 10}
```

> ⚠️ **The trap that will cost you an hour.** On these parts, the CS pin also
> selects the protocol: held high the sensor speaks I²C, pulled low it speaks
> SPI. A sensor wired for SPI is therefore **completely invisible to an I²C
> scanner** — you get a clean "no devices found" from perfectly good hardware.
> If you're unsure how yours is wired, flash `firmware/spi_probe` — it sweeps
> the likely MISO pins and tells you exactly what answered where.


## Flashing it

```bash
git clone <repo>
cd diy/firmware

arduino-cli compile --fqbn esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc geoshake_diy
arduino-cli upload -p /dev/cu.usbmodemXXXX --fqbn esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc geoshake_diy
```

Then open the serial monitor at **115200**.

> ⚠️ **Second trap.** If you're plugged into the S3's *native* USB port (the one
> that shows up as `usbmodem`), you need `USBMode=hwcdc,CDCOnBoot=cdc` — in the
> Arduino IDE that's **Tools → USB CDC On Boot: Enabled**. Without it the sketch
> runs fine but `Serial` output never reaches USB, and you stare at an empty
> monitor wondering what you broke. Nothing. It's this setting.

## Verify your build — the part most projects skip

Before trusting a detector, prove its maths is right.

The reference implementation ships with **golden vectors**: fixed input
sequences with known outputs, locked to full float precision. They're what keeps
the production firmware honest. Your build runs them at boot:

```
GeoShake DIY seismometer
========================
Golden vector self-test ... PASS
  -> bit-compatible with the reference DSP, on this chip
```

If that says PASS, your STA/LTA produces **bit-identical results** to the
algorithm running on real GeoShake stations. Not "similar" — identical, to the
last digit. If it fails, the firmware refuses to run, because a detector you
can't trust is worse than no detector.

You can also run them on your PC, without any hardware:

```bash
cd firmware/test
c++ -std=c++11 -O2 -I../geoshake_diy golden_host_test.cpp -o golden_test && ./golden_test
```

```
golden 1: identical sensors        PASS
   sta   got 0.00024528734454568979
         want 0.00024528734454568979
golden 2: differing sensors        PASS
behaviour suite                    PASS
ALL PASS — bit-compatible with the GeoShake reference DSP.
```

Worth knowing: the ESP32-S3's FPU is single-precision, so the `double` maths
here is emulated in software — and it *still* matches exactly. That's why the
code says, in two places, don't switch to `float`. It would be faster and it
would silently stop matching.

## What you should see

After boot, ~30 seconds of warm-up while the LTA fills:

```
[warmup] n=569   rate=104.0 Hz  ratio=6.51  noise=0.00332  triggers=0
[warmup] n=1089  rate=103.6 Hz  ratio=3.30  noise=0.00438  triggers=0
[warmup] n=2025  rate=103.6 Hz  ratio=1.97  noise=0.00541  triggers=0
```

`noise` is √LTA — the RMS of your background vibration, in m/s². **This is your
noise floor, measured.** Ours settled around 0.009 m/s² on a desk in a quiet
room — and climbed past 0.04 m/s² when something in the building started
vibrating. A concrete floor will read lower; a desk next to a road, higher.

Once it says `[armed]`, knock on the desk:

```
*** SHAKING DETECTED ***
  trigger #3
  STA/LTA  12.02  (threshold 6.0)
  PGA      0.2489 m/s^2
```

Our test run: **150 seconds, 7 triggers, zero false positives.** Ratios between
6.18 and 12.02, PGA between 0.064 and 0.249 m/s². In between, sitting quietly,
the ratio wandered between 0.3 and 1.4 — nowhere near the threshold. That margin
is what a working detector looks like.

## Reading the waveform dump — where early warning becomes visible

After each trigger the firmware dumps ~2 seconds of buffered waveform as CSV.
Because it's a ring buffer, the dump contains the moments **before** the trigger
fired. Paste it into any plotting tool. Here's the STA/LTA profile from our
strongest detection:

```
i= 99   ratio=  0.86   quiet
i=104   ratio=  0.82   quiet
i=109   ratio= 12.02   ← TRIGGER FIRED
i=119   ratio= 50.78
i=136   ratio= 72.13   ← peak of the shaking
```

Look at the gap: the alarm fired at sample 109, the shaking peaked at sample
136. **27 samples — about 0.26 seconds — of advance notice**, on a knock, where
the P-wave and S-wave are essentially on top of each other.

In a real earthquake that same gap is the P-to-S separation, and it grows with
distance from the epicentre: seconds, sometimes tens of seconds. The mechanism
you're watching in that CSV is exactly the mechanism behind every earthquake
early warning system in the world. Yours just has a smaller gap to work with.

`tools/plot_waveform.py` turns any dump into this picture — no dependencies, it
just writes an SVG:

```bash
python3 tools/plot_waveform.py serial.log -o waveform.svg
```

![A knock, detected — the alarm fires 0.26 s before the shaking peaks](assets/waveform-trigger.svg)

Read it top to bottom: the three acceleration traces are quiet, then the knock
arrives. The red line is the moment STA/LTA crossed 6 and the detector fired.
The dashed line is where the shaking actually peaked. **The shaded band between
them is warning time** — the whole reason any of this exists.

Across our seven triggers the lead ranged from 0.26 s to 1.01 s, depending on
how sharply the knock started. A gentle onset gives the detector more room; a
sudden bang gives it less. Real P-waves are, helpfully, on the gentle side.

## Tuning: the parameter maths

The constants assume a ~109 Hz sample rate. The firmware prints the rate it
actually measures — ours ran at a rock-steady **103.6 Hz**. If yours differs a
lot, here's how the numbers are derived:

```
alphaHp       = 2π · fc / fs        high-pass coefficient, fc ≈ 0.2 Hz
nSta          = 0.5 s · fs          short window, in samples
nLta          = 30 s  · fs          long window, in samples
warmupSamples = nLta                don't trigger before the LTA is full
```

The high-pass is what removes gravity. Your sensor reads ~9.81 m/s² on one axis
constantly; without removing that DC component, the "energy" would be dominated
by the Earth simply existing.

**But don't change them casually.** The golden vectors encode these exact
constants — change one and the self-test fails, and you lose the ability to say
your build matches the reference. A 5% rate difference (like our 103.6 vs 109)
shifts the windows by 5%: a 0.53 s STA instead of 0.50 s. Harmless. Leave it.

## Two sensors: array gain, measured

Here's the part worth the extra $12.

Add a second sensor and the firmware averages them. Sensor noise is
*uncorrelated* — the two parts' errors are unrelated, so averaging cancels some
of it, by roughly √2. Ground motion is *correlated* — both sensors feel the same
shake, so averaging preserves it fully.

Better signal-to-noise, from nothing but arithmetic. The averaging happens
inside the reference DSP itself, which expects four sensors; with two, each
fills two slots, and the maths is untouched.

### We measured it

`firmware/array_gain` runs **three detectors in parallel on the same samples**:
sensor 0 alone, sensor 1 alone, and both averaged. Running them simultaneously
matters — background vibration changes minute to minute, so measuring one after
the other would compare different rooms rather than different sensor counts.

Flash it, leave the desk alone for seven minutes, and read the result. Ours, over
30 clean windows:

```
  single-sensor noise floor : 0.009295 m/s^2
  two-sensor  noise floor   : 0.006638 m/s^2
  measured array gain       : 1.402x   (theory for uncorrelated noise: 1.414x)
  clean windows averaged    : 30
  discarded (room moved)    : 0
```

**1.40× lower noise floor, for one extra $12 part** — within 1% of the
theoretical maximum. Consecutive windows read 1.400, 1.397, 1.399, 1.400, 1.400,
1.398, 1.393: that stability is what a settled measurement looks like.

### Getting this wrong taught us more than getting it right

Our first two attempts gave 1.35×, and we nearly published that number with a
confident physical explanation for the shortfall. Both runs were contaminated —
once by someone bumping the desk, once by a vibration source that appeared
mid-measurement and pushed the noise floor from 0.012 to 0.045 m/s².

In the quiet run the noise floor dropped to 0.0093 m/s² and the gain rose to
1.402×. So the shortfall was never a deep truth about correlated motion in the
building — it was **the room**, and the fix was to actually be quiet.

That said, the underlying physics is real and it's why the contamination hurt:
averaging only cancels noise that is **uncorrelated** between the two parts.
Their own electronic noise is uncorrelated, so it drops by √2. A vibrating desk
is *correlated* — both sensors see it identically, so averaging preserves it,
exactly as it preserves an earthquake. A noisy room therefore doesn't just raise
your noise floor, it also **caps how much a second sensor can help**. Watching
the gain fall from 1.40 toward 1.15 as the room got busier is that effect,
measured.

Two practical consequences:

- **Your measured gain is a quietness score.** Close to 1.41 means what's left
  is mostly sensor noise. Down near 1.1 means your surroundings dominate — move
  the sensors to a concrete floor, against a wall, away from anything with a
  motor in it.
- **Don't trust a run with discards.** The sketch flags any window where the
  STA/LTA ratio crosses 2.5 and leaves it out of the average. If more than a
  couple get discarded, the number is not yet real. (Its blind spot: *continuous*
  vibration raises the LTA too, so the ratio stays low and the window passes.
  A noise floor that drifts upward across the run is the giveaway — watch it.)

This is also the honest answer to a question you might reasonably ask: why does a
commercial station bother with four accelerometers when one detects shaking
fine? Because the noise floor decides how small an event you can see at all, and
array gain is the cheapest way to lower it. Four sensors buy 2× over one, in the
same arithmetic — which is why the T1 has four.

## The honest limits

Everything above is real. So is this list.

Your build **cannot**:

- **Tell an earthquake from a truck.** It detects *shaking*. Classification needs
  either a much better noise floor or corroboration from other stations. You'll
  get triggers from footsteps, doors, and the washing machine.
- **Confirm anything.** A single station has no way to know if a signal is local
  or regional. Real networks confirm an event by seeing it at several stations
  with arrival times consistent with a common epicentre. One sensor can't.
- **Measure absolute ground motion accurately.** Nothing here is calibrated. PGA
  numbers are indicative, not metrological.
- **Warn you reliably.** No redundancy, no watchdog, no signed updates. If the
  Wi-Fi drops or the sketch hangs, nothing tells you it stopped.

None of this is a flaw in the build. It's what separates a learning instrument
from a monitoring instrument, and pretending otherwise would waste your time.

## If you want the real thing

We build [GeoShake T1](https://geoshake.org/geoshake-t1), a home seismic station
— and everything you just flashed comes from its firmware, which is open source.

Here's the honest comparison:

| | This build (~$25) | GeoShake T1 (€199) |
|---|---|---|
| Detects shaking | ✅ | ✅ |
| Same STA/LTA algorithm | ✅ identical code | ✅ |
| Sensors | 1–2 LSM6DS3 | 4× LSM6DSO, array gain |
| Calibrated noise floor | ❌ | ✅ per-unit calibration |
| Network validation | ❌ | ✅ multi-station confirmation |
| **Confirmed events** | ❌ | ✅ |
| Early-warning notifications | ❌ | ✅ 5–20 s, to your phone |
| Signed OTA updates | ❌ | ✅ RSA-3072, A/B rollback |
| Live on the world map | ❌ | ✅ |
| Science mode / jAmaSeis | ❌ | ✅ raw waveform streaming |
| Enclosure | 3D print ours (CC BY-SA) | injection moulded, tested |

**The DIY build teaches you the physics. The T1 gives you the network.** What
you're buying isn't the hardware — it's validation and the other stations.

The app is free either way: [geoshake.org/download](https://geoshake.org/download).
Watch the live network at [stations.geoshake.org](https://stations.geoshake.org).

> 📬 **Built it? Get the parameter calculator.** Drop your email at
> [geoshake.org/diy](https://geoshake.org/diy) and we'll send the calculator that
> computes `alphaHp` / `nSta` / `nLta` for any sample rate, a build-log template —
> and a maker discount code, in case you ever want a T1.

## What else is in here

| Sketch / tool | What it's for |
|---|---|
| `firmware/geoshake_diy` | The detector. This is the one you want. |
| `firmware/webui` | The detector **plus a live dashboard the device serves itself** — see below. |
| `firmware/showcase` | Same detector, drawn as a live dashboard with a moving STA/LTA bar. Needs a real terminal (`screen`, iTerm) — the Arduino IDE monitor can't render it. Nice for demos and filming. |
| `firmware/array_gain` | Measures what a second sensor actually buys you. |
| `firmware/spi_probe` | Finds your wiring when the sensor doesn't answer. |
| `firmware/i2c_scan` | Same, for I²C wiring. |
| `firmware/test` | Golden vectors on your PC, no hardware needed. |
| `tools/plot_waveform.py` | CSV dump → SVG plot. No dependencies. |
| `tools/wiring_diagram.py` | Regenerates the wiring diagram. Change a pin in one dict, the picture follows. |
| `tools/calculator.html` | Offline STA/LTA parameter calculator. |

## The web dashboard

Flash `firmware/webui` instead and the device serves its own interface — live
waveform, detector state, noise floor, and every detection with a downloadable
waveform. No app, no cloud, no account.

![The device's own dashboard: live three-axis ground motion, STA/LTA detector, and a table of detections](assets/web-dashboard.png)

That screenshot is the real thing, caught mid-detection — the badge reads
DETECTING because someone knocked on the desk while the page was open. It works
on a phone too ([narrow layout](assets/web-dashboard-mobile.png)).

**First boot** has no Wi-Fi saved, so the device opens its own access point
called **GeoShake-DIY**. Join it, open `http://192.168.4.1`, enter your network,
and it restarts and connects. After that it's at `http://geoshake-diy.local`
(the serial monitor also prints the IP).

A few things it does deliberately:

- **Sampling runs on its own core.** The detector sits in a high-priority task
  pinned to core 1; Wi-Fi and HTTP live on core 0. A page load can take tens of
  milliseconds, which at 104 Hz would drop samples — and a detector that stops
  detecting while someone opens a browser tab isn't a detector.
- **No libraries to install.** WiFi, WebServer, Preferences and mDNS all ship
  with the ESP32 core. The page is plain HTML/CSS/JS served from flash.
- **It works with no internet.** Your router doesn't need a WAN connection for
  this to work. A seismic station that needs the cloud to show you its own data
  is the wrong design.
- **Every detection is downloadable** as the same CSV the serial dump produces,
  so `tools/plot_waveform.py` works on it directly.

Costs about 300 KB of flash over the plain build (74% used, so there's room).

## Troubleshooting

| Symptom | Cause |
|---|---|
| Serial monitor completely empty | USB CDC On Boot not enabled. See the flashing section. |
| "NONE FOUND", I²C scan also empty | Sensor is in SPI mode (CS pulled low) — an I²C scan can't see it. Run `spi_probe`. |
| `spi_probe` finds nothing | Ground. Then SCLK/MOSI swapped. Then MISO on an unlisted pin. |
| Golden self-test FAILS | `float` snuck in somewhere, or `-ffast-math` is on. |
| Triggers constantly | Sensor on something that vibrates. Move it to the floor, ideally against a wall. |
| Never triggers | Still warming up (~30 s), or knock harder — you need ratio ≥ 6. |
| Rate reads far from ~104 Hz | Check `CTRL1_XL` is `0x40`. Recompute the constants (see tuning). |

## Licence and provenance

Code in this directory: **GPL-3.0-or-later**, because it derives from the
GeoShake firmware's published DSP reference (`firmware/shared/src/stalta.ts`).
This write-up: **CC BY-SA 4.0**.

The T1's PCB and schematics are **not** published — so the T1 as a whole is not
open-source hardware, and we don't describe it that way. The firmware, the DSP
reference, the enclosure and the data are open for real. What's open is open;
what isn't, we say so.
