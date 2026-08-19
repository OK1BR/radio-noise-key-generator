# Scope — what this project still needs

Written 2026-08-19, at the end of M0. This is the working list for whoever
picks the project up; `docs/SPEC.md` says *what* the program is and why,
this says *what is left to build*.

Read in this order: `CLAUDE.md` (the four rules), `docs/SPEC.md` (design),
`docs/STATUS.md` (what is verified and what is not), then this.

---

## Where it stands

M0 is done: engine, CLI, three gates, all passing, clean build at
`warning_level=2`. Verified against files; **never against a dongle**. The
`HAVE_LIBRTLSDR` branch of `rnkg-source.c` has not even been compiled.

```
meson setup build && meson compile -C build && meson test -C build
```

---

## M1 — hardware bring-up

The whole point of M1 is to stop guessing about the real signal chain. Until
this is done, every number in the README about what a dongle measures is
hypothetical.

### 1.1 Install and permissions

`extra/rtl-sdr` (1:2.0.2-1) is **not installed** — needs Richard's explicit
go-ahead first, it is a system change. What it brings, from `pacman -Fl`:

| File | Why it matters |
|---|---|
| `usr/lib/librtlsdr.so` + `pkgconfig` | what meson needs for `HAVE_LIBRTLSDR` |
| `usr/lib/modprobe.d/rtlsdr.conf` | blacklists the kernel DVB driver |
| `usr/lib/udev/rules.d/10-rtl-sdr.rules` | non-root access to the device |
| `usr/lib/sysusers.d/rtl-sdr.conf` | creates the group the udev rule grants to |
| `usr/bin/rtl_test`, `rtl_sdr`, `rtl_power` | independent cross-check tools |

Two things to verify **after** installing, not assume:

- `dvb_usb_rtl28xxu` is present in this kernel (7.1.8-arch1-3) and nothing
  currently blacklists it. If it grabs the dongle first, `rtlsdr_open()`
  fails with a device-busy error. Check the packaged modprobe.d actually
  covers it, and that the module is not already loaded from a previous plug.
- Whether `rfa` needs adding to the group `sysusers.d` creates. If yes, that
  is a second system change and needs its own go-ahead.

### 1.2 Compile the branch that has never compiled

`rnkg-source.c` under `HAVE_LIBRTLSDR`. Expect the usual first-build
problems there and nothing more exotic: `rtlsdr_get_tuner_gains()` signature,
whether gains really come back ascending (`highest_gain()` assumes the last
element is the largest — verify against `rtl_test`, do not trust the
assumption), and the `int`-vs-`gsize` chunking in `rnkg_source_read()`.

### 1.3 What is missing in the engine for real hardware

Known gaps, in order of how much they matter:

- **Warm-up discard.** After `rtlsdr_reset_buffer()` the first samples come
  from a tuner that has not settled. They are not noise, they are a
  transient. Discard the first block before crediting anything. Not
  implemented at all right now.
- **Startup health test.** SP 800-90B §4.3 wants a fixed number of samples
  (1024 minimum) tested before *any* output is produced. Currently the first
  block happens to do this, but only by accident of block size. Make it
  explicit, so shrinking `RNKG_BLOCK_BYTES` later cannot silently remove it.
- **Read watchdog.** `rtlsdr_read_sync()` on a wedged device can block
  indefinitely. The CLI can hang forever today. Needs a timeout, and the GUI
  needs it more than the CLI does.
- **DC spike.** The RTL2832U puts a large spur at the centre of the
  passband. It biases the sample distribution and MCV will see it as reduced
  min-entropy — which is conservative, so it is not a safety problem, but it
  may cost real entropy for no reason. Measure first; only then decide
  whether to offset-tune away from it.

### 1.4 Measurements that go into STATUS.md

This is the actual deliverable of M1 — numbers, not code:

- Measured min-entropy per sample at several frequencies (1300 MHz default,
  plus a deliberately bad choice like an FM broadcast channel for contrast)
  and at several gains, including gain 0.
- Serial and I/Q correlation on real samples. **The thresholds of 0.05 in
  `rnkg-health.h` were chosen against synthetic uniform noise.** Real
  hardware may sit closer to them. If real thermal noise trips them, the
  threshold is wrong, not the hardware — but find out which before changing
  a number that exists to catch a carrier.
