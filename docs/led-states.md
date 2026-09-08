# T1 status light

The GeoShake T1 has one status light on its top face: a single colour on
GPIO39, dimmed by hardware PWM. What tells the states apart is the
**pattern**, not the brightness.

This describes firmware `blep-0.9.3` and later. For older firmware, see
[steady light on older firmware](#steady-light-on-older-firmware).

## What the pattern means

| What you see | Period | What it means | What to do |
|---|---|---|---|
| **Soft pulse** — brightness ramps smoothly up and down, never reaching full | ~3 s | **Working.** The station is online and sending data. | Nothing. This is the state you want. |
| **Slow blink** — clean on and off | ~0.5 s | **Not on the network.** Either waiting to be set up, or set up and unable to connect. | Tell the two apart with the WiFi check below. |
| **Fast blink** | ~0.2 s | **Collecting its identity.** Transient: the device is registering itself, and restarts on its own afterwards. | Let it finish. |
| **Double flash** — two short flashes, then a long dark gap | ~2 s | **No identity stored.** The board left the factory without an identity, or lost it. It cannot join the network in this state. | Contact support. This needs a USB repair; it cannot be fixed over WiFi. |
| **Steady, full brightness** | ~2.5 s, then back to the pulse | **Trigger.** The device detected ground motion above its threshold. | Nothing. It returns to normal by itself. Footsteps, a slammed door and a passing lorry all do this — one flash is not an earthquake. |
| **Very fast flicker** | ~0.15 s | **The device failed its own start-up self-check.** Readings from it cannot be trusted. | Unplug, wait ten seconds, plug back in. If the flicker returns, contact support. |

## The one ambiguity: a slow blink means two different things

The ~0.5 s blink appears both when the device is waiting to be set up and
when it is already set up but cannot reach the network. Those need
different fixes. One question separates them:

> **Look at the WiFi list on your phone. Is there an open network called
> `GeoShake-XXXX`?**

| Answer | State | What to do |
|---|---|---|
| **Yes** | Waiting for setup — it has no WiFi credentials yet | Run setup from the app, or join that network and open `http://192.168.4.1` |
| **No** | Set up, but cannot reach the network or the server | Check the router is on, that the band is 2.4 GHz, and whether the WiFi password changed |

While it is waiting for setup the device is **also** advertising over BLE as
`GST1-XXXXX`. The two setup paths run side by side; use whichever works.

> **The four characters in `GeoShake-XXXX` are not the code on the label.**
> The label carries the device id (`GEO-XXXXX`); the SoftAP name carries the
> last bytes of the MAC address. For example, BLE `GST1-FVYSA` belongs to
> the same unit as SoftAP `GeoShake-14DD`. Do not expect them to match.

## Steady light on older firmware

A light that simply stays on is the commonest worry, and on firmware older
than `blep-0.9.3` it **was not a fault**. A device waiting for setup never
drove GPIO39 at all, and an undriven pin leaves the LED **steady on**. So
on those builds "steady light and no network" meant *waiting for setup*,
not broken.

From `blep-0.9.3` onward all four waiting states show one of the patterns
in the table above, and a steady light happens **only** on an earthquake
trigger, for about 2.5 seconds.

Units already in the field do not pick this up over OTA. A device in setup
mode has no network, and OTA arrives over the network — the update cannot
reach the exact state it fixes. A unit that shipped with older firmware and
has never been set up will still show a steady light the first time it is
powered on.

## Where this comes from

The state machine and the waiting patterns are in
[`firmware/t1-revc/main/main.c`](../firmware/t1-revc/main/main.c):
`led_task()` drives a running device, and `led_wait_forever()` drives the
branches that never reach the normal boot path. The periods above are the
`LED_*_MS` constants in
[`firmware/t1-revc/main/config.h`](../firmware/t1-revc/main/config.h) and
in `main.c`.

The soft pulse is deliberately not used for any waiting state: in
`led_task()` that pattern means "healthy and connected", so reusing it
while the device is waiting would produce exactly the confusion the
patterns exist to prevent.
