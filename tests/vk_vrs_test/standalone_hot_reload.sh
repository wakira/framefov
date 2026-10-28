#!/usr/bin/env bash
# Hot reload in standalone mode: a Vulkan-only app runs with the layer in
# standalone mode while settings.ini is rewritten; the layer must pick the new
# values up (log line, and the final rate map is rendered with the new radii).
set -uo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../.." && pwd)"
bin="$root/build/tests/vk_vrs_test"

if [[ ! -x "$bin" ]]; then
  echo "build first: cmake --build build" >&2
  exit 1
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

export VK_LAYER_PATH="${VK_LAYER_PATH:-$root/build}"
export FRAMEFOV_ENABLE=1
export XDG_CONFIG_HOME="$tmp/config"
export XDG_STATE_HOME="$tmp/state"
mkdir -p "$XDG_CONFIG_HOME/framefov"

write_cfg() {  # $1 = fixed inner radius
  cat > "$XDG_CONFIG_HOME/framefov/settings.ini" <<INI
[global]
standalone = 1
eye_size = 1024x1024
fixed_inner_radius = $1
fixed_outer_radius = 0.45
INI
}

log="$XDG_STATE_HOME/framefov/framefov.log"
frames=400000
for attempt in 1 2 3; do
  write_cfg 0.25
  rm -rf "$XDG_STATE_HOME/framefov"
  "$bin" dynamic --frames="$frames" > "$tmp/out.txt" 2>&1 &
  app=$!
  sleep 1.2
  if ! kill -0 "$app" 2>/dev/null; then
    wait "$app" 2>/dev/null
    echo "app finished too early; retrying with $((frames * 3)) frames" >&2
    frames=$((frames * 3))
    continue
  fi
  # Rewrite the config while the app is rendering: a much larger sharp spot.
  write_cfg 0.70
  sleep 0.5
  if ! kill -0 "$app" 2>/dev/null; then
    # The rewrite landed after the app stopped recording, so the reload could not
    # be observed: that is this script being too short, not the layer failing.
    wait "$app" 2>/dev/null
    echo "app stopped before the rewrite; retrying with $((frames * 2)) frames" >&2
    frames=$((frames * 2))
    continue
  fi
  wait "$app"
  rc=$?
  if [[ $rc -ne 0 ]]; then
    echo "FAIL: the render no longer matches the new radii (rc=$rc)" >&2
    exit 1
  fi
  if strings "$log" 2>/dev/null | grep -q "standalone config reloaded"; then
    reloaded="$(strings "$log" | grep -m1 "standalone config reloaded")"
    spot="$(sed -n '/rate map/,/center code/p' "$tmp/out.txt" | sed -n '7p' | tr -cd '.' | wc -c)"
    echo "PASS: $reloaded"
    echo "      final map centre row sharp width: $spot texels"
    exit 0
  fi
  if [[ $attempt -lt 3 ]]; then
    echo "no reload line within this run; retrying with $((frames * 2)) frames" >&2
    frames=$((frames * 2))
    continue
  fi
  echo "FAIL: no 'standalone config reloaded' after the file changed" >&2
  exit 1
done

echo "SKIP: could not keep the app running long enough to test the reload" >&2
exit 1
