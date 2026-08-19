# Status

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