- How the program behaves when the dongle is pulled mid-collection.
- A capture through the NIST reference tool
  ([SP800-90B_EntropyAssessment](https://github.com/usnistgov/SP800-90B_EntropyAssessment)),
  its min-entropy estimate next to ours. They should be close, and ours
  should be the lower of the two after the credit margin.
- Wall-clock time to collect enough for a 256-bit key, so the GUI knows
  whether it needs a progress bar or just a spinner.

---

## M2 — GTK4 front end

**Why a GUI exists at all:** the live spectrum. It is the visual proof that
what is being sampled is an empty channel and not a carrier — the one thing
`rtl_entropy` never had and the reason a person can trust the result without
reading the health-test output. Build the spectrum first; if it does not
work, the GUI has no reason to exist.

### 2.1 Structure

`src/app/`, following `pass-for-linux`: `main.c` (AdwApplication bootstrap),
`window.c`, plus one file per view. The engine does not change for the GUI —
`rnkg_collector_step()` already exists to be driven from an idle callback so
the window keeps repainting. If the GUI needs an engine change, that is a
signal the split is wrong; fix the split.

### 2.2 The spectrum needs an FFT — decision required

The engine has no FFT and no dependency that provides one. Three ways, and
this needs deciding before any UI code is written:

- a small radix-2 FFT written into the engine (~100 lines, no new
  dependency, and 1024 points for a display is not demanding);
- `fftw3` (fast, packaged, but a real dependency on a program whose entire
  point is being auditable);
- reuse whatever `sdr-for-linux` already does, for consistency across the
  family.

The third is worth looking at first — the family already solved this.

### 2.3 Screens

- **Source panel:** device picker (`rnkg_source_device_count()` /
  `_device_name()` already exist), frequency, gain, sample rate.
- **Spectrum:** GtkDrawingArea, peak hold, and a clear marker for where the
  tuner sits. Black background, family style.
- **Health panel:** one indicator per test (RCT, APT, serial, I/Q), live.
  When one fails, say *which* and what to do about it — the strings in
  `rnkg_health_verdict_message()` are already written for a human.
- **Entropy counter:** credited bits against the target.
- **Generation panel:** length, alphabet, count; reveal/hide; copy with an
  auto-clear timer, as `pass-for-linux` does it.

### 2.4 Handling of secrets in the UI

Same rule as `pass-for-linux`: nothing generated reaches persistent storage,
buffers are `gcry_malloc_secure()`, clipboard clears on a timer. A GUI makes
this harder than a CLI — a GtkLabel holding a password is a heap allocation
outside secure memory. Decide deliberately how far to take this and write
the decision into SPEC.

---

## M3 — packaging

Last, and only when Richard asks. `pass-for-linux` had M6 packaging
postponed deliberately in favour of manual use; expect the same here.

- `data/cz.ok1br.radio_noise_key_generator.svg` and `-symbolic`, Papirus
  style, matching the rest of the family.
- `.desktop`, metainfo, `data/meson.build` (copy the pattern from
  `pass-for-linux`).
- AUR package, alongside the family's other three.
- Install to `~/.local`, never `/usr`.

---

## Cross-cutting: still open

Decisions that are not blocking M1 but should not be forgotten:

- **App id** is `cz.ok1br.radio_noise_key_generator` — long. Confirm before
  it appears in a `.desktop` file and becomes awkward to change.
- **GitHub repo** not created yet. Needs a name (the directory is
  `radio-noise-key-generator`), description, topics, and Richard's explicit
  go-ahead — it is a step outward.
- **Diceware passphrase mode?** The keyboard-layout argument behind the
  letters-only default applies differently to word lists.
- **Write into a `pass` store directly** via `pass-for-linux`'s engine, or
  stay a generator that hands over a string?
- **Export the last capture** so a user can hand it to the NIST tool without
  re-running `rtl_sdr`. Against: key-adjacent material staying in memory
  longer than necessary.
- **More of §6.3.** Right now only the MCV estimator is implemented and its
  result is halved. Implementing the collision, Markov, compression and
  prediction estimators and taking the minimum — as the standard actually
  requires — would let the 0.5 margin be justified rather than assumed.

---

## Traps worth writing down

Things already hit once here, or certain to be hit:

- **Integer width in rejection sampling.** For hex and base64 the rejection
  threshold is exactly 256; a `guint8` wraps it to zero and the generator
  loops forever. Cost an hour in M0.
- **Secure memory pool sizing.** `gcry_control(GCRYCTL_INIT_SECMEM, ...)`
  must cover a whole collection block (256 KiB) plus the sponge plus the
  secret. It was 64 KiB at first and the collector could not allocate.
- **The MCV worked example is the test.** Anyone tempted to "simplify" the
  confidence bound should run `meson test` — the SP 800-90B example pins
  p_u = 0.6895 exactly.
- **The correlation test earns its place.** A stream can pass both approved
  tests and still be half-correlated with itself; there is a gate for
  exactly that case (`/health/structure-correlated`). Do not remove it as
  redundant.
