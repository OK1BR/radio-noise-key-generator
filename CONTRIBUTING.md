# Contributing

Thanks for your interest — but please read this before opening anything.

## Pull requests are not accepted

This project does **not** accept pull requests. It generates key material:
every rule in the engine exists because NIST SP 800-90B says so or because
a measurement on real hardware says so, and the four rules that override
convenience are written down in [CLAUDE.md](CLAUDE.md) precisely so nobody
"simplifies" them away. Drive-by changes to entropy accounting are how
random-number generators rot. Unsolicited PRs will be closed without
review, regardless of quality.

## What IS welcome: issues

- **Bug reports** — dongle model and tuner, distro, the exact command, and
  the stderr output (`-v` prints every measurement).
- **Measurements from other hardware** — the single most valuable
  contribution. The credit margin is justified by measurements on an
  RTL-SDR Blog V4 (R828D); numbers from other tuners, frequencies, gains
  and sample rates either confirm it or catch it. Best of all: a capture
  (`rtl_sdr` + `rnkg --file -v`) with a cross-check against the
  [NIST reference tool](https://github.com/usnistgov/SP800-90B_EntropyAssessment),
  or a real-world stream that fools one of the health tests.
- **Feature requests** — [`docs/SPEC.md`](docs/SPEC.md) §7 lists what is
  deliberately still open (diceware mode, pass-store hand-off, capture
  export); operating experience with those is useful input.

## Why so strict?

The design contract is [`docs/SPEC.md`](docs/SPEC.md) and the honest state
of verification is [`docs/STATUS.md`](docs/STATUS.md): what is claimed is
measured, what is not measured is not claimed. That discipline is
incompatible with drive-by code contributions — but it is exactly why the
output is worth trusting. Feedback, testing and measurements are very
welcome.
