# Status

## 2026-08-19 — M1 bring-up: first live generation

Hardware: RTL-SDR Blog V4 (R828D tuner), `extra/rtl-sdr` 1:2.0.2-1 installed
with Richard's go-ahead. Device access works unprivileged via the packaged
udev rule's `uaccess` ACL — `rfa` did **not** need adding to the `rtlsdr`
group. The DVB kernel modules (`dvb_usb_rtl28xxu`, `rtl2832`) had grabbed
the dongle from a pre-install plug and were unloaded manually; the packaged
blacklist covers future boots.

**Verified live on the dongle:**

- The `HAVE_LIBRTLSDR` branch compiled first try, zero warnings, and
  `rtlsdr_get_tuner_gains()` really returns ascending values (29 gains,
  0.0 → 49.6 dB), so `highest_gain()`'s assumption holds on this hardware.
- End to end from the dongle at 1300 MHz: password and 32-byte key
  generated, one 256 KiB block credits ~110 kb. Collection is effectively
  instant — the GUI needs no progress bar for ordinary targets.
- Live rejection works: tuned to an FM broadcast (100 MHz) the serial
  correlation test fails the run (serial −0.14) with the retune message.
  The 0.05 threshold has real margin: the engine measures |serial| ≤ 0.004
  on empty-channel hardware noise, 0.13–0.18 on a carrier.

**The M0 assumption that fell:** a healthy stream is *not* >4 bits/sample.
The V4 at 1300 MHz spans only ~6 ADC codes around the DC offset; MCV
measures 0.62–0.84 bits/sample across manual gains (12.5 → 49.6 dB,
p_max 0.65 → 0.56). `RNKG_ASSESSED_H` dropped 4.0 → 0.3 — the old value
made the RCT cutoff (6) fire instantly on real noise. Credited entropy is
unaffected (the estimator measures per block); a stuck source still trips
RCT at 68 samples. All 3 gates still green after the change.

**Engine gaps closed later the same day** (SCOPE 1.3), verified live:

- **Warm-up discard:** the source throws away the first 256 KiB after
  tuning, before anything reaches the health tests.
- **Explicit §4.3 startup test:** `RNKG_STARTUP_SAMPLES` (one block) must
  pass every health test before crediting begins; the block is discarded.
  The CLI marks it: `block 1 … (startup test — discarded)`. A fourth gate
  (`rnkg-collect-test`, 3 tests) pins the behaviour: startup credits
  nothing, a startup failure is terminal, startup samples alone can never
  satisfy a target. 4/4 gates, 23 tests.
- **Read watchdog:** every dongle read runs in its own thread with a
  deadline (4× stream time + 1 s); a device that stops delivering fails
  the run instead of hanging it. The timeout path is written but **not
  verified** — triggering it needs a genuinely wedged device. The normal
  path is exercised by every live run.

**Observed once, not explained:** out of six live runs at 100 MHz (FM
broadcast), five were rejected by the serial correlation test (serial
−0.12 to −0.14) and one passed and generated. Hypothesis: a transient
retune failure left the tuner off-frequency, so the samples genuinely were
noise — consistent with the per-block checks all passing in that run, but
unproven. If it was real correlated FM passing under the threshold, the
design still held: credit is measured per block and the kernel seed is
mixed in unconditionally. Watch for it during M2's live spectrum work.

**Still open in M1:** DC-spike measurement/offset-tuning decision,
dongle-pull behaviour (needs a hand on the hardware), NIST reference-tool
cross-check of the MCV numbers.

## 2026-08-19 — M0 done, no hardware yet

Engine and CLI are written, build clean at `warning_level=2`, and all three
gates pass (health 8, estimate 4, generate 8).

**Verified on this machine:**

- Health cutoffs reproduce the worked examples and Table 2 of SP 800-90B.
- MCV reproduces the §6.3.1 worked example (L = 20, p̂ = 0.4 → p_u = 0.6895).
- End to end from a file of `/dev/urandom`: 7.78 bits/sample measured,
  serial −0.0012, I/Q −0.0046, passwords and keys produced.
- Rejected as designed: all-zero input (repetition count), correlated input
  (serial 0.5030), short input.
- Long output works: 1024-byte key, 5000-character password.

**Not verified — no dongle attached, and `rtl-sdr` is not installed:**

- The `HAVE_LIBRTLSDR` branch of `rnkg-source.c` has never been compiled.
  It is written but unproven; treat it as such until M1.
- No idea yet what min-entropy a real dongle measures at 1300 MHz, or
  whether the correlation thresholds (0.05) are right for real samples. They
  were picked to be well clear of what uniform noise produces over a
  256 KiB block, but real hardware may sit closer to them than synthetic
  data does.

**Next:** install `extra/rtl-sdr`, build with librtlsdr, and run M1.
