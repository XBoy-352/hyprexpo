#!/usr/bin/env bash
set -euo pipefail

# Launch a nested Hyprland session that loads the local hyprexpo.so,
# so you can test changes without restarting your main session.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
SO="$REPO_ROOT/hyprexpo.so"
CONF="${XDG_CACHE_HOME:-$HOME/.cache}/hyprexpo-dev.conf"

if [[ ! -f "$SO" ]]; then
  echo "[run-nested] Build first: make -C $REPO_ROOT" >&2
  exit 1
fi

mkdir -p "$(dirname "$CONF")"
cat > "$CONF" <<EOF
monitor=,preferred,auto,auto

# load local build
plugin = $SO

plugin {
  hyprexpo {
    # layout + visuals
    columns = 3
    gaps_in = 20
    bg_col = rgb(101010)
    workspace_method = center current
    skip_empty = 0

    # borders (hypr-style gradient, thicker to showcase)
    border_style = hyprland
    border_width = 4
    border_color_current = rgb(66ccff)
    border_color_focus   = rgb(ffcc66)

    # keyboard nav
    keynav_enable = 1
    keynav_wrap_h = 1
    keynav_wrap_v = 1
    keynav_reading_order = 0

    # labels (numbers): smaller font, rounded background bubble
    label_enable = 1
    label_font_size = 12
    label_position = bottom-right
    label_offset_x = 8
    label_offset_y = 8
    label_show = hover+focus
    label_color_default = rgb(ffffff)
    label_color_hover   = rgb(72ff7a)
    label_color_focus   = rgb(ffcc66)
    label_color_current = rgb(66ccff)
    label_scale_hover = 1.0
    label_scale_focus = 1.2
    label_bg_enable = 1
    label_bg_color = rgba(000000cc)
    label_bg_rounding = 999  # fully rounded bubble
    label_padding = 6

    # outer margin around the grid to demo spacing from screen edge
    gaps_out = 20

    # demo hyprland style gradient borders
    border_grad_current = rgba(33ccffee) rgba(00ff99ee) 45deg
    border_grad_focus   = rgba(ffdd44ee) rgba(22aaffee) 30deg
  }
}

# toggle with an unmodified function key to avoid host grabs
bind = , F10, hyprexpo:expo, toggle

# submap for keyboard nav (the plugin auto-enters this when open)
submap = hyprexpo
  bind = , left,  hyprexpo:kb_focus, left
  bind = , right, hyprexpo:kb_focus, right
  bind = , up,    hyprexpo:kb_focus, up
  bind = , down,  hyprexpo:kb_focus, down
  bind = , return, hyprexpo:kb_confirm
  bind = , 1, hyprexpo:kb_selectn, 1
  bind = , 2, hyprexpo:kb_selectn, 2
  bind = , 3, hyprexpo:kb_selectn, 3
  bind = , 4, hyprexpo:kb_selectn, 4
  bind = , 5, hyprexpo:kb_selectn, 5
  bind = , 6, hyprexpo:kb_selectn, 6
  bind = , 7, hyprexpo:kb_selectn, 7
  bind = , 8, hyprexpo:kb_selectn, 8
  bind = , 9, hyprexpo:kb_selectn, 9
  bind = , 0, hyprexpo:kb_selectn, 0
submap = reset
EOF

echo "[run-nested] Launching nested Hyprland with $CONF"
exec env WLR_BACKENDS=wayland WLR_RENDERER=pixman HYPRLAND_NO_LOGO=1 Hyprland -c "$CONF"
