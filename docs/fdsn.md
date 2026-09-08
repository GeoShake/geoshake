# FDSN data access

GeoShake publishes station metadata, an event catalogue and live waveforms
through standard FDSN services. This page says what is served, where, and
what the numbers mean. Access is anonymous: no registration, no API key.

## The network

| | |
|---|---|
| Network code | `GW` |
| Registered name | GeoShake Global Seismic Network |
| FDSN registry | <https://www.fdsn.org/networks/detail/GW/> |
| Network DOI | [10.7914/ft1b-4x43](https://doi.org/10.7914/ft1b-4x43) |
| Data centre | `GEOSHAKE` — <https://www.fdsn.org/datacenters/detail/GEOSHAKE/> |

`GW` is permanently registered with the FDSN. It is not provisional and not
self-assigned.

The code, the channel identifiers and the count scaling all live in one
file in this repository —
[`firmware/shared/src/fdsn.ts`](../firmware/shared/src/fdsn.ts). The
firmware, the SeedLink feeder and the metadata publisher read the same
constants, because a value written twice eventually disagrees with itself
and metadata that disagrees with waveforms fails silently.

> **Files downloaded before 27 July 2026 carry a different network code.**
> The change to `GW` on that date was a breaking one: SAC and StationXML
> files fetched before then, and old SeedLink stream ids, still carry the
> previous code. Do not fold those files into a shared archive — re-fetch
> them and they come back as `GW`.

## Services

| Service | Endpoint | Format |
|---|---|---|
| `fdsnws-station` | `https://api.geoshake.org/fdsnws/station/1/` | StationXML 1.1 |
| `fdsnws-event` | `https://api.geoshake.org/fdsnws/event/1/` | QuakeML 1.2 |
| SeedLink | `seedlink.geoshake.org:18000` | miniSEED |

**Station.** `level=network`, `station`, `channel` and `response` are all
supported, in StationXML or in the FDSN text format (`format=text`).
`level=response` returns a nominal sensitivity — see below.

**Event.** An origin is published only once at least three stations
associate the same event, and station density does not yet support that in
most of the world. An empty catalogue answers HTTP 204, which is a normal
reply rather than an error.

**SeedLink.** Four channels per station, served by RingServer 4.5.4
(SeedLink 4.0, backwards compatible with 3.1). A station appears once it
has been online long enough to fill its buffer.

Archived waveform windows are not served over FDSN. Live waveforms come
from SeedLink, and individual triggers can be downloaded as SAC from each
station page. The REST API behind those pages is documented at
<https://api.geoshake.org/developer>.

A public limit of 100 requests per 5 minutes per IP applies as abuse
protection. There is nothing to apply for and no separate quota.

## Channels

Every station publishes four channels under location code `00`:

| Channel | Sample rate | Content |
|---|---|---|
| `LNZ` | 1 sps | continuous scalar ground-motion envelope |
| `ENZ` | ~110 sps (nominal) | trigger burst, true vertical, positive up |
| `EN1` | ~110 sps (nominal) | trigger burst, horizontal, azimuth unknown |
| `EN2` | ~110 sps (nominal) | trigger burst, horizontal, orthogonal to `EN1` |

Three things are worth knowing before you process any of it.

**`LNZ` is not a component.** Each sample is the peak, over that one
second, of the three-axis acceleration vector magnitude
`sqrt(x² + y² + z²)`, averaged across the four MEMS sensors in the
enclosure. It is a non-negative scalar with no polarity and no direction,
so the trace never crosses zero. The trailing `Z` is a placeholder the SEED
channel-code format requires; it is not a claim that these samples measure
vertical motion. `LNZ` runs continuously, which makes it the channel to use
when you need a trace that is always there.

**`ENZ` is a genuine vertical.** Each burst is rotated out of the enclosure
frame into a local frame built from the gravity vector measured in that
same burst, so the component letter holds regardless of how the unit is
mounted. `Azimuth 0` / `Dip -90`, positive up.

**The horizontals have no azimuth.** `EN1` and `EN2` are orthogonal and lie
in the horizontal plane (`Dip 0`), but the devices have no magnetometer and
gravity fixes only the vertical, so the bearing is unknown. That is why the
component letters are `1` and `2` rather than `N` and `E`: in SEED those
would assert true north and east. `Azimuth` is omitted rather than set to
zero, so rotation routines fail loudly instead of quietly returning a
wrongly oriented result. The unknown azimuth is stable for a station that
is not remounted, so the pair is still usable for particle motion,
horizontal peak amplitude, or an empirical back-azimuth to a known source.

The burst channels are published only while a station is triggered, so they
are intermittent and gappy by design. Their sample rate in the metadata is
nominal; the real rate is measured per burst and varies slightly, and the
authoritative value is in each miniSEED record header.

Every channel carries a `<Description>` in StationXML stating exactly what
its samples are. Anchor your processing to that text rather than to the
channel code.

## Counts to acceleration

Samples are integer counts. The conversion is a single constant:

```
acceleration (m/s²) = counts / 1000000
```

The same constant sets the SeedLink feeder gain and the sensitivity
published in StationXML, so `level=response` reports `1000000` with input
units `M/S**2` and output units `COUNTS`.

That sensitivity is **nominal, not a calibration.** It is the product of two
known quantities, neither of which is a measurement of the individual unit:
the LSM6DSO datasheet full-scale setting (±2 g across a 16-bit output),
which is how the firmware converts raw sensor codes to m/s², and the
constant integer gain the publisher then applies to reach counts. There is
no per-unit calibration, no shake-table calibration, and no correction for
mounting, enclosure or ground coupling. No frequency response has been
measured either, which is why the response is a flat scalar sensitivity
with no poles and zeros — publishing a pole-zero model would present an
unmeasured quantity as a measured one.

Use it to convert counts to m/s² and to compare stations with each other.
Do not treat the result as an absolute, traceable amplitude, and do not use
these amplitudes for magnitude or spectral work.

## Reading it with ObsPy

ObsPy appends `/fdsnws/<service>/1/` to the base URL itself, so pass the
host:

```python
from obspy.clients.fdsn import Client

client = Client(base_url="https://api.geoshake.org")
print(sorted(client.services))

inv = client.get_stations(network="GW", level="channel")
print(inv)

inv = client.get_stations(network="GW", station="KK9YF",
                          channel="ENZ", level="response")
sens = inv[0][0][0].response.instrument_sensitivity
print(sens.value, sens.input_units, "->", sens.output_units)
```

The last line prints `1000000.0 M/S**2 -> COUNTS`. On connect ObsPy prints
a warning about parameters missing from the service WADL; the calls above
are unaffected.

Live waveforms come from the same library through its SeedLink client:

```python
from obspy import UTCDateTime
from obspy.clients.seedlink.basic_client import Client

sl = Client("seedlink.geoshake.org", port=18000, timeout=30)
end = UTCDateTime()
st = sl.get_waveforms("GW", "KK9YF", "00", "LNZ", end - 600, end)

for tr in st:
    tr.data = tr.data / 1e6      # counts -> m/s²
print(st)
```

A pull from the ring buffer can come back as several traces separated by
short gaps; call `st.merge()` if you need one continuous array. Use the
station codes exactly as the inventory reports them rather than deriving
them from a device id.

## Boundaries in the archive

Three dates change what the data means. A single StationXML entry cannot
express a change of meaning, so they are recorded here instead.

| Date | What changed |
|---|---|
| 27 July 2026 | Network code became `GW`. Earlier downloads carry the old code. |
| 5 August 2026 | `ENZ` rotation to true vertical landed in the publisher. Samples archived before this date carry the raw enclosure Z axis, whose orientation is unknown. |
| 28 August 2026 | Station codes became identical across StationXML, SAC and SeedLink. Inventory cached earlier used a longer form (`GEOBH2JN`), and two stations were renumbered to satisfy the ISC letter-first rule (`274YU` → `G274Y`, `2TRS8` → `G2TRS`). |

Two further caveats that are not dated but are permanent: station
coordinates are supplied by their owners, who are asked to drop the pin
near their home rather than on it, so positions are approximate by design
for resident privacy; and station elevation is sampled from a digital
elevation model at those coordinates, describing the terrain rather than
the sensor — a unit on an upper floor sits above the published value.

## Citation

If you publish work that uses this network, cite it as:

> GeoShake (2026): GeoShake Global Seismic Network. International
> Federation of Digital Seismograph Networks. Dataset/Seismic Network.
> https://doi.org/10.7914/ft1b-4x43

The network DOI is `10.7914/ft1b-4x43`, and it identifies the network
itself. The event catalogue is deposited separately as *GeoShake Community
Seismic Event Catalog*, [10.5281/zenodo.21313786](https://doi.org/10.5281/zenodo.21313786)
— cite that when you mean the catalogue rather than the network. That is a
concept DOI: it always resolves to the newest version, so a citation
written today keeps working as the catalogue grows. For a fixed snapshot,
cite the version DOI instead.

## Licence

GeoShake network data — event catalogue, station metadata, ground-motion
features, waveforms and derived products, however obtained — is published
under **Creative Commons Attribution 4.0 International (CC BY 4.0)**. The
full terms are in [`data/LICENSE`](../data/LICENSE).

You may share and adapt the data for any purpose, including commercially
and in academic publications, provided you give credit and say whether you
changed anything.

## What this network is not

Being straight about the limits is what makes the rest of it credible.

- **Not a warning service.** GeoShake is a community network, not a
  civil-protection system. Do not rely on it for life-safety decisions.
- **The sensors are uncalibrated MEMS accelerometers.** Amplitudes are
  indicative, not metrologically traceable.
- **"Magnitude" in the QuakeML is a PGA-derived MMI intensity**, labelled
  as such in the file. It is not a seismic magnitude, and there is no depth
  estimate.
