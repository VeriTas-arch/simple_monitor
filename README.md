# Simple Monitor

A minimal Windows taskbar-adjacent status overlay built with Win32 APIs. It is an
independent top-level window, not an Explorer taskbar component or DeskBand.

## Build with MinGW

The checked-in `w64devkit-release` preset pins this Windows checkout to the
toolchain under `D:/Dependency/w64devkit`, including GCC, windres, binutils,
and Ninja. It also skips the IPO/LTO probe because the packaged compiler is
built without LTO support. Configure it with:

```pwsh
cmake --preset w64devkit-release
cmake --build build
```

When replacing the toolchain behind that path or migrating an older build
directory, use `cmake --fresh --preset w64devkit-release` once so the compiler
and auxiliary-tool entries in `CMakeCache.txt` are regenerated together.

The executable is generated at:

```bash
build/simple_monitor.exe
```

The taskbar-visibility beta is generated separately at
`build/simple_monitor_dev.exe`. Its hidden controller survives Shell changes
and keeps one taskbar-owned overlay for each committed Explorer taskbar
generation. A single reconciler owns its style, position, rendering, and final
show/hide commit; transient or failed observations preserve the last committed
visibility decision. Suppression transitions use short enter/exit dwell times,
and a hidden overlay is presented successfully before it is shown again.
Refreshing stale content on an already visible overlay never recommits its
visibility or churns its style and position. The overlay follows the taskbar
owner, suppresses itself when the taskbar has no
meaningful visible area, and uses the Windows notification state plus
full-monitor coverage to identify presentation mode.
Build only that target with:

```pwsh
cmake --build build --target simple_monitor_dev
```

Run the platform-independent visibility-policy tests with:

```pwsh
ctest --test-dir build --output-on-failure
```

The test suite includes focused reducer tests plus table-driven trace replay for
PowerPoint transitions, Start/Search transients, screenshot freeze, taskbar
auto-hide, Explorer restart, display/DPI repair decisions, and the
present-before-show sequence gate. These tests do not replace validation on an
interactive Windows desktop.

For the usual edit-build-run loop during development, use:

```pwsh
.\dev-rebuild.ps1
```

It stops any running `simple_monitor_dev.exe` and rebuilds only the dev target.
Start the new executable manually after the build completes.

## Development status

`simple_monitor_dev` remains the taskbar-visibility beta; `simple_monitor` is
the stable implementation. The dev implementation should be promoted only
after all of the following are true:

- A clean local Release build compiles stable, dev, and both policy-test targets
  without warnings.
- Interactive checks pass for normal startup, Start/Search, Snipping Tool,
  PowerPoint slideshow entry/exit, taskbar auto-hide, Explorer restart, and
  display/DPI changes.
- Fresh dev logs show the expected suppression and visibility commits, healthy
  owner/generation state, and no unexplained warnings or failure counters.
- CMake, manifest, release notes, and the Git tag are assigned one consistent
  release version as part of the promotion commit.

## Current MVP

- Shows upload, download, CPU, RAM, GPU, and disk state.
- Repositions near the taskbar and tries to sit to the left of the tray area.
- Uses DirectWrite/Direct2D with per-pixel alpha so only text is visible over
  the taskbar.
- Handles DPI, display changes, and Explorer taskbar recreation.
- Provides a tray menu for opening/reloading config, click-through mode,
  startup, and exit.

GPU and disk usage use Windows PDH counters. If a counter is unavailable on a
machine or Windows language setup, the value is shown as `--`.

Network speed uses a compact format such as `1.2KB/s`, `12KB/s`, or `1.2MB/s`
to keep the taskbar layout stable while values change.

## Configuration

The checked-in CMake configuration points both the stable and dev executables
to `config\simple_monitor.ini` in this repository, outside the disposable
`build` directory. The local config is ignored by Git, while
`config\simple_monitor.ini.example` remains tracked as the template.
The repository path is compiled into the executables; after moving the checkout,
reconfigure and rebuild with the preset so they pick up the new absolute path.

