#ifndef EMULATOR_LAUNCHER_MODE_H_
#define EMULATOR_LAUNCHER_MODE_H_

#include "common/emulatorConfig.h"

#include <string_view>

// Launcher mode: Kyty started by the game launcher (common/launcher/session.h). The launcher
// draws all UI, so Kyty reports to it instead of showing its own trophy toasts, and takes its
// settings from it.
namespace Emulator::LauncherMode {

// Connects if the launcher started Kyty, says the game is booting, and applies the settings the
// launcher sends before boot to `config`. Without `wait_for_configure` (Kyty's own plugin started
// the game, with its settings on the command line) it doesn't wait for them. Returns whether Kyty
// runs in launcher mode.
bool Start(Config::ConfigOptions& config, bool wait_for_configure = true);

// Reports a boot that can't start, connecting first if need be. Does nothing outside
// launcher mode.
void Fail(std::string_view reason, std::string_view code);

} // namespace Emulator::LauncherMode

#endif // EMULATOR_LAUNCHER_MODE_H_
