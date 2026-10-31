#include "common/emulatorConfig.h"
#include "common/launcher/library.h"
#include "common/launcher/protocol.h"
#include "common/launcher/settings.h"
#include "common/launcher/trophyList.h"
#include "common/trophies.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
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
	Check(Launcher::Settings::Apply(cfg, "console-language", "English (UK)").empty() &&
		          cfg.console_language == 18,
	      "a console language applies by name");
	Check(Launcher::Settings::Apply(cfg, "console-language", "0").empty() && cfg.console_language == 0,
	      "a console language applies by number");
	Check(Launcher::Settings::Apply(cfg, "console-language", "30") != "",
	      "an unknown console language is refused");
	Check(Launcher::Settings::Apply(cfg, "ps5-pro", "true").empty() && cfg.pro_enabled,
	      "PS5 Pro mode applies");
	Check(Launcher::Settings::Apply(cfg, "emulate-psn", "true").empty() && cfg.psn_emulation_enabled,
	      "PSN emulation applies");
	Check(Launcher::Settings::Apply(cfg, "no-such-setting", "1") != "", "unknown keys are refused");
	Check(Launcher::Settings::IsLive("perf-overlay") && !Launcher::Settings::IsLive("hdr") &&
	          !Launcher::Settings::IsLive("ps5-pro"),
	      "only the performance overlay changes in game");
}

void TestPluginRows() {
	const auto rows = Launcher::Settings::PluginRows();
	Check(!rows.empty() && rows.front().key == "resolution", "the curated settings, in order");
	for (const auto& row: rows) {
		Config::ConfigOptions cfg;
		Check(Launcher::Settings::Apply(cfg, row.key, row.default_value).empty(),
		      "each row's default applies");
		for (const auto& choice: row.choices) {
			Check(Launcher::Settings::Apply(cfg, row.key, choice).empty(), "each choice applies");
		}
	}
	const auto vblank = std::ranges::find(rows, std::string("vblank-frequency"),
	                                      &Launcher::Settings::Row::key);
	Check(vblank != rows.end() && vblank->choices.size() <= 26 &&
	          std::ranges::find(vblank->choices, vblank->default_value) != vblank->choices.end(),
	      "a number is offered as steps that include its default");
}

void Write(const std::filesystem::path& path, const std::string& text) {
	std::filesystem::create_directories(path.parent_path());
	std::ofstream(path, std::ios::binary) << text;
}

void TestLibrary() {
	namespace fs   = std::filesystem;
	const auto root = fs::temp_directory_path() / "kyty-library-test";
	fs::remove_all(root);
	// A folder of games: a PS5 one, a PS4 one, one without an eboot, and a stray folder
	const auto ps5 = root / "games" / "astro";
	Write(ps5 / "sce_sys" / "param.json",
	      R"({"titleId":"ppsa21564","localizedParameters":{"defaultLanguage":"en-US","en-US":{"titleName":"ASTRO BOT"}}})");
	Write(ps5 / "eboot.bin", "");
	Write(ps5 / "sce_sys" / "icon0.png", "");
	Write(ps5 / "sce_sys" / "pic0.png", "");
	// A PSF with TITLE and TITLE_ID
	std::string sfo("\0PSF\1\1\0\0", 8);
	const auto  u32 = [](uint32_t v) { return std::string(reinterpret_cast<const char*>(&v), 4); };
	const auto  u16 = [](uint16_t v) { return std::string(reinterpret_cast<const char*>(&v), 2); };
	const std::string keys = std::string("TITLE\0TITLE_ID\0", 15);
	const std::string vals = std::string("Bloodborne\0\0CUSA00900\0\0\0", 24);
	sfo += u32(20 + 32) + u32(20 + 32 + 15) + u32(2);
	sfo += u16(0) + u16(0x0204) + u32(11) + u32(12) + u32(0);
	sfo += u16(6) + u16(0x0204) + u32(10) + u32(12) + u32(12);
	sfo += keys + vals;
	const auto ps4 = root / "games" / "bloodborne";
	Write(ps4 / "sce_sys" / "param.sfo", sfo);
	Write(ps4 / "eboot.bin", "");
	Write(root / "games" / "broken" / "sce_sys" / "param.json", R"({"titleId":"PPSA00001"})");
	fs::create_directories(root / "games" / "not-a-game");

	const auto library = Launcher::ScanLibrary({root / "games", ps5, root / "missing"});
	Check(library.games.size() == 2, "both games, the second sight of one ignored");
	Check(library.games[0].title_id == "CUSA00900" && library.games[0].title == "Bloodborne" &&
	          library.games[0].platform == "Ps4" && library.games[0].icon.empty(),
	      "a PS4 game from param.sfo");
	Check(library.games[1].title_id == "PPSA21564" && library.games[1].title == "ASTRO BOT" &&
	          library.games[1].platform == "Ps5" && !library.games[1].icon.empty() &&
	          library.games[1].hero.filename() == "pic0.png",
	      "a PS5 game from param.json, with its art, the id in upper case");
	Check(library.warnings.size() == 2, "the game without an eboot and the missing folder warn");
	fs::remove_all(root);
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
	TestPluginRows();
	TestLibrary();
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
