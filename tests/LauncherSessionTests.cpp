#include "common/emulatorConfig.h"
#include "common/launcher/protocol.h"
#include "common/launcher/settings.h"
#include "common/launcher/trophyList.h"
#include "common/trophies.h"

#include <cstdio>
#include <cstdlib>
#include <nlohmann/json.hpp>
#include <string>

namespace {

namespace Launcher = Common::Launcher;
namespace Trophies = Common::Trophies;

void Check(bool value, const char* text) {
	if (!value) {
		std::fprintf(stderr, "LauncherSessionTests: failed: %s\n", text);
		std::abort();
	}
}

void TestTrophyList() {
	Trophies::Package package;
	Trophies::Trophy  platinum;
	platinum.id    = 0;
	platinum.grade = 1;
	platinum.name  = "All of them";
	Trophies::Trophy bronze;
	bronze.id          = 1;
	bronze.grade       = 4;
	bronze.name        = "First steps";
	bronze.description = "Take them.";
	Trophies::Trophy dlc;
	dlc.id       = 2;
	dlc.grade    = 3;
	dlc.group_id = 0;
	dlc.hidden   = true;
	package.trophies.emplace(0, platinum);
	package.trophies.emplace(1, bronze);
	package.trophies.emplace(2, dlc);

	Trophies::UnlockData unlocks;
	unlocks.unlocked = {1, 2};
	// 2023-11-14 22:13:20 UTC
	unlocks.timestamps.emplace(1, Trophies::UnixEpochTick + 1700000000ULL * 1000000ULL);

	const auto list = Launcher::TrophyList(package, unlocks, 0);
	Check(list.size() == 3, "every trophy is listed");
	Check(list[0].grade == "platinum" && !list[0].unlocked_at, "platinum, locked");
	Check(list[1].id == "1" && list[1].grade == "bronze" && list[1].unlocked_at == 1700000000 &&
	          !list[1].dlc,
	      "bronze, unlocked at its time");
	Check(list[2].grade == "silver" && list[2].dlc && list[2].hidden && list[2].unlocked_at == 0,
	      "a group's trophy is DLC; unlocked without a time is 0");

	Check(Launcher::TrophyList(package, unlocks, 2)[1].id == "2:1",
	      "other trophy sets prefix their ids");
	Check(Launcher::TickToUnix(0) == 0, "no tick, no time");
}

void TestSettings() {
	const auto rows = nlohmann::json::parse(Launcher::Settings::Describe());
	Check(rows.is_array() && !rows.empty(), "settings are described");
	for (const auto& row: rows) {
		Check(row.contains("key") && row.contains("label") && row.contains("section") &&
		          row.contains("kind") && row["default"].is_string() && row["live"].is_boolean(),
		      "each row has the fields the launcher reads");
	}

	Config::ConfigOptions cfg;
	Check(Launcher::Settings::Apply(cfg, "present-mode", "Fifo").empty() &&
	          cfg.present_mode == Config::PresentMode::Fifo,
	      "a choice applies");
	Check(Launcher::Settings::Apply(cfg, "resolution", "1920x1080").empty() &&
	          cfg.screen_width == 1920 && cfg.screen_height == 1080,
	      "the resolution sets width and height");
	Check(Launcher::Settings::Apply(cfg, "splash", "false").empty() && !cfg.splash_enabled,
	      "a toggle applies");
	Check(Launcher::Settings::Apply(cfg, "splash", std::nullopt).empty() && cfg.splash_enabled,
	      "no value is the default");
	Check(Launcher::Settings::Apply(cfg, "vblank-frequency", "500") != "",
	      "a number out of range is refused");
	Check(Launcher::Settings::Apply(cfg, "no-such-setting", "1") != "", "unknown keys are refused");
	Check(Launcher::Settings::IsLive("perf-overlay") && !Launcher::Settings::IsLive("hdr"),
	      "only the performance overlay changes in game");
}

void TestHostMessages() {
	const auto setting = Launcher::ParseHost(R"({"type":"setting","key":"hdr","value":{"Bool":true}})");
	Check(setting.type == "setting" && setting.key == "hdr" && setting.value == "true",
	      "a Bool setting value reads as text");
	const auto reset = Launcher::ParseHost(R"({"type":"setting","key":"hdr","value":null})");
	Check(!reset.value, "null is the default");
	Check(!Launcher::ParseHost("not json").ok, "a bad frame is reported");
}

} // namespace

int main(int argc, char* argv[]) {
	TestTrophyList();
	TestSettings();
	TestHostMessages();

	// With the launcher's examples (game-launcher docs/plugin-api/examples), check the messages
	// against them too.
	if (argc > 1) {
		const auto errors = Launcher::SelfTest(argv[1]);
		for (const auto& error: errors) {
			std::fprintf(stderr, "LauncherSessionTests: %s\n", error.c_str());
		}
		Check(errors.empty(), "messages match the launcher's examples");
	}

	std::printf("LauncherSessionTests: ok\n");
	return 0;
}
