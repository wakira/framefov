#!/usr/bin/env bash
# Gaze sweep smoke test: the full-rate blob in the injected rate map must follow
# FRAMEFOV_FAKE_GAZE. Run from the repository root inside the dev shell:
#
#   nix develop -c bash tests/vk_vrs_test/gaze_sweep.sh
set -uo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../.." && pwd)"
bin="$root/build/tests/vk_vrs_test"

if [[ ! -x "$bin" ]]; then
  echo "build first: cmake --build build" >&2
  exit 1
fi

export FRAMEFOV_ENABLE=1
export FRAMEFOV_FORCE_ACTIVE=1
export FRAMEFOV_FAKE_EYE=1024x1024
export VK_LAYER_PATH="$root/build"
export XDG_STATE_HOME="$root/build/test-state"
# A compact full-rate blob: its centroid is then not biased by clipping at the
# image border, so it can be compared with the injected gaze directly.
export FRAMEFOV_FIXED_INNER_RADIUS=0.15
export FRAMEFOV_FIXED_OUTER_RADIUS=0.35

fail=0
for gaze in 0.25,0.30 0.75,0.70 0.50,0.50 0.20,0.80; do
  for mode in dynamic rp1 rp2 multiview imageless; do
    out="$(FRAMEFOV_FAKE_GAZE="$gaze" "$bin" "$mode" 2>&1)"
    verdict="$(printf '%s\n' "$out" | grep -E '^(PASS|FAIL)' | head -1)"
    centroid="$(printf '%s\n' "$out" | grep 'centroid' | sed 's/^ *//')"
    printf '%-9s gaze %-9s %-5s %s\n' "$mode" "$gaze" "${verdict%%:*}" "$centroid"
    case "$verdict" in
      PASS:*) ;;
      *) fail=1 ;;
    esac
  done
done
exit $fail
