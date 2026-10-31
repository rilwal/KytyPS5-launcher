#ifndef KYTY_COMMON_LAUNCHER_SETTINGS_H_
#define KYTY_COMMON_LAUNCHER_SETTINGS_H_

#include "common/emulatorConfig.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Kyty's options for the launcher, which shows them in its own settings UI when Kyty is its plugin
// (launcherPlugin.h). Keys are the command-line option names without the leading dashes, e.g.
// "present-mode"; "resolution" sets the width and height together.
namespace Common::Launcher::Settings {

// Sets an option from its text form, or back to its default if there is no value. Returns an
// error message, or an empty string on success.
std::string Apply(Config::ConfigOptions& cfg, std::string_view key,
                  const std::optional<std::string>& value);

// Whether the setting can change while a game runs (the launcher sends those with `setting`).
[[nodiscard]] bool IsLive(std::string_view key);

// A setting as a row of Kyty's page in the launcher's settings. Numbers are offered as a choice of
// steps.
struct Row {
	enum class Kind { Toggle, Choice, Text };
	std::string              key;
	std::string              label;
	std::string              section;
	Kind                     kind = Kind::Toggle;
	std::vector<std::string> choices; // Choice only
	std::string              default_value;
	bool                     live = false;
	// One of the plugin's own settings rather than a game setting: text, which game settings can't
	// be, or a value the plugin needs itself (the language it lists trophies in). The plugin passes
	// these to the games it starts with SettingFlag; the launcher sends the rest in configure.
	bool runner = false;
};

// Every setting worth a row in the launcher, in order.
[[nodiscard]] std::vector<Row> PluginRows();

// What a spawned game gets on its command line for one of the plugin's own settings: a value
// Apply reads.
inline constexpr char SettingFlag[] = "--launcher-setting";

} // namespace Common::Launcher::Settings

#endif // KYTY_COMMON_LAUNCHER_SETTINGS_H_
