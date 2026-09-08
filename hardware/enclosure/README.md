# GeoShake T1 — Enclosure (v2.1)

Open enclosure designs, licensed under **CC BY-SA 4.0** (see [LICENSE](LICENSE)).
SPDX-License-Identifier: `CC-BY-SA-4.0`

| Path | Contents |
|---|---|
| [`stl/`](stl/) | Printable meshes — the current **v2.1** set |
| [`stl/legacy/`](stl/legacy/) | The superseded **Rev-C** set, kept for anyone who already printed it |
| [`cad/`](cad/) | `GeoShake_T1V1.f3d` — editable Fusion 360 source (**Rev-C**, see below) |
| [`docs/`](docs/) | Print settings, hardware list, heat-set insert photos, assembly guide |

**Start here:** print the three v2.1 STLs with the settings in
[`docs/README.md`](docs/README.md), then follow
[`docs/GeoShake T1 - Assembly Guide.pdf`](docs/GeoShake%20T1%20-%20Assembly%20Guide.pdf).

## The v2.1 set

| File | Part |
|---|---|
| `GS_T1_Enclosure_Top_v2_1.stl` | Lid — carries the two insert seats and the USB-C opening |
| `GS_T1_Enclosure_Base_v2_1.stl` | Base — the board bolts to its four corner posts |
| `GS_T1_Mounting_Base_v2_1.stl` | Mounting base — screws to the wall or floor, the device seats in it |

v2.1 keeps the same outer envelope as Rev-C — same footprint, same height, same
USB-C position — and changes what is inside it. Two things came out of the
revision:

- **Better Wi-Fi.** This is the revision we print and ship against, and it is
  the reason to prefer it. Print the lid at the 15 % gyroid infill in
  [`docs/README.md`](docs/README.md) — the setting is part of the design, not a
  saving, and a solid lid gives up the gain.
- **Proper heat-set insert seats.** The lid bosses now have a conical lead-in
  (Ø5.4 mm tapering to the Ø3.6 mm pilot), so an M3 insert starts straight and
  seats flush. Rev-C had a plain straight hole.

Both fixing methods still work — the previous nut method and the insert method
— so nothing about the older build instructions is invalidated. See
[`docs/README.md`](docs/README.md) for the hardware list of each.

Rev-C is not deleted. It is in [`stl/legacy/`](stl/legacy/) for anyone who has
already printed it or is mid-build.

## About the CAD source

`cad/GeoShake_T1V1.f3d` is the **Rev-C** Fusion 360 source. It is still the
right starting point for understanding or remixing the design — the outer
geometry is unchanged in v2.1 — but it is not the file the v2.1 meshes were
exported from. The v2.1 source is not published yet.

If you use another CAD package, you can export STEP or IGES from the `.f3d`.

## About the top shell

The published lid is the **production geometry with the engraved GeoShake
wordmark removed**. Retail units differ from these files only by that
engraving, which is a trademark and excluded from the license.

You may print, modify and sell enclosures based on these files. You may
not put the GeoShake name or logo on them — see
[../../TRADEMARKS.md](../../TRADEMARKS.md).

## Scope note

This license covers the **enclosure only**. The GeoShake T1 PCB design is
proprietary and is not published. Please do not describe devices built
from these files as "open source hardware" — the accurate phrasing is
"open enclosure design".

## Attribution

CC BY-SA 4.0 requires credit. If you publish a remix or a product using
these designs, include:

```
Based on the GeoShake T1 enclosure by Appflows Teknoloji A.S.
Source: https://github.com/GeoShake/geoshake
Licensed under CC BY-SA 4.0
```
