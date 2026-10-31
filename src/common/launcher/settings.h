#ifndef KYTY_COMMON_LAUNCHER_SETTINGS_H_
#define KYTY_COMMON_LAUNCHER_SETTINGS_H_

#include "common/emulatorConfig.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

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

// A setting as a row of Kyty's page in the launcher's settings, when Kyty is the launcher's plugin
// (launcherPlugin.h). Numbers are offered as a choice of steps.
struct Row {
	enum class Kind { Toggle, Choice, Text };
	std::string              key;
	std::string              label;
	std::string              section;
	Kind                     kind = Kind::Toggle;
	std::vector<std::string> choices; // Choice only
	std::string              default_value;
	bool                     live = false;
};

// The settings worth a row in the launcher, in order: every one Describe lists.
[[nodiscard]] std::vector<Row> PluginRows();

// What a spawned game gets on its command line for one setting: a value Apply reads.
inline constexpr char SettingFlag[] = "--launcher-setting";

} // namespace Common::Launcher::Settings

#endif // KYTY_COMMON_LAUNCHER_SETTINGS_H_
