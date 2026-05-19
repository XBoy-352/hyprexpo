#!/usr/bin/env bash
set -euo pipefail

# Rebuilds hyprexpo.so on source changes and relaunches a nested Hyprland
# that loads the local build. No impact on your main session.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
SO="$REPO_ROOT/hyprexpo.so"
CONF="${XDG_CACHE_HOME:-$HOME/.cache}/hyprexpo-dev.conf"

gen_conf() {
  cat > "$CONF" <<EOF
monitor=,preferred,auto,auto
plugin = $SO
plugin {
  hyprexpo {
    border_style = hyprland
    tile_rounding = 12
    tile_rounding_focus = 16
    tile_rounding_current = 14
  }
}
bind = , F10, hyprexpo:expo, toggle
submap = hyprexpo
  bind = , left,  hyprexpo:kb_focus, left
  bind = , right, hyprexpo:kb_focus, right
  bind = , up,    hyprexpo:kb_focus, up
  bind = , down,  hyprexpo:kb_focus, down
  bind = , return, hyprexpo:kb_confirm
submap = reset
EOF
}

launch_nested() {
  echo "[dev-watch] launching nested Hyprland"
  WLR_BACKENDS=wayland WLR_RENDERER=pixman HYPRLAND_NO_LOGO=1 Hyprland -c "$CONF" &
  NESTED_PID=$!
}

stop_nested() {
  if [[ -n "${NESTED_PID:-}" ]] && kill -0 "$NESTED_PID" 2>/dev/null; then
    echo "[dev-watch] stopping nested ($NESTED_PID)"
    kill "$NESTED_PID" 2>/dev/null || true
    wait "$NESTED_PID" 2>/dev/null || true
  fi
}

build() {
  echo "[dev-watch] building hyprexpo.so"
  make -C "$REPO_ROOT" -j || { echo "[dev-watch] build failed"; return 1; }
}

trap stop_nested EXIT INT TERM
gen_conf
build
launch_nested

echo "[dev-watch] watching sources... (press Ctrl-C to stop)"
command -v inotifywait >/dev/null 2>&1 || { echo "[dev-watch] please install inotify-tools"; exit 1; }

mapfile -t watch_paths < <(find "$REPO_ROOT" -maxdepth 1 -type f \( -name '*.cpp' -o -name '*.hpp' -o -name 'Makefile' -o -name 'meson.build' -o -name 'CMakeLists.txt' \) | sort)

inotifywait -qm -e close_write,move,create,delete --format '%w%f' \
  "${watch_paths[@]}" "$REPO_ROOT/tests" \
  | while read -r changed; do
      echo "[dev-watch] change detected: $changed"
      if build; then
        stop_nested
        launch_nested
      fi
    done
