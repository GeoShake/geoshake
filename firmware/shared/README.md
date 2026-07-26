# @geoshake/shared

The **foundation package** of the GeoShake community Earthquake Early Warning (EEW)
network. It holds the single source of truth for the **wire contract** (the zod
schemas that every MQTT message, DB row, and HTTP/WebSocket payload must satisfy)
plus the small set of **pure, deterministic functions** that the device firmware,
ingest, association, simulator, and web app all rely on.

Two of these functions — `computeWindowFeatures` and `staltaStep` — are **mirrored
bit-for-bit by the ESP32-S3 C firmware** and locked by golden vectors. A mismatch
between the TypeScript reference here and the C mirror would silently break
detection in the field, so this package is treated as the canonical definition and
the firmware must follow it.

```
device firmware (ESP32-S3) ─STA/LTA─▶ MQTT (EMQX) ─▶ apps/ingest ─▶ TimescaleDB
                                                                        │
                                                                  apps/assoc ─▶ events ─▶ apps/web map
                                                                        ▲
                  packages/shared  ← imported by ingest · assoc · sim · web
                  (and the firmware C mirror follows it, golden-locked)
```

---

## What it does

- **The wire contract** — zod schemas in [`src/schema.ts`](src/schema.ts) define
  every message and payload shape. Validation happens at the trust boundary
  (ingest parses untrusted MQTT, the web API validates DB-built payloads), so a
  malformed packet is rejected before it can poison the pipeline.
- **Pure feature/detection math** — `computeWindowFeatures` (per-window RMS + PGA),
  `staltaStep` (recursive STA/LTA trigger), and `haversineKm` (great-circle
  distance). No I/O, no global state — same input, same output everywhere.
- **C↔TS parity guarantee** — golden vectors lock the exact float outputs of the
  feature and STA/LTA math. The firmware re-computes the same vectors at boot
  (`golden_check`, `golden_stalta_check` in
  [`../../firmware/main/main.c`](../../firmware/main/main.c)) and logs a FAIL if it
  disagrees with the locked literals in the test files here.

There is **no runtime, server, or CLI** in this package — it is a pure library.

---

## Exports

Everything is re-exported from [`src/index.ts`](src/index.ts), so consumers import
straight from the package root: `import { FeaturePacket, haversineKm } from "@geoshake/shared"`.

### Zod schemas — the contract ([`src/schema.ts`](src/schema.ts))

Each schema is exported together with its inferred TypeScript type of the same name
(`export type X = z.infer<typeof X>`). A reusable `devId` matcher
(`/^gs-[0-9a-z]{4,}$/`, file-local) validates every device id — note the literal
hyphen is **not** allowed inside the suffix (`gs-00-01` is rejected).

| Export | What it is | Direction (producer → consumer) |
|--------|-----------|----------------------------------|
| `FeaturePacket` | 1 s feature summary: `v, dev, seq, t_ms, fs, n, rms[3], pga` | firmware/sim → ingest |
| `TriggerPacket` | STA/LTA trigger event: `v, dev, seq, t_ms, sta_lta, pga` | firmware/sim → ingest → assoc / web feed |
| `HealthPacket` | Device heartbeat: `v, dev, seq, t_ms, up_s, heap, rssi, wifi_rc, mqtt_rc, fw` | firmware → ingest |
| `EventPacket` | Association event announcement: `v, event_id, originMs, lat, lon, nStations, intensityClass, maxPga, rmsResidual` | assoc (`geoshake/events`) → web (API + WS) |
| `StationInfo` | Map station (located device): `dev, lat, lon, lastHealthMs?` | web map |
| `Waveform` | Event-triggered single-trace waveform: `t0Ms, fs, samples[]` (4-sensor mean magnitude, m/s²) | firmware → web detail page |
| `StationDetail` | Detail-page station: trigger + location + derived `distKm`/`delayMs` (+ optional `waveform`) | web event detail |
| `EventDetail` | `{ event: EventPacket, stations: StationDetail[] }` — one event in full | web event detail (`/api/events/[id]`) |
| `StationProfile` | Station page payload (`/istasyon/[dev]`): `dev, lat, lon, health?, triggers[], events[]` | web station page |

