# radio-noise-key-generator — specification

Written 2026-08-19. Read this before changing the engine.

## 1. What the program claims, and what it does not

**Claims:** the output is at least as strong as `getrandom()`, and carries
whatever additional entropy the radio contributed on top, with that
contribution measured rather than assumed.

**Does not claim:** to be a certified SP 800-90B entropy source. The tests
of that document are implemented and their cutoffs are checked against its
own worked examples, but no validation lab has looked at this, no restart
tests are performed, and the conditioning is not one of the vetted
constructions of SP 800-90C. Anyone who needs a validated RNG needs
certified hardware, not this.

The honest summary: this is `getrandom()` with a radio stirred in, and an
instrument panel showing what the radio was doing at the time.

## 2. Pipeline

```
RTL-SDR ──► block (256 KiB of 8-bit I/Q)
              │
              ├─► Repetition Count Test      §4.4.1   ─┐
              ├─► Adaptive Proportion Test   §4.4.2    ├─ any failure
              ├─► serial correlation         §4.5      │  stops collection
              ├─► I/Q correlation            §4.5     ─┘
              │
              ├─► Most Common Value estimate §6.3.1 ──► credit = h_min × n × 0.5
              │
              └─► SHAKE-256.absorb(block)
                                    │
    getrandom(64 B) ────────────────┤  (unconditional, at finish)
                                    │
                                    └─► SHAKE-256.squeeze ──► password / key
```

## 3. Why SHAKE-256 and nothing else

The requirement is "keys of any length". A sponge with extendable output
absorbs an arbitrary amount of noise and squeezes an arbitrary amount of key
material with one primitive — no counter mode, no chained blocks, no place
for an off-by-one to repeat a block of output. Squeezing is stateful, so
successive calls continue the stream rather than restarting it; the unit
test pins that.

Domain separation string `cz.ok1br.rnkg/shake256/v1` is absorbed first. Any
future change to the construction gets a new version there, so the same noise
never silently produces the old output under new rules.

## 4. Choice of frequency and gain

Default 1300 MHz: above the LTE bands, below the aeronautical L-band
services, and nothing in region 1 transmits there at a level a bare dongle
picks up indoors. Default gain is the tuner's maximum, with AGC off — the
noise we want is the receiver's own thermal floor, and reducing gain merely
replaces it with quantisation steps. AGC would track whatever signal
wandered into the passband, which is precisely backwards.

Both are overridable. The health tests are what decide whether a given
choice was good, not the defaults.

## 5. The credit margin

`RNKG_CREDIT_MARGIN = 0.5`. The MCV estimator assumes i.i.d. samples. An
RTL-SDR stream is not i.i.d. — USB framing, DC offset and tuner spurs all
leave structure that MCV cannot see, so its estimate is an upper bound on
the truth. Halving it is a blunt instrument, chosen because being wrong here
is expensive and being slow is not: at 2.048 MS/s a block is an eighth of a
second, and even a 512-bit key needs one block.

If a future version implements more of §6.3 (collision, Markov, compression,
the prediction estimators) and takes the minimum across them as the standard
requires, this margin should be revisited — not before.

## 6. Milestones

- **M0 — engine and CLI.** Source (dongle + file), health tests, MCV
  estimate, SHAKE-256 conditioning, password and key generation, gates
  running without hardware. *Done 2026-08-19.*
- **M1 — hardware bring-up.** Verify against a real dongle: measured
  min-entropy at several frequencies and gains, capture replayed through the
  NIST reference tool, behaviour when the dongle is pulled mid-collection.
- **M2 — GTK4 front end.** Live spectrum so the empty channel is visible,
  one status line, and the generation panel with live rotation and Snap —
  the window is specified in §8. The spectrum is the reason a GUI exists:
  it is the visual proof that what is being sampled is noise and not a
  carrier.
- **M3 — packaging.** `.desktop`, metainfo, icon in the family's Papirus
  style, AUR package.

## 7. Open questions

- Whether to offer a diceware-style passphrase mode, and if so with which
  word list — the Czech keyboard-layout argument that drives the letters-only
  default applies to word lists differently.
- Whether the GUI should be able to write directly into a `pass` store via
  `pass-for-linux`'s engine, or stay a generator that hands over a string.
- Whether to keep a rolling capture of the last block for export, so a user
  can hand it to the NIST tool without re-running `rtl_sdr` separately.
  Against: it means key-adjacent material sits in memory longer than it needs
  to.

## 8. The window (decided with Richard, 2026-08-19)

One window, three parts, nothing to configure:

- **The spectrum**, most of the window. Its only job is the visual check
  that the frequency is empty. No controls around it.
- **One status line** under it: the measured numbers while the source is
  healthy (min-entropy, serial, I/Q, DC, block count), the engine's human
  message in red plus a Reopen button when a test has failed.
- **The generation panel**, the main act. Every healthy block derives a
  fresh candidate — its own extractor, that block's measured credit, the
  unconditional kernel seed — and the panel rotates through them live, at
  the block rate. **Snap** freezes whichever candidate is on screen at the
  moment the user chooses; Copy puts it on the clipboard with a timed
  clear; Rotate resumes. Length and alphabet sit next to the button.

No source panel: the GUI runs on the engine defaults (1300 MHz, maximum
gain, 2.048 MS/s) and whoever needs a different frequency has the CLI.
A first draft had device/frequency/gain/rate controls and a seven-field
health row; Richard cut it — the GUI is a generator, not an SDR console.

**Secrets in the UI, decided:** showing candidates on screen is the point
of the rotation, so the display label holds them in ordinary heap and that
is accepted. What the program controls, it wipes: engine strings live in
secure memory until copied out, our own copies are zeroed, the clipboard
clears itself after 45 s (and only if it still holds our text). What GTK
and Pango copy internally cannot be wiped — the same call pass-for-linux
made for its reveal path.
