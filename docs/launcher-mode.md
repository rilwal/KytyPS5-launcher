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
| Connected | `booting`, then the quick menu's items (`overlay_items`) |
| First frame on screen | `ready {pid, window}`: the HWND, X11 window id or NSWindow number |
| Settings the launcher sent can't be applied | `notify` (error) |
| The game opens its trophy set | `achievements`: every trophy with the stored unlocks |
| A trophy (and maybe the platinum) unlocks | `unlocked {id, at}` per trophy |
| Quick menu opened or closed | the guest pauses or resumes, then `paused` |
| A fatal error (`EXIT`, failed checks) | `failed {reason, code: "fatal_error"}` |
| The game folder or options are invalid | `failed {code: "invalid_file_or_folder"}` |
| `quit` | the window loop ends as on closing the window: the pipeline cache is saved, then Kyty exits |

In launcher mode Kyty's own trophy toasts are off, since the launcher shows them.

Trophy ids are the trophy ids as text, prefixed with the service label for any trophy set past the
first (`"1:12"`). Only the configured user's trophies (`--user-id`) are reported.

Pausing uses the same path as the OS sending the app to the background: the present thread keeps
showing the last frame and the guest's vblanks stop.

## Settings

`kyty_emulator --launcher-describe-settings` prints the options the launcher can show, as JSON
(`src/common/launcher/settings.cpp`). Keys are the command-line options without their dashes
(`present-mode`), plus `resolution` for the window size. The launcher stores the player's values,
globally and per game, and sends them with `configure` before boot; Kyty applies them in memory
over its command line. `perf-overlay` is live: it can change from the launcher while a game runs.

## Trophies without playing

`kyty_emulator --launcher-list-trophies <game folder> [--user-id <n>] [--console-language <n>]`
prints a game's trophies with the unlocks Kyty keeps for that user, as the launcher's achievement
list. Unlocks are read from `_Trophies` in the current folder, as when Kyty plays, so run it from
the folder Kyty runs games in (the launcher uses the executable's folder).

## Checking the protocol

`kyty_emulator --launcher-self-test <game-launcher>/docs/plugin-api/examples` checks every message
Kyty builds and reads against the launcher's examples, which the launcher regenerates from its own
types. `launcher_session_tests` runs the same check when given that folder, plus unit tests of the
trophy list and settings.

## Not done yet

- Drawing the launcher's quick menu into Kyty's frames (`composited_overlay`). Until then the
  launcher brings its own window to the front over the game.
- Shader compilation progress as an `activity`.
