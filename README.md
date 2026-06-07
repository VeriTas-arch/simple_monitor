# Simple Monitor

A minimal Windows taskbar-adjacent status overlay built with Win32 APIs. It is an
independent top-level window, not an Explorer taskbar component or DeskBand.

## Build with MinGW

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

The executable is generated at:

```text
build/simple_monitor.exe
```

## Current MVP

- Shows upload, download, CPU, RAM, GPU, and disk state.
- Repositions near the taskbar and tries to sit to the left of the tray area.
- Uses DirectWrite/Direct2D with per-pixel alpha so only text is visible over
  the taskbar.
- Handles DPI, display changes, and Explorer taskbar recreation.
- Provides a tray menu for repositioning, click-through mode, startup, and exit.

GPU and disk usage use Windows PDH counters. If a counter is unavailable on a
machine or Windows language setup, the value is shown as `--`.

Network speed uses a compact format such as `1.2KB/s`, `12KB/s`, or `1.2MB/s`
to keep the taskbar layout stable while values change.

## Configuration

The program reads `simple_monitor.ini` from the executable directory. For
example, when running `build\simple_monitor.exe`, place the config at
`build\simple_monitor.ini`.

```ini
[layout]
content_padding_x=8
column_gap=28
gap_after_network=14
gap_after_system=28
gap_after_disk=12
offset_right=8
font_size=13
key_font_size=13
network_arrow_style=thin
network_arrow_font_size=17
network_arrow_gap=3
show_key_widget=1
```

Width is calculated from the displayed content. Use `font_size` to tune text size. Use `column_gap` as the default spacing, or
override it with `gap_after_network` and `gap_after_system`. Use
`offset_right` to tune the distance from the tray area. After editing the file,
use the tray menu's `Reposition` command or restart the program.

`show_key_widget=1` adds a compact `CAP INS NUM` status column after the disk
column. Active keys are white; inactive keys are dimmed.

Network arrow styles are `thin` (`↑`/`↓`), `triangle` (`▲`/`▼`), `heavy`
(`⬆`/`⬇`), and `chevron` (`▴`/`▾`).

## Design boundary

This intentionally does not inject into Explorer, subclass taskbar windows, or
use DeskBand. It is a lightweight overlay that visually aligns with the taskbar.

## Technical route

Recommended route for this project:

1. Keep the main product as a Win32 overlay window owned by the taskbar window.
2. Treat taskbar integration as positioning and z-order management, not Explorer
   component integration.
3. Keep metrics collection independent from rendering.
4. Add persistence only for user-facing choices: monitor, width, right offset,
   opacity, click-through, refresh interval, and startup.
5. Add diagnostic logging before adding Shell hacks, so positioning failures can
   be reproduced from taskbar rect, tray rect, DPI, and monitor data.

The overlay route should handle:

- normal foreground app switching,
- Explorer taskbar recreation,
- DPI and resolution changes,
- tray-icon recovery,
- user-controlled offsets when Windows 10/11 taskbar internals differ.

The window is an owned popup of `Shell_TrayWnd`. That is different from being a
child control inside Explorer: it keeps z-order behavior close to the taskbar
without injecting into Explorer or joining the taskbar layout engine.

Avoid making these part of the default path:

- Explorer injection,
- DeskBand,
- subclassing taskbar child windows,
- depending on fixed Windows 11 taskbar child-window class layouts.

Those can be experimental compatibility modes later, but they should not be
required for the monitor to work.

If the overlay disappears only when switching Windows virtual desktops, that is
a different class of problem: ordinary top-level windows are desktop-scoped.
Topmost maintenance is not enough for that case.
