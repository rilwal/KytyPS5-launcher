# Launcher mode

This fork of KytyPS5 works with the [game launcher](https://github.com/rilwal/game-launcher) as a
patched emulator (the launcher's design decision D26). The launcher draws all UI; Kyty reports to
it over the launcher's plugin protocol (`docs/plugin-api.md` there) as a `session` plugin.

## How it starts

The launcher's Kyty runner starts `kyty_emulator --game <game folder>` with two environment
variables:

- `LAUNCHER_ENDPOINT`: the launcher's socket (Unix) or named pipe (Windows).
- `LAUNCHER_SESSION`: a token for this game session.

When both are set, Kyty connects before it boots (`src/launcherMode.cpp`). Without them, nothing
changes and Kyty runs as before.

## What Kyty sends and does

| When | Message |
| --- | --- |
| Connected | `booting` |
| First frame on screen | `ready {pid, window}`: the HWND, X11 window id or NSWindow number |
| Settings the launcher sent can't be applied | `notify` (error) |
| The game opens its trophy set | `achievements`: every trophy with the stored unlocks |
| A trophy (and maybe the platinum) unlocks | `unlocked {id, at}` per trophy |
| Quick menu opened or closed | the guest pauses or resumes, then `paused` |
| The window's size, with `ready` and on every change | `view {width, height}` (composited overlay) |
| The frame the quick menu opened over | `capture_surface {id, width, height}` (composited overlay) |
| A fatal error (`EXIT`, failed checks) | `failed {reason, code: "fatal_error"}` |
| The game folder or options are invalid | `failed {code: "invalid_file_or_folder"}` |
| `quit` | the window loop ends as on closing the window: the pipeline cache is saved, then Kyty exits |

In launcher mode Kyty's own trophy toasts are off, since the launcher shows them.

Trophy ids are the trophy ids as text, prefixed with the service label for any trophy set past the
first (`"1:12"`). Only the configured user's trophies (`--user-id`) are reported.

Pausing uses the same path as the OS sending the app to the background: the present thread keeps
showing the last frame and the guest's vblanks stop.

## Quick menu in the game window

On Linux, Kyty declares `composited_overlay` and draws the launcher's quick menu into its own
frames, so the menu shows over the game (fullscreen too) and the launcher's window stays where it
is. It follows the launcher's "Composited overlay" protocol (`docs/plugin-api.md` there):

- The GPU needs `VK_EXT_external_memory_dma_buf`, `VK_KHR_external_memory_fd` and
  `VK_EXT_image_drm_format_modifier`. Kyty turns them on when they're there, and sends `view` only
  then, so on a GPU without them the launcher brings its window to the front as before.
- On `overlay_opened` the guest pauses, and the next present copies the frozen frame into a linear
  dma-buf for the launcher (`capture_surface`), which draws the menu over it.
- `overlay_surface` names the launcher's socket and its buffers; Kyty fetches the dma-bufs from it
  (`src/common/launcher/overlayPorts.cpp`) and imports them into Vulkan.
- While `overlay_visible` is on, every present draws the newest buffer the launcher published over
  the whole window, after the game's frame and Kyty's own overlays
  (`src/graphics/presentation/launcherOverlay.cpp`). The paused guest keeps presenting its last
  frame, so the menu animates.

macOS (IOSurfaces over Mach ports) and Windows aren't done; there the launcher's window comes to
the front.

## Settings

`kyty_emulator --launcher-describe-settings` prints the options the launcher can show, as JSON
(`src/common/launcher/settings.cpp`). Keys are the command-line options without their dashes
(`present-mode`), plus `resolution` for the window size. The launcher stores the player's values,
globally and per game, and sends them with `configure` before boot; Kyty applies them in memory
over its command line. `perf-overlay` is live: it can change from the launcher while a game runs.
`console-language` is a choice of language names; Kyty also takes the number, as on its command line.

Kyty adds no rows of its own to the quick menu, so the launcher's stored value stays the one that
counts. F2 still shows or hides the performance overlay, for that session only.

## Trophies without playing

`kyty_emulator --launcher-list-trophies <game folder> [--user-id <n>] [--console-language <n>]`
prints a game's trophies with the unlocks Kyty keeps for that user, as the launcher's achievement
list. Unlocks are read from `_Trophies` in the current folder, as when Kyty plays, so run it from
the folder Kyty runs games in (the launcher uses the executable's folder).

## Checking the protocol

`kyty_emulator --launcher-self-test <game-launcher>/docs/plugin-api/examples` checks every message
Kyty builds and reads against the launcher's examples, which the launcher regenerates from its own
types. `launcher_session_tests` runs the same check when given that folder, plus unit tests of the
trophy list and settings. CTest runs it against a copy of those examples in `tests/launcher-examples`,
or a game-launcher checkout's when `KYTY_LAUNCHER_EXAMPLES_DIR` points at it.

## Not done yet

- The composited quick menu on macOS and Windows.
- An HDR game's frame is copied for the menu's background without tone mapping, so it looks off.
- Shader compilation progress as an `activity`.
