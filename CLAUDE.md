# radio-noise-key-generator

Passwords and keys from RTL-SDR noise (OK1BR). Fifth app of the family with
`sdr-for-linux`, `skimmer-for-linux`, `log-for-linux` and `pass-for-linux` —
same conventions apply:

- Plain **C11**, GTK4 + libadwaita, meson. No Rust/Python in the app.
- **Engine is GLib-only** (`src/engine/`, no GTK includes) — headless and
  testable; the GTK front end lives in `src/app/`, the CLI in `src/cli/`.
- App id: `cz.ok1br.radio_noise_key_generator`. Licence: GPL-3.0-or-later.
- Build: `meson setup build && meson compile -C build`.
- On Arch always build from source; install goes to `~/.local`, not `/usr`.

Scope/design: `docs/SPEC.md` — what the program is and why. **Read it first.**
What is left to build: `docs/SCOPE.md` — the working list, milestone by
milestone, including the traps already hit here. Status and handover:
`docs/STATUS.md` — what is verified and what is explicitly not.

**Status (2026-08-19): M1 done, 4/4 gates green (23 tests), live
generation from the dongle verified (RTL-SDR Blog V4, 1300 MHz).** Real
hardware measures 0.62–0.84 bits/sample, not the >4 M0 assumed —
`RNKG_ASSESSED_H` is 0.3 now; see `docs/STATUS.md` for the measurements.
Warm-up discard, §4.3 startup test and the read watchdog are in; the DC
spike is measured (no offset tuning), the NIST-tool cross-check passed
(halved credit under the full suite's minimum on both operating points),
and a mid-collection dongle pull ends the run cleanly. Loose ends noted
in STATUS.md: the watchdog's timeout path is unverified (needs a silently
wedged device) and one 100 MHz run passed unexplained. Next: M2, the GTK4
front end — the live spectrum first (SCOPE 2.2 needs an FFT decision).

Public repo: https://github.com/OK1BR/radio-noise-key-generator — commit
and push continuously as work lands (Richard asked for this 2026-08-19);
milestone-sized batches are fine, broken intermediate states are not.

## Four rules that override convenience

1. **The kernel is mixed in unconditionally.** Every derivation absorbs 64
   bytes from `getrandom()` at finish time. This is not a fallback for when
   the radio fails — it runs every time, so that output is never weaker than
   `getrandom()` alone even if the dongle is hostile. Never make it optional,
   never make it conditional on a health test passing.
2. **Never credit entropy that was not measured.** A block is credited from
   the MCV estimate over that block, halved. No constants, no "an 8-bit ADC
   gives 8 bits per sample", no crediting a block that failed a health test.
3. **A failed health test is terminal.** SP 800-90B §4.3: a source that fails
   a continuous test does not resume. Do not add a retry that skips the bad
   block and carries on.
4. **Rejection sampling, never modulo.** Mapping bytes onto an alphabet whose
   size does not divide 256 biases the low residues invisibly. Watch the
   integer width while doing it — for hex and base64 the rejection threshold
   is exactly 256, which a `guint8` wraps to zero.

## Testing without hardware

Every gate runs on a machine with no dongle attached: the cutoffs are checked
against the worked examples and Table 2 printed in SP 800-90B itself, and the
detectors are fed synthetic streams (stuck, dominant-value, correlated,
uniform). `rnkg --file` replays captures. Do not add a test that needs the
dongle present — it will not run in CI and it will rot.
