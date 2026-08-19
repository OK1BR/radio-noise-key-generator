# radio-noise-key-generator

Passwords and keys of any length, derived from radio noise picked up by an
RTL-SDR dongle — with the entropy source measured, not assumed.

Native GTK4/libadwaita application and command line tool for Linux, written
in C. Fifth app of the OK1BR family, alongside
[sdr-for-linux](https://github.com/OK1BR/sdr-for-linux),
[skimmer-for-linux](https://github.com/OK1BR/skimmer-for-linux),
[log-for-linux](https://github.com/OK1BR/log-for-linux) and
[pass-for-linux](https://github.com/OK1BR/pass-for-linux).

## Why

`/dev/urandom` is good. The interesting part is not replacing it — it is
being able to *see* where a key came from: which frequency, how much
min-entropy the samples actually carried, and which health tests passed
while they were collected.

So the radio never replaces the kernel here. Every derivation mixes in 64
bytes from `getrandom()`, unconditionally. **The output can never be weaker
than `getrandom()` alone**, whatever the dongle hands over — a dead antenna,
a driver returning constants, or someone deliberately transmitting into the
frequency you chose. The radio can only add.

## What it measures

Raw RTL-SDR samples are not entropy. They carry a DC offset, a spur in the
middle of the passband, correlation between the I and Q branches, and
periodic artefacts from USB framing. So every block is tested before it is
credited with anything:

| Test | Source | Catches |
|---|---|---|
| Repetition Count | NIST SP 800-90B §4.4.1 | source stuck on one value |
| Adaptive Proportion | NIST SP 800-90B §4.4.2 | one value becoming dominant |
| Serial correlation | §4.5 developer-defined | tuned to a signal, not to noise |
| I/Q correlation | §4.5 developer-defined | structure in the sample stream |
| Most Common Value | NIST SP 800-90B §6.3.1 | how much min-entropy is really there |

Min-entropy, not Shannon entropy: Shannon answers "how well does this
compress", min-entropy answers "how well can an attacker guess the most
likely sample" — and only the second one bounds what a key is worth. Of the
min-entropy that MCV reports, half is credited; MCV assumes i.i.d. samples
and an RTL-SDR stream is not i.i.d.

A block that fails any test is not merely skipped: collection stops. A noise
source that failed a continuous health test may not go back to producing
output without a restart.

## Build

```
meson setup build
meson compile -C build
meson test -C build
```

Dependencies: `glib-2.0`, `libgcrypt`, and `librtlsdr` for the dongle.
librtlsdr is optional — without it everything still builds and the engine
runs from captured sample files, which is how the tests work.

## Use

```
rnkg                          # 20 letters, from the dongle
rnkg -n 32 -a all             # 32 characters, letters + digits + punctuation
rnkg -k 32                    # 32 bytes of raw key material, as hex
rnkg -c 5                     # five passwords from one collection
rnkg --freq 1300 --gain 49.6  # pick the frequency and gain yourself
rnkg --file capture.bin -v    # replay a capture, show every measurement
rnkg --list                   # what dongles are present
```

Passwords go to stdout, one per line. Everything else — health tests,
entropy estimates, the source description — goes to stderr, so pipes work.

The default alphabet is letters only. A password made of letters survives
being typed on a keyboard whose layout is not the one it was generated on,
which is exactly the situation a LUKS passphrase prompt puts you in.

## Verifying the source yourself

`rnkg --file` also runs in the other direction: capture with `rtl_sdr`, then
feed the same bytes to the NIST reference tool
([SP800-90B_EntropyAssessment](https://github.com/usnistgov/SP800-90B_EntropyAssessment))
and compare its min-entropy estimate against what this program reports.

```
rtl_sdr -f 1300000000 -s 2048000 -g 49.6 -n 20000000 capture.bin
rnkg --file capture.bin -v
```

## Licence

GPL-3.0-or-later.
