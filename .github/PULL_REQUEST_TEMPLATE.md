<!--
Thanks for contributing. Please read `CONTRIBUTING.md` first - this template
only repeats the parts that block a merge.
-->

## What this changes

<!-- One paragraph. Link the issue it fixes, if there is one. -->

## Sign-off (DCO) - required

Every commit must carry a `Signed-off-by` line certifying the
[Developer Certificate of Origin 1.1](https://developercertificate.org/).
Add it automatically with:

```sh
git commit -s -m "fix: ..."
```

Please use your real name - pseudonyms are not accepted for DCO purposes.
**Pull requests without a sign-off cannot be merged.**

- [ ] Every commit in this PR is signed off (`git commit -s`)

## Licensing of this contribution

You keep the copyright to your contribution and license it to the project
and to everyone else under the license of the area you touched. Tick what
applies:

- [ ] `firmware/` - accepted under **GPL-3.0-or-later**
- [ ] `hardware/enclosure/` - accepted under **CC BY-SA 4.0**
- [ ] `docs/` - accepted under **CC BY-SA 4.0**
- [ ] This is my own work, or I have the right to submit it, and it contains
      no code copied from incompatible-licensed sources

## Firmware changes: keep the goldens green

The C firmware in `firmware/t1-revc` is **bit-locked** to the TypeScript
reference in `firmware/shared`. If you changed any feature-math or STA/LTA
constant you must update **both**, and refresh the golden literals so the
boot self-checks still print `OK`. A `FAIL` on any golden line means the C
has drifted from the reference - that will not be merged.

- [ ] Not a firmware change, this section does not apply
- [ ] Both `firmware/t1-revc` and `firmware/shared` are updated
- [ ] Boot log still prints `golden OK`, `golden FRAME OK`, `golden STA/LTA OK`

<!-- Paste those boot log lines here if you touched the detector or the feature math. -->

## Not accepted

Per `CONTRIBUTING.md`, these cannot be merged:

- PCB design contributions - the board design is proprietary and is not
  published in this repository
- Changes that add the GeoShake trademark to derivative designs
- Code copied from incompatible-licensed sources

Security issues do not belong in a public pull request. See `SECURITY.md`.
