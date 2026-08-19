# Status

## 2026-08-19 end of day — where this stands, for the next session

M0 + M1 done, M2 essentially done. The window matches SPEC §8 after
several rounds of Richard's feedback (spectrum strip with block-mean DC
removal and an axis that follows the noise, one status line, generation
panel with live rotation + Snap, hamburger with Settings/About, GKeyFile
persistence in ~/.config incl. save-on-change). The CLI grew the
scriptable snap (`--snap` on stdin newline/EOF, `--snap-after SEC`) and
defaults its length/alphabet from the same ini the GUI writes
(command line > ini > built-in). 5/5 gates, 27 tests, everything pushed
to https://github.com/OK1BR/radio-noise-key-generator.

**Open, in rough order for next time:**

- **M3 packaging** — icon (Papirus style), .desktop, metainfo, AUR;
  SCOPE says only when Richard asks. The About dialog already points at
  the app-id icon name.
- The one unexplained 100 MHz pass (see below); watch during spectrum
  work.
- Watchdog timeout path still unverified (needs a silently wedged
  device).
- SPEC §7 open questions: diceware mode, pass-store hand-off, capture
  export, more §6.3 estimators.
- Cosmetic: GTK 4.22 prints GtkImage baseline warnings on stderr with
  the About dialog open; upstream noise, not ours, but check again after
  a GTK update.

## 2026-08-19 — M2 begun: the live spectrum works

The FFT decision fell to a plain radix-2 written into the engine
(`rnkg-spectrum.c`, display-only, zero new dependencies; `sdr-for-linux`'s
FFTW is WDSP's requirement, not a family convention worth importing). A
fifth gate pins it against a naive DFT. `src/app/` holds the AdwApplication
bootstrap, a window reading the dongle on a worker thread, and the spectrum
view: black, white live trace, dim peak hold, dB grid, tuning marker.

**Verified live, including by eye:** Richard confirmed the rendered
spectrum on the real dongle — noise floor around −75 dB, the DC spur in
the centre, the 1300.000 MHz marker. 5/5 gates green, zero warnings, the
app opens/reads/exits cleanly (`RNKG_AUTOCLOSE_MS` hook).

**Built later the same day, on Richard's design:** the window per SPEC §8
(spectrum strip with block-mean DC removal and an auto-following axis,
one status line, generation panel with live rotation + Snap + Copy with
clipboard self-clear, settings behind the family hamburger with About,
GKeyFile persistence in ~/.config incl. save-on-change). Verified live
and by Richard's eye through several iterations.

**CLI snap** — the scriptable face of the rotation, verified live on the
dongle: `--snap-after 2` snapped at ~2.7 s wall clock (open + warm-up
included), `sleep 3 | rnkg --snap` snapped the moment the pipe closed,
`--snap-after 10 -n 44 -a all` rotated ten seconds and emitted a valid
44-char candidate; `-k 32` works; the three option-conflict validations
print and exit 2. Found and fixed en route: password candidates come
from gcry secure memory and must not be g_free()d — copy out and
rnkg_secure_string_free() the original (SIGABRT otherwise).

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

**DC spike, measured — decision: no offset tuning.** Averaged PSD over
127 MB of the 1300 MHz / gain 49.6 capture: the spur is +35 dB over the
floor but confined to a single 31 Hz bin (4.8 % of total power), and in
the time domain it is nothing but a constant −0.13 LSB offset on I and Q —
exactly the `DC −0.0005` the engine already reports and MCV already
charges for. Offset tuning would buy back hundredths of a bit per sample
at the cost of a more complicated source. Strongest non-DC spur: +9.8 dB
over floor in one bin, negligible.

**NIST reference-tool cross-check** (`ea_non_iid`, SP800-90B_EntropyAssessment
built 2026-08-19 from git, samples 4M–5M of each capture):

| Capture | our MCV | NIST H_original | NIST H_I (full suite) | our credit (MCV/2) |
|---|---|---|---|---|
| 1300 MHz, gain 49.6 | 0.83 | 0.803 | 0.662 | 0.42 |
| 1300 MHz, gain 12.5 | 0.62 | 0.580 | 0.514 | 0.31 |

Reading: plain MCV tracks NIST's H_original closely (as it should — same
estimator), the full suite's minimum lands at ~0.8× MCV, and our halving
margin sits well under that on both operating points. The 0.5 margin is
now empirically supported on this hardware, not just assumed.

**Dongle pulled mid-collection — verified, clean.** Richard pulled the
V4 at block 1035 of a slow run (250 kS/s, ~94 % of an 82 Mb target).
librtlsdr printed its own register-error noise, then the run ended
immediately through our error path (`rtlsdr_read_sync() failed — device
removed?`), exit 1, no hang, no crash, nothing generated. The library
returns an error on removal on its own, so the watchdog deadline never
had to fire; the timeout path stays unverified until a device wedges
silently for real. Side observation from the same run: at 250 kS/s the
stream measures 0.56–0.57 b/sample against 0.83–0.86 at 2.048 MS/s.

**Still open in M1:** the watchdog timeout path (needs a silently wedged
device); the one unexplained 100 MHz pass above.

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
