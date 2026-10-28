#!/bin/sh
# Install the framefov layer for the current user (no root, no /usr writes).
#
#   scripts/install-frame.sh [build-dir]
#
# Installs:
#   ~/.local/lib/framefov/libVkLayer_framefov.so   the layer
#   ~/.local/lib/framefov/framefov-gaze            gaze publisher (on-demand)
#   ~/.local/share/vulkan/implicit_layer.d/VkLayer_framefov.json
#   ~/.config/systemd/user/framefov-gaze.service
#   ~/.config/systemd/user/framefov-gaze.service.d/wait-for-session.conf (if absent)
#
# The layer is an *implicit* layer gated on FRAMEFOV_ENABLE=1, so nothing changes
# unless a launch opts in: Steam's launch wrapper owns VK_INSTANCE_LAYERS.
set -eu

build="${1:-build}"
lib="$build/libVkLayer_framefov.so"
gaze="$build/framefov-gaze"
[ -f "$lib" ] || { echo "missing $lib (build first)" >&2; exit 1; }

prefix="$HOME/.local/lib/framefov"
manifest_dir="$HOME/.local/share/vulkan/implicit_layer.d"
manifest="$manifest_dir/VkLayer_framefov.json"
unit_dir="$HOME/.config/systemd/user"
unit="$unit_dir/framefov-gaze.service"
drop_dir="$unit_dir/framefov-gaze.service.d"
drop_in="$drop_dir/wait-for-session.conf"

mkdir -p "$prefix" "$manifest_dir" "$unit_dir"
install -m 755 "$lib" "$prefix/libVkLayer_framefov.so"
if [ -f "$gaze" ]; then install -m 755 "$gaze" "$prefix/framefov-gaze"; fi

# Replace dst with the staged file only when the content differs, so systemd does
# not report "changed on disk" on every install. Returns 0 when it changed.
install_staged() {
  staged="$1"
  dst="$2"
  if [ -f "$dst" ] && cmp -s "$staged" "$dst"; then
    rm -f "$staged"
    return 1
  fi
  mv "$staged" "$dst"
  return 0
}

changed=0

tmp="$manifest.tmp.$$"
cat > "$tmp" <<EOF
{
  "file_format_version": "1.2.0",
  "layer": {
    "name": "VK_LAYER_FRAMEFOV_foveation",
    "type": "GLOBAL",
    "library_path": "$prefix/libVkLayer_framefov.so",
    "api_version": "1.4.309",
    "implementation_version": "1",
    "description": "Steam Frame foveated rendering (VRS injection)",
    "enable_environment": { "FRAMEFOV_ENABLE": "1" },
    "disable_environment": { "FRAMEFOV_DISABLE": "1" }
  }
}
EOF
if install_staged "$tmp" "$manifest"; then :; fi

if [ -f "$prefix/framefov-gaze" ]; then
  tmp="$unit.tmp.$$"
  cat > "$tmp" <<EOF
[Unit]
Description=framefov gaze publisher (SteamVR eye tracking -> /dev/shm/framefov-gaze)
After=steamvr.service
PartOf=steamvr.service

[Service]
ExecStart=$prefix/framefov-gaze -q
Restart=on-failure
RestartSec=5

[Install]
WantedBy=default.target
EOF
  if install_staged "$tmp" "$unit"; then changed=1; fi

  # gamescope's OpenVR backend asserts if it becomes session manager after another
  # VR app has already connected. The publisher connects on demand now, so this is
  # belt-and-braces — and never overwrite a drop-in the user already has.
  if [ ! -f "$drop_in" ]; then
    mkdir -p "$drop_dir"
    tmp="$drop_in.tmp.$$"
    cat > "$tmp" <<'EOF'
# gamescope becomes the OpenVR "session manager" and aborts with
#   rendervulkan.cpp: Assertion `!modifiers.empty()' failed
# if another VR application has already connected to vrserver. Wait for the
# gamescope session unit (Type=notify) before connecting, and start with SteamVR.
[Unit]
After=gamescope-session.service

[Service]
ExecStartPre=/bin/sh -c 'for i in $(seq 1 120); do systemctl --user -q is-active gamescope-session.service && break; sleep 1; done; systemctl --user -q is-active gamescope-session.service'
ExecStartPre=/bin/sleep 3

[Install]
WantedBy=steamvr.service
EOF
    if install_staged "$tmp" "$drop_in"; then changed=1; fi
  fi
fi

if [ -n "${XDG_RUNTIME_DIR:-}" ] && command -v systemctl >/dev/null 2>&1; then
  # Always reload: cheap, and it clears any stale "changed on disk" state.
  systemctl --user daemon-reload 2>/dev/null || true
fi
[ "$changed" = "1" ] && echo "unit files updated (daemon-reload done)" || true

echo "installed:"
echo "  $prefix/libVkLayer_framefov.so"
[ -f "$prefix/framefov-gaze" ] && echo "  $prefix/framefov-gaze"
echo "  $manifest"
[ -f "$unit" ] && echo "  $unit"
[ -f "$drop_in" ] && echo "  $drop_in"
echo
echo "opt in per launch:   FRAMEFOV_ENABLE=1 %command%"
echo "eye tracking:        FRAMEFOV_ENABLE=1 FRAMEFOV_OPENVR_GAZE=1 %command%"
echo "publisher:           systemctl --user enable --now framefov-gaze   (idles until a game asks)"
echo "disable Valve's FDM layer for a clean comparison:"
echo "  VK_LOADER_LAYERS_DISABLE=VK_LAYER_VALVE_fdm_injection"
