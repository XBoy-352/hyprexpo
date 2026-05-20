# Runtime Smoke Checklist

Run these checks in a nested Hyprland session or another disposable compositor session before publishing a release.

`scripts/run-nested.sh` launches a disposable nested session with a fresh user-owned build under `${XDG_CACHE_HOME:-$HOME/.cache}/hyprexpo`.

`scripts/dev-watch.sh` rebuilds and relaunches that session on source changes.

Nested test binds:

- `F10` for overview
- `SUPER+Return` for a terminal
- `SUPER+1..9` for workspaces
- `SUPER+SHIFT+1..9` to move a window
- `SUPER+Q` to close a window
- `SUPER+SHIFT+Q` to exit

## Checklist

1. Load the locally built plugin and confirm no API/hash mismatch is reported.
2. Toggle overview on and off with `hyprexpo:expo, toggle`.
3. Cancel overview with the configured `cancel_key` and confirm the workspace does not change.
4. Move keyboard focus with `hyprexpo:kb_focus`, then select with `hyprexpo:kb_confirm`.
5. Select by pointer or touch near the outside edges of the monitor.
6. Confirm current, hover, focus, label, and border styling render as configured.
7. Open two or more windows on one workspace, open overview, press on a visible window preview, drag across the move threshold, and confirm the under-pointer drag proxy appears.
8. Drag that window preview over another valid workspace tile and confirm a positional landing proxy appears inside the hovered target tile.
9. Move the pointer within the target tile and between target tiles; confirm the landing proxy tracks the pointer position, preserves the original grab offset, and disappears when hovering the source or an invalid target.
10. Release on a target workspace and confirm the window still moves through the existing safe workspace move behavior. This release is not yet a positional layout insertion.
11. Exercise the Lua gesture registration path and complete or cancel a swipe.
12. Unload or reload the plugin after closing overview and confirm no stale render pass callback crashes the session.

::: warning
The public site and docs should not claim full release readiness until this runtime smoke gate has been completed for the intended release artifact.
:::
