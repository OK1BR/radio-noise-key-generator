#!/bin/sh
# Gate for the CLI snap path's §4.3 startup discard — no hardware needed.
# Exactly one block of input must yield no candidate (the startup block is
# evidence the source is healthy, not material), and two blocks must yield
# exactly one password, derived from the second.
#
# Uniform bytes from /dev/urandom pass every health test with absurd margin
# (RCT needs 68 identical bytes in a row, APT ~459 of one value in 512,
# the correlation thresholds sit ~25 sigma above what uniform data shows),
# so both runs are deterministic in practice.
set -u

RNKG="$1"
BLOCK=262144

out=$(head -c "$BLOCK" /dev/urandom \
      | "$RNKG" -f - -n 20 -a letters --snap-after 0 2>/dev/null)
if [ $? -eq 0 ] || [ -n "$out" ]; then
  echo "FAIL: a lone startup block produced a candidate" >&2
  exit 1
fi

out=$(head -c $((2 * BLOCK)) /dev/urandom \
      | "$RNKG" -f - -n 20 -a letters --snap-after 0 2>/dev/null)
if [ $? -ne 0 ]; then
  echo "FAIL: two blocks did not produce a candidate" >&2
  exit 1
fi
if ! printf '%s\n' "$out" | grep -qxE '[A-Za-z]{20}'; then
  echo "FAIL: candidate is not 20 letters" >&2
  exit 1
fi

echo "ok: startup block discarded, second block snapped"
