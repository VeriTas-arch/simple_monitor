# Repository agent instructions

## Scope and source boundaries

- These instructions apply to the entire repository.
- Keep experimental overlay lifecycle and visibility work in `src/main-dev.cpp`.
  Change the stable `src/main.cpp` only when the user explicitly requests it or
  the same verified defect affects both implementations.
- Keep changes surgical. Preserve unrelated tracked changes and untracked
  artifacts, and stage only files that belong to the current task.

## Build and tests

- The configured `build` directory is the Release/Ninja build used for the
  current dev executable. Verify `build/CMakeCache.txt` if that may have drifted.
- Run the platform-independent policy tests before restarting the GUI:

  ```powershell
  cmake --build build --target overlay_policy_tests
  ctest --test-dir build --output-on-failure
  ```

- The running executable locks `build/simple_monitor_dev.exe` on Windows. Do not
  try to relink that target behind the running process; use the restart workflow
  below. An incremental build reporting `no work to do` is a successful
  up-to-date build check.

## Interactive dev rebuild and restart

- Use the repository script for the complete stop, build, and restart cycle:

  ```powershell
  powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\dev-rebuild.ps1 -Restart
  ```

- In Codex, run that exact script command outside the sandbox with a narrowly
  scoped approval. This is required for access to the real interactive Windows
  desktop; it does not require an Administrator token or UAC elevation.
- Never launch `simple_monitor_dev.exe` directly from the ordinary sandbox. The
  sandbox can share the same Windows session ID while still being unable to see
  `Shell_TrayWnd`, so such a launch is not valid runtime evidence.
- Do not bypass the script's `Shell_TrayWnd` guard. It intentionally rejects a
  restart before stopping the current process when the caller cannot see the
  interactive taskbar.
- The script stops only the process whose executable path matches this
  repository's `build/simple_monitor_dev.exe`, performs the CMake target build,
  starts it with a hidden window, and returns the new PID.

## Runtime verification

- Build, link, and process creation are not sufficient proof. After a restart,
  inspect the fresh session in `build/debug-dev.log`.
- At minimum, confirm:
  - `overlay_created` has matching `requested_owner` and `effective_owner`, or a
    bounded `overlay_owner_binding` retry subsequently succeeds.
  - `overlay_visibility_commit` reports a successful visible commit after a new
    presentation sequence.
  - `health` reports `issues=0x00`, a valid/visible/topmost/layered overlay, the
    expected owner, and zero relevant failure counters.
  - There are no unexplained `WARN`, `ERROR`, `present_failed`, `render_failed`,
    or exhausted owner-binding records.
- Use `overlay_generation` and `present_success_sequence` as lifecycle evidence;
  do not infer continuity from HWND values or timestamps alone.
- A healthy normal-startup record does not prove Start/Search, Snipping Tool,
  PowerPoint, taskbar hiding, Explorer restart, or display/DPI transitions.
  Validate the scenarios relevant to the change on the interactive desktop.
- Suppression-context diagnostics can include foreground/root window titles.
  Review logs before sharing them outside the machine.
