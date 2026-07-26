# Contributing to GeoShake

Thanks for your interest. Bug reports, firmware improvements and
enclosure remixes are all welcome.

## Developer Certificate of Origin (DCO)

Every contribution must be signed off. By adding a `Signed-off-by` line
to your commit you certify the [Developer Certificate of Origin
1.1](https://developercertificate.org/) — in short: that you wrote the
contribution, or have the right to submit it under the project's license.

Add it automatically with:

```sh
git commit -s -m "fix: ..."
```

which appends:

```
Signed-off-by: Your Name <your.email@example.com>
```

Pull requests without a sign-off cannot be merged. Please use your real
name (pseudonyms are not accepted for DCO purposes).

## Licensing of contributions

- Contributions to `/firmware` are accepted under **GPL-3.0-or-later**.
- Contributions to `/hardware/enclosure` are accepted under
  **CC BY-SA 4.0**.
- Contributions to `/docs` are accepted under **CC BY-SA 4.0**.

You keep the copyright to your contribution; you license it to the
project and to everyone else under the license above.

## What we can and cannot accept

**Welcome:**
- Firmware bug fixes, portability work, new board/sensor support
- Detection tuning with evidence (test vectors, real event recordings)
- Enclosure variants, mounting options, print profile improvements
- Documentation, translations, build instructions

**Cannot accept:**
- PCB design contributions — the board design is proprietary and not
  published here.
- Changes that add the GeoShake trademark to derivative designs.
- Code copied from incompatible-licensed sources.

## Firmware changes: keep the goldens green

The C firmware in `firmware/t1-revc` is **bit-locked** to the TypeScript
reference in `firmware/shared`. If you change any feature-math or STA/LTA
constant you must update **both**, and update the golden literals so the
boot self-checks still print `OK`. A `FAIL` on any golden line means the
C has drifted from the reference — that will not be merged.

## Reporting security issues

Do not open a public issue. See [SECURITY.md](SECURITY.md).