Key invariants baked into the schemas (all enforced by tests):

- **`intensityClass` enum** is exactly `"hafif" | "orta" | "siddetli"` (used by both
  `EventPacket` and the embedded events in `StationProfile`).
- **Time guard**: every `t_ms`/`originMs`/`t0Ms` is `int().nonnegative().max(8.64e15)`
  — the JS `Date` range. `8.64e15 + 1` is rejected.
- **Finiteness guard**: `fs`, `pga`, `lat`, `lon`, `rms[i]`, etc. are `.finite()`,
  so `Infinity` (`1e400`) is rejected.
- **`StationDetail.delayMs` is intentionally signed** — it can be negative because
  the origin time is back-solved, so arrival can precede the computed origin. Do
  **not** add `.nonnegative()` to it. (`distKm`, by contrast, must be `>= 0`.)
- **`StationInfo.lastHealthMs`, `StationDetail.waveform`, `StationProfile.health`**
  are `.optional()` — present only for real devices, absent for simulated stations.

### Pure functions

| Export | Signature | What it does | File |
|--------|-----------|--------------|------|
| `computeWindowFeatures` | `(samples: number[][]) => WindowFeatures` | Averages the 4 sensors per axis, then computes per-axis RMS and the peak in-window vector magnitude (PGA) over one ~1 s window. Empty window → `{ rms: [0,0,0], pga: 0 }`. | [`src/features.ts`](src/features.ts) |
| `staltaStep` | `(s: StaltaState, sample: number[]) => StaltaResult` | One per-sample step of the recursive STA/LTA detector: 4-sensor mean → 1-pole high-pass (gravity/DC removal) → energy characteristic function → STA/LTA ratio vs. trigger threshold, with warm-up suppression, LTA freeze while triggered, and de-trigger hysteresis. | [`src/stalta.ts`](src/stalta.ts) |
| `initStalta` | `() => StaltaState` | Builds the initial detector state (`armed: true`, `lta = ltaMin`, `count: 0`). | [`src/stalta.ts`](src/stalta.ts) |
| `haversineKm` | `(aLat, aLon, bLat, bLon: number) => number` | Great-circle distance in km (`R = 6371`). Same point → exactly `0`. Adequate for the small regional footprint. | [`src/geo.ts`](src/geo.ts) |

Supporting exports:

- `WindowFeatures` — `interface { rms: [number, number, number]; pga: number }`
  ([`src/features.ts`](src/features.ts)).
- `STALTA_PARAMS` — the frozen (`as const`) detection constant block (see table below)
  ([`src/stalta.ts`](src/stalta.ts)).
- `StaltaState` — detector state: `dc[3], sta, lta, armed, triggered, count`.
- `StaltaResult` — `{ state, fired, ratio, pga }`; `fired` is the **rising-edge**
  trigger for *this* sample (true exactly once per onset).

> ⚠️ **Sensor-major frame layout.** Both `computeWindowFeatures` and `staltaStep`
> expect each `sample`/frame to be **exactly 12 numbers** — 4 sensors × 3 axes,
> sensor-major (`[s0x,s0y,s0z, s1x,s1y,s1z, s2x,s2y,s2z, s3x,s3y,s3z]`). The axis
> average is `(s[0]+s[3]+s[6]+s[9])/4` for x, and so on. This shape is **not
> validated** — a short/wrong frame produces `NaN`. Feed fixed-shape data.
> One difference between the two: `computeWindowFeatures` expects gravity already
> **removed**; `staltaStep` takes **raw** samples (gravity included) and removes DC
> itself via the high-pass filter.

### Golden vectors — and why they matter

The firmware C code and this TypeScript are two implementations of the same
arithmetic. To guarantee they agree bit-for-bit, the test files lock the **exact
float outputs** of known inputs as full-precision literals, and the firmware
asserts the same values at boot:

- **Window features** — [`test/features.test.ts`](test/features.test.ts) locks two
  vectors. The first (single-axis, constant amplitude) is deliberately weak; the
  second (`s0=[0.03,0.04,0]×4`, `s1=[0.01,0,0.02]×4`) catches what the first misses:
  RMS ≠ mean-abs (`rms_x ≈ 0.0223607`), 3-axis PGA magnitude (`0.05`), and axis
  ordering (`rms_y ≈ 0.0282843`, `rms_z ≈ 0.0141421`). Firmware mirror:
  `golden_check` in `firmware/main/main.c`.