If the repository config is missing, an existing `simple_monitor.ini` beside
the executable is copied there without deleting the old file. If neither file
exists, the program copies the tracked example or creates a minimal config as a
final fallback. An existing repository config is never overwritten at startup.

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
debug_log=0
log_level=info
show_key_widget=1
```

Width is calculated from the displayed content. Use `font_size` to tune text size. Use `column_gap` as the default spacing, or
override it with `gap_after_network` and `gap_after_system`. Use
`offset_right` to tune the distance from the tray area. After editing the file,
use the tray menu's `Reload config` command or restart the program.

`show_key_widget=1` adds a compact `CAP INS NUM` status column after the disk
column. Active keys are white; inactive keys are dimmed.

Network arrow styles are `thin` (`↑`/`↓`), `triangle` (`▲`/`▼`), `heavy`
(`⬆`/`⬇`), and `chevron` (`▴`/`▾`).

Set `debug_log=1` to write event-oriented diagnostics next to the executable.
The stable build writes `debug.log`; the dev build writes `debug-dev.log`, so
their files and rotations cannot interfere. Writes and rotations are serialized
across processes; if several instances of the same build run together, their
session-tagged records can share that build's active file. The log records
placement changes, tray layout events, fullscreen or presentation suppression
context, overlay restoration state, and screenshot freeze transitions. Each
UTF-8 record contains a timestamp, severity, process ID, thread ID, session ID,
sequence number, monotonic elapsed time, and `event=...` fields. Use
`log_level=debug`, `info`, `warning`, or `error` to control detail.

On startup, the previous active file is normally retained as the corresponding
`.1` backup. The active log attempts rotation at 2 MiB; if rotation fails, the
logger preserves the current file, reports the logger failure, and retries later.
When the dev build is launched with `--startup`, it also appends structured
process-performance checkpoints to `build/startup-perf-dev.log`. This file is
not reset by manual restarts, rotates to `.1` at 256 KiB, and records the
`logging_ready`, `controller_ready`, `overlay_ready`, `monitor_ready`, and 30-second `settled`
stages. Records include cumulative process CPU time, process I/O byte counts,
PDH initialization duration, and the observed GPU instance count. `overlay_ready`
marks the first presented overlay before deferred PDH provider initialization;
`monitor_ready` marks completion of that initialization. Process I/O
counters do not include every form of memory-mapped image paging, so use a WPR
boot trace when Windows-equivalent CPU and disk attribution is required.
Repeated sustained failures are rate-limited with suppressed counts and duration.
Rapid failure/recovery flapping is sampled using the same cooldown, with one
recovery record for each reported failure period. At `info` or `debug` level,
the dev build also writes a 60-second health record containing
overlay visibility, ownership, z-order style, committed and candidate
suppression state, desired visibility, decision and presentation sequences,
frame state, repair/refresh counts, and the ages of render, present, placement,
and metrics activity.
For an overlay disappearance, inspect `suppression_candidate`,
`overlay_visibility_commit`, `overlay_refresh`, `overlay_invariant_failed`,
`overlay_repair`, `health`, `render_failed`, `present_failed`, and `timer_gap`
in sequence.

Raw performance samples, key states, and the full command line are not logged.
Logs do include the configuration path plus relevant process basenames and
window classes. When a dev suppression decision changes, its context record can
also include truncated, line-sanitized foreground and root window titles.
Review logs before sharing them outside your system.

`Start with Windows` writes the current executable path under `HKCU Run`.
The stable executable uses `SimpleMonitor`; the dev executable uses
`SimpleMonitorDev`, so the two builds do not overwrite each other's setting.
The app applies a short startup warmup before using its fullscreen-window
suppression heuristic, so Explorer and the taskbar have time to settle.
Disabling startup removes the corresponding value.

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
