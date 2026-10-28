#!/usr/bin/env bash
# Watch a Steam game attempt with the layer, live.
#
#   scripts/steam-game-check.sh <appid> [extra launch-option pairs]
#
# 1. paste the printed launch options into the game's Steam properties
#    (Properties -> General -> Launch Options), replacing what is there;
# 2. start the game;
# 3. this script streams, live, the lines that matter: our layer's own log and
#    the interesting parts of Proton's log (loader/layer messages, vrclient,
#    OpenXR, DXVK instance creation, crashes).
#
# Ctrl-C stops watching; the files are kept for later inspection.
set -uo pipefail

appid="${1:?usage: steam-game-check.sh <appid> [extra VAR=VAL ...]}"
shift || true
extra="$*"

opts="DISABLE_LSFG=1 FRAMEFOV_ENABLE=1 FRAMEFOV_LOG_STDERR=1 FRAMEFOV_LOG_LEVEL=debug"
opts="$opts VK_LOADER_DEBUG=error,warn,layer PROTON_LOG=1"
opts="$opts WINE_CPU_TOPOLOGY=8:8,9,10,11,12,13,14,15"
[[ -n "$extra" ]] && opts="$opts $extra"
opts="$opts %command%"

cat <<EOF
== Launch options for app $appid (paste these into Steam):

  $opts

   Variant worth trying afterwards:
     ... FRAMEFOV_DISABLE_VK=1   (layer loaded, inert)

== Watching logs now; start the game in Steam.
EOF

layer="${XDG_STATE_HOME:-$HOME/.local/state}/framefov/framefov.log"
proton="$HOME/steam-$appid.log"
mkdir -p "$(dirname "$layer")"
touch "$layer" "$proton" 2>/dev/null
echo "   layer:  $layer"
echo "   proton: $proton"
echo

# Our own lines, prefixed, as they are written (new lines only).
tail -n0 -F "$layer" 2>/dev/null | sed -u 's/^/[layer ] /' &
# The interesting Proton side: layer loading, VR stack, instance creation, crashes.
tail -n0 -F "$proton" 2>/dev/null | grep -u -iE \
  "framefov|vulkan loader|implicit layer|explicit layer|validate|vrclient|openxr|dxvk(instance)?|failed to create|cannot load|error|wine: |crash" | sed -u 's/^/[proton] /' &
wait