- **STA/LTA** — [`test/stalta.test.ts`](test/stalta.test.ts) locks two sequences.
  The "4 sensors DIFFERENT" sequence is the one the firmware re-runs: it asserts
  `state.sta ≈ 0.00003883608158025049`, `state.lta ≈ 6.74217320754769e-7`, and
  `ratio ≈ 57.60172630506509`. Locking *different* sensor values also pins the
  4-sensor averaging index map and operation order. Firmware mirror:
  `golden_stalta_check` in `firmware/main/main.c`.

**Rule of thumb:** if you change the feature or STA/LTA math (or its constants), the
golden literals here **and** the firmware constants/asserts must be re-locked
together — otherwise the field devices and the backend will silently disagree.

---

## Building

This is the foundation package; build it first if a consumer needs the compiled
`dist/` (most consumers just transpile the `.ts` source directly — see below).

```bash
pnpm --filter @geoshake/shared build      # tsc -p tsconfig.json → dist/ (.js + .d.ts)
```

`tsconfig.json` extends the repo-root [`../../tsconfig.base.json`](../../tsconfig.base.json)
(`target ES2022`, `module ESNext`, `moduleResolution "Bundler"`, `strict`,
`declaration`) and emits `dist/` from `src/`. `package.json` points `main` at
`src/index.ts` (raw TS), so within the pnpm workspace consumers resolve and
transpile the source rather than the build output.

### How consumers import it

The package is `"type": "module"` (ESM). Imports inside `src/*.ts` use **explicit
`.js` extensions on TypeScript source** — e.g. `index.ts` does
`export * from "./schema.js"`. This is the standard NodeNext/ESM convention: the
specifier names the *emitted* file, while the toolchain resolves the matching `.ts`
at build/test time. Consumers depend on it via the workspace protocol and import
from the package root:

```jsonc
// consumer package.json
"dependencies": { "@geoshake/shared": "workspace:*" }
```

```ts
import { FeaturePacket, EventPacket, haversineKm } from "@geoshake/shared";
import type { StationDetail, Waveform } from "@geoshake/shared";
```

Note: the **`.js`-extension → `.ts` rewrite** for the browser/Next consumer lives in
the consumer, not here — `apps/web/next.config.mjs` sets `transpilePackages:
["@geoshake/shared"]` and a webpack `resolve.extensionAlias` map. Node-side consumers
(`apps/ingest`, `apps/assoc`, `apps/sim`) run the TS via `tsx`/`vitest` under the
same `moduleResolution: "Bundler"` base config.

---

## Parameters

This is a pure library, so there are no runtime flags or env vars. The only tunable
values are the STA/LTA detection constants in `STALTA_PARAMS`
([`src/stalta.ts`](src/stalta.ts)). These are **placeholders** to be tuned against
real `.gsd` recordings; the golden vectors lock the *arithmetic*, so C↔TS parity
holds even after the values change (re-lock the goldens when you do).

| Constant | Value | Meaning |
|----------|-------|---------|
| `alphaHp` | `0.0115` | 1-pole high-pass coefficient (`2π·fc/fs`, ~0.2 Hz @ 109 Hz) — removes gravity/DC |
| `nSta` | `55` | STA window length in samples (~0.5 s @ 109 Hz) |
| `nLta` | `3270` | LTA window length in samples (~30 s @ 109 Hz) |
| `ltaMin` | `1e-8` | LTA floor (m/s²)² — divide-by-zero guard / noise floor |
| `trigger` | `6` | STA/LTA ratio that fires a trigger |
| `detrigger` | `2` | STA/LTA ratio that re-arms the detector (hysteresis) |
| `warmupSamples` | `3270` | Triggers suppressed until this many samples seen (~30 s, lets LTA fill) |

The firmware mirrors these one-for-one as `STALTA_ALPHA_HP`, `STALTA_N_STA`,
`STALTA_N_LTA`, `STALTA_LTA_MIN`, `STALTA_TRIGGER`, `STALTA_DETRIGGER`,
`STALTA_WARMUP` in `firmware/main/main.c` — keep both sides in sync.

