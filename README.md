# hypr-scrolling-rtl

Hyprland plugin for right-aligned `scrolling` workspaces (`direction = left`). Hooks Hyprland's built-in `CScrollingAlgorithm` / `CScrollTapeController` in place so overview plugins (e.g. `hyprland-scroll-overview`) and native layout dispatchers keep working.

## What it does

On scrolling workspaces with `direction = left`:

- **Right-aligned tape**: pins fitting tapes flush to the right edge instead of centering them, and spawns new windows at the rightmost column.
- **Drag-and-drop fix**: inverts outer-edge left/right column insertion for reversed scrolling so columns can be dropped on either edge.
- **Mouse resize fix**: mirrors left/right edge bounds and camera offsets for reversed scrolling, keeps the tape flush right when shrinking, and allows edge-pinned growth on both edges.

On **all** scrolling workspaces:

- **Preserve width on move**: moving a tiled window (Super+Left-click drag-and-drop or moving between scrolling workspaces) keeps its column width instead of resetting to `column_width`.
## Install

Via `hyprpm`:

```bash
hyprpm add https://github.com/ZiadAbdelati/hypr-scrolling-rtl
hyprpm enable hypr-scrolling-rtl
```

Or build manually (requires Hyprland headers via `pkg-config --cflags hyprland`):

```bash
make
hyprctl plugin load "$PWD/build/hypr-scrolling-rtl.so"
```

Rebuild after Hyprland upgrades (`make clean all` or `hyprpm update`).
