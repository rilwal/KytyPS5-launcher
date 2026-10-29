#ifndef KYTY_COMMON_LAUNCHER_SETTINGS_H_
#define KYTY_COMMON_LAUNCHER_SETTINGS_H_

#include "common/emulatorConfig.h"

#include <optional>
#include <string>
#include <string_view>

// Kyty's options for the launcher, which shows them in its own settings UI (the launcher's
// docs/plugin-api.md, "Emulator settings"). Keys are the command-line option names without the
// leading dashes, e.g. "present-mode"; "resolution" sets the width and height together.
namespace Common::Launcher::Settings {

// The flag the launcher runs Kyty with to ask for its settings.
inline constexpr char DescribeFlag[] = "--launcher-describe-settings";

// A JSON array, one row per setting: {key, label, section, kind, default, live}, where kind is
// "toggle", {"choice": [...]}, {"number": {min, max}} or {"text": {}}.
[[nodiscard]] std::string Describe();

// Sets an option from its text form, or back to its default if there is no value. Returns an
// error message, or an empty string on success.
std::string Apply(Config::ConfigOptions& cfg, std::string_view key,
                  const std::optional<std::string>& value);

// Whether the setting can change while a game runs (the launcher sends those with `setting`).
[[nodiscard]] bool IsLive(std::string_view key);

} // namespace Common::Launcher::Settings

#endif // KYTY_COMMON_LAUNCHER_SETTINGS_H_
