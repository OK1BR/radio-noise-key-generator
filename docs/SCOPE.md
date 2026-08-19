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

Done 2026-08-19 (see STATUS.md): warm-up discard (first 256 KiB after
tuning), explicit §4.3 startup test (`RNKG_STARTUP_SAMPLES`, gated by
`rnkg-collect-test`), read watchdog (per-read thread with a deadline; the
timeout path itself is unverified — it needs a wedged device).

Also resolved 2026-08-19: the DC spike was measured (a single 31 Hz bin,
a constant −0.13 LSB offset MCV already charges for — STATUS.md has the
numbers) and the decision is **no offset tuning**.

Still open:

- **The one unexplained 100 MHz pass.** One live run out of six on an FM
  broadcast generated instead of being rejected; the retune-glitch
  hypothesis is in STATUS.md. Keep an eye out once the M2 spectrum makes
  the passband visible.

### 1.4 Measurements that go into STATUS.md

This is the actual deliverable of M1 — numbers, not code. Most of it was
measured 2026-08-19 and lives in STATUS.md: min-entropy across gains at
1300 MHz plus the FM contrast case, serial/I/Q on real samples (the 0.05
thresholds hold — empty-channel hardware measures ≤0.005 in the engine's
own arithmetic, a carrier 0.12–0.18), and wall-clock (a block is ~64 ms;
no progress bar needed).

The NIST cross-check is done too (2026-08-19, table in STATUS.md): plain
MCV tracks the tool's H_original, the full-suite minimum lands at ~0.8×
MCV, and our halved credit sits under all of it on both operating points.
The dongle-pull test as well (same day): a mid-collection unplug ends the
run cleanly through the library's error return — details in STATUS.md.

Nothing is left in 1.4.

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

### 2.3 Screens — revised 2026-08-19 with Richard, built the same day

The first draft (source panel with device/freq/gain/rate, a seven-field
health row) was cut on Richard's feedback: the GUI is a generator, not an
SDR console. What stands now is specified in SPEC §8: spectrum, one status
line, and the generation panel with live rotation and Snap. All built;
interactive verification by Richard pending.

### 2.4 Handling of secrets in the UI

Decided and written into SPEC §8: display in ordinary heap is accepted
(showing candidates is the point), everything the program controls is
wiped, the clipboard clears itself after 45 s if it still holds our text.

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
  (The NIST-tool cross-check of 2026-08-19 found the full suite's minimum
  at ~0.8× MCV on this hardware, so the 0.5 margin has empirical support
  now; implementing the estimators would make it structural.)

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
