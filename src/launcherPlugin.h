#ifndef EMULATOR_LAUNCHER_PLUGIN_H_
#define EMULATOR_LAUNCHER_PLUGIN_H_

// Kyty as the game launcher's plugin for its own games (the launcher's docs/plugin-api.md, "Plugin
// runner messages"), through the launcher's client library (3rdparty/launcher-client). launcherd
// keeps it running; it finds the games in the game folders, describes Kyty's settings, lists a
// game's trophies without playing it, and starts a game by having launcherd spawn this same
// executable with --game and the settings as --launcher-setting. That game then connects back as
// a session (launcherMode.h).
namespace Emulator::LauncherPlugin {

// `kyty_emulator --launcher-plugin [--game-dir <dir>]...`, or no arguments at all from launcherd.
[[nodiscard]] bool IsPluginLaunch(int argc, char* argv[]);

// Runs until launcherd hangs up. Returns the exit code.
int Run(int argc, char* argv[]);

} // namespace Emulator::LauncherPlugin

#endif // EMULATOR_LAUNCHER_PLUGIN_H_