---

## Editing & extending

**To change the wire contract**, edit the relevant zod schema in
[`src/schema.ts`](src/schema.ts). Because every TS package imports from here, the
change propagates to `apps/ingest`, `apps/assoc`, `apps/sim`, and `apps/web`
automatically (the type errors will tell you what to update). If the changed
schema is one the firmware emits (`FeaturePacket`, `TriggerPacket`, `HealthPacket`)
or that the firmware mirrors, **update the firmware to match** and re-lock any
affected golden vectors.

Gotchas:

- **`.optional()` rejects an explicit `null`.** Optional fields
  (`StationInfo.lastHealthMs`, `StationDetail.waveform`, `StationProfile.health`)
  must be **omitted** when absent — passing `null` fails validation. Build payloads
  by leaving the key out, not by setting it to `null`.
- **Unknown fields are silently stripped** (zod's default `.strip()`). This is a
  deliberate forward-compatibility choice (see the comment atop `schema.ts`): an old
  ingest ignores new fields a newer fleet sends. Parsing `{...packet, future: 1}`
  succeeds and drops `future`. Don't rely on extra keys surviving a `.parse()`.
- **Don't tighten `StationDetail.delayMs` to non-negative** — negative delays are
  valid (origin is back-solved).
- Keep new pure functions **side-effect-free and deterministic**; if a function will
  be mirrored in firmware, add a golden vector test the same day.

---

## Tests

```bash
pnpm --filter @geoshake/shared test       # vitest run — 10 files, 48 tests
```

The suite (in [`test/`](test/)) covers:

- **Schema validation** — happy-path round-trips plus rejection of bad device ids,
  out-of-enum intensity classes, `Infinity`, over-range `t_ms`, wrong `rms` length,
  and negative gated fields; confirms unknown-field stripping and optional-field
  presence/absence (`schema.test.ts`, `event.test.ts`, `trigger.test.ts`,
  `eventDetail.test.ts`, `station.test.ts`, `stationProfile.test.ts`,
  `waveform.test.ts`).
- **`computeWindowFeatures`** — zero-window base case and the **two golden vectors**
  pinning RMS, PGA, and axis ordering (`features.test.ts`).
- **`staltaStep`** — behavioral tests (constant input never triggers, warm-up
  suppresses early onsets, a single rising-edge fire, LTA freezes while triggered,
  de-trigger re-arms) **and the two C↔TS golden vectors** asserting exact
  `sta`/`lta`/`ratio` (`stalta.test.ts`).
- **`haversineKm`** — zero distance, a known İstanbul–Ankara range check
  (~330–370 km), and symmetry (`geo.test.ts`).

The golden tests are the contract with the firmware: they assert full-precision
float literals so the C `golden_check` / `golden_stalta_check` can reproduce them.

---

## Related

- Repo root: [`../../README.md`](../../README.md)
- Consumers:
  - [`../../apps/ingest`](../../apps/ingest) — validates MQTT packets with
    `FeaturePacket` / `HealthPacket` / `TriggerPacket`, writes TimescaleDB.
  - [`../../apps/assoc`](../../apps/assoc) — uses `haversineKm` + `EventPacket` to
    cluster triggers into events.
  - [`../../apps/web`](../../apps/web) — map/UI; consumes `EventPacket`,
    `StationInfo`, `EventDetail`, `StationProfile`, `Waveform`, `haversineKm`.
  - [`../../apps/sim`](../../apps/sim) — simulator producing the same packet shapes.
- Firmware C mirror: [`../../firmware/main/main.c`](../../firmware/main/main.c)
  (`computeWindowFeatures` / `staltaStep` mirrors, `golden_check`,
  `golden_stalta_check`, `STALTA_*` constants).
- Bring-up docs: [`../../docs/faz2b-bringup.md`](../../docs/faz2b-bringup.md)
  (device STA/LTA), [`../../docs/faz3b-bringup.md`](../../docs/faz3b-bringup.md)
  (event detail / moveout), [`../../docs/faz3c-bringup.md`](../../docs/faz3c-bringup.md)
  (event-triggered waveform). Full set: `../../docs/faz*-bringup.md`.
