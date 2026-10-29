#ifndef KYTY_COMMON_LAUNCHER_PROTOCOL_H_
#define KYTY_COMMON_LAUNCHER_PROTOCOL_H_

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The plugin messages Kyty builds and reads (the launcher's docs/plugin-api.md). The launcher
// writes an example of every message from its Rust types into docs/plugin-api/examples/, and
// SelfTest checks these against them, so the two sides can't drift apart unnoticed.
namespace Common::Launcher {

// An encoded message, ready to send.
struct Message {
	std::string type;
	std::string json;
	std::string activity_id; // activity and activity_done only
};

// One trophy as the launcher's AchievementDef.
struct Achievement {
	std::string            id;
	std::string            name;
	std::string            description;
	bool                   hidden = false;
	std::string            grade; // bronze, silver, gold or platinum, or empty for none
	bool                   dlc    = false;
	std::optional<int64_t> unlocked_at;
};

// A row of the quick menu, as the launcher's Setting.
struct Setting {
	enum class Kind { Toggle, Choice, Action };
	struct Option {
		std::string value;
		std::string label;
	};
	std::string         key;
	std::string         section;
	std::string         label;
	std::string         help;
	Kind                kind = Kind::Action;
	std::vector<Option> options; // Choice only
	std::string         value;   // "true"/"false" for a toggle, the option's value for a choice
	std::string         default_value;
};

// configure's values, by Kyty's own keys; null means the default.
using ConfigureValues = std::map<std::string, std::optional<std::string>>;

// Plugin to launcher
[[nodiscard]] Message Hello(std::string_view token, int api, const std::vector<std::string>& caps,
                            std::string_view name);
[[nodiscard]] Message Booting();
[[nodiscard]] Message Ready(std::optional<uint32_t> pid, std::optional<uint64_t> window);
[[nodiscard]] Message Paused(bool paused);
[[nodiscard]] Message Failed(std::string_view reason, std::string_view code);
[[nodiscard]] Message ActivityMsg(const std::string& id, const std::string& label,
                                  std::optional<float> progress);
[[nodiscard]] Message ActivityDoneMsg(const std::string& id);
[[nodiscard]] Message Notify(std::string_view title, std::string_view body, bool error);
[[nodiscard]] Message AchievementsMsg(const std::vector<Achievement>& list);
// The list alone, as a JSON array (what --launcher-list-trophies prints)
[[nodiscard]] std::string AchievementListJson(const std::vector<Achievement>& list);
[[nodiscard]] Message UnlockedMsg(std::string_view id, std::optional<int64_t> at);
[[nodiscard]] Message OverlayItemsMsg(const std::vector<Setting>& items);

// Launcher to plugin
struct HostMessage {
	std::string                type;
	int                        api = 0;   // accepted
	std::string                title;     // accepted
	ConfigureValues            values;    // configure
	std::string                key;       // invoke, setting
	std::optional<std::string> value;     // invoke, setting; as text
	std::string                server;    // The welcome's server name
	std::string                encoding;  // The welcome's encoding
	bool                       ok = true; // False if the frame wasn't a JSON object
};

[[nodiscard]] HostMessage ParseHost(std::string_view frame);

// Checks the messages above against the launcher's examples in `dir`
// (docs/plugin-api/examples). Returns what's wrong.
[[nodiscard]] std::vector<std::string> SelfTest(const std::string& dir);

} // namespace Common::Launcher

#endif // KYTY_COMMON_LAUNCHER_PROTOCOL_H_
