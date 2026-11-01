#include "common/emulatorConfig.h"
#include "common/launcher/library.h"
#include "common/launcher/osk.h"
#include "common/launcher/protocol.h"
#include "common/launcher/settings.h"
#include "common/launcher/trophyList.h"
#include "common/trophies.h"

#include <algorithm>
#include <cstdio>
#include <functional>
#include <memory>
#include <vector>
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
	const auto rows = Launcher::Settings::PluginRows();
	Check(!rows.empty(), "settings are described");
	for (const auto& row: rows) {
		Check(!row.key.empty() && !row.label.empty() && !row.section.empty(),
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
	for (const char* key: {"perf-overlay", "resolution", "fullscreen", "stretch", "present-mode",
	                       "vblank-frequency", "hide-cursor", "tessellation", "sync-raw-image-buffers",
	                       "network-connected", "controller-volume", "controller-vibration"}) {
		Check(Launcher::Settings::IsLive(key), "settings the emulator reads as it goes are live");
	}
	for (const char* key: {"hdr", "ps5-pro", "gpu", "splash", "shader-cache", "occlusion-queries",
	                       "readback-linear-images", "emulate-psn", "console-language", "user-name"}) {
		Check(!Launcher::Settings::IsLive(key), "settings read at boot wait for the next one");
	}
}

// Config::ChangeLive, before and after Load
void TestLiveOptions() {
	Config::ChangeLive([](Config::ConfigOptions& cfg) { cfg.stretch_enabled = true; });
	Config::Initialize();
	Config::ConfigOptions boot;
	boot.screen_width  = 1600;
	boot.screen_height = 900;
	Config::Load(boot);
	Check(Config::StretchEnabled() && Config::GetScreenWidth() == 1600,
	      "a change before Load applies over its options");

	Config::ChangeLive([](Config::ConfigOptions& cfg) {
		Check(cfg.stretch_enabled && cfg.screen_height == 900, "a change gets the options as they are");
		cfg.screen_width       = 2560;
		cfg.screen_height      = 1440;
		cfg.present_mode       = Config::PresentMode::Immediate;
		cfg.fullscreen_enabled = true;
		cfg.hdr_enabled        = true;
	});
	Check(Config::GetScreenWidth() == 2560 && Config::GetScreenHeight() == 1440 &&
	          Config::GetPresentMode() == Config::PresentMode::Immediate &&
	          Config::FullscreenEnabled() && Config::StretchEnabled(),
	      "live options change after Load");
	Check(!Config::HdrEnabled(), "the others stay as Load set them");
	Config::Shutdown();
}

void TestPluginRows() {
	const auto rows = Launcher::Settings::PluginRows();
	Check(rows.front().key == "resolution", "every setting, in order");
	for (const auto& row: rows) {
		const bool own = row.key == "user-name" || row.key == "console-language";
		Check(row.runner == own, "only text and the trophy language are the plugin's own settings");
		Check(row.runner || row.kind != Launcher::Settings::Row::Kind::Text,
		      "game settings are toggles and choices");
		Check(row.live == Launcher::Settings::IsLive(row.key), "rows say whether they're live");
		Check(!row.help.empty(), "every row says what it does");
		Check(row.live == (row.help.find("next starts") == std::string::npos),
		      "rows that aren't live say they wait for the next boot");
	}
	const auto perf = std::ranges::find(rows, std::string("perf-overlay"), &Launcher::Settings::Row::key);
	Check(perf != rows.end() && perf->live && !perf->runner,
	      "the performance overlay is a live game setting");
	for (const char* key: {"ps5-pro", "emulate-psn"}) {
		Check(std::ranges::find(rows, std::string(key), &Launcher::Settings::Row::key) != rows.end(),
		      "newer settings get a row too");
	}
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

// An IME that applies edits at once, like the dialog's, or queues them (`deferred`), like the
// core IME's until the game's next update.
struct FakeIme {
	uint64_t                 generation = 0;
	std::u16string           text;
	uint32_t                 cursor   = 0;
	uint32_t                 max      = 16;
	bool                     deferred = false;
	std::vector<std::function<void()>> queued;
	std::string              finished; // accept or cancel

	std::shared_ptr<const Launcher::Osk::Ime> Make() {
		auto ime      = std::make_shared<Launcher::Osk::Ime>();
		const auto at = [this](uint64_t g) { return generation != 0 && g == generation; };
		const auto run = [this](std::function<void()> f) {
			if (deferred) {
				queued.push_back(std::move(f));
			} else {
				f();
			}
		};
		ime->snapshot = [this](Launcher::Osk::Field* f) {
			if (generation == 0) {
				return false;
			}
			*f            = {};
			f->generation = generation;
			f->text       = text;
			f->cursor     = cursor;
			f->max_length = max;
			f->title      = u"Character name";
			return true;
		};
		ime->insert = [this, at, run](uint64_t g, std::u16string_view t) {
			if (!at(g)) return false;
			run([this, s = std::u16string(t)]() {
				const auto room = max - std::min<uint32_t>(max, static_cast<uint32_t>(text.size()));
				const auto add  = s.substr(0, room);
				text.insert(cursor, add);
				cursor += static_cast<uint32_t>(add.size());
			});
			return true;
		};
		ime->backspace = [this, at, run](uint64_t g) {
			if (!at(g)) return false;
			run([this]() {
				if (cursor > 0) {
					text.erase(--cursor, 1);
				}
			});
			return true;
		};
		ime->move = [this, at, run](uint64_t g, int by) {
			if (!at(g)) return false;
			run([this, by]() {
				cursor = static_cast<uint32_t>(
				    std::clamp<int64_t>(int64_t {cursor} + by, 0, static_cast<int64_t>(text.size())));
			});
			return true;
		};
		ime->accept  = [this, at](uint64_t g) { return at(g) && (finished = "accept", true); };
		ime->cancel  = [this, at](uint64_t g) { return at(g) && (finished = "cancel", true); };
		ime->pending = [this](uint64_t) { return queued.size(); };
		return ime;
	}

	void Update() {
		for (auto& f: queued) {
			f();
		}
		queued.clear();
	}

	Launcher::Osk::Field Field() const {
		Launcher::Osk::Field f;
		f.generation = generation;
		f.text       = text;
		f.cursor     = cursor;
		f.max_length = max;
		f.title      = u"Character name";
		return f;
	}
};

void TestOsk() {
	namespace Osk = Launcher::Osk;
	Check(Osk::Utf8(Osk::Utf16("Anné 😀")) == "Anné 😀", "UTF-8 and UTF-16 round trip");
	const auto wide = Osk::Utf16("a😀b");
	Check(wide.size() == 4 && Osk::Characters(wide, 3) == 2 && Osk::Units(wide, 2) == 3,
	      "positions count characters, not UTF-16 units");

	for (const bool deferred: {false, true}) {
		std::vector<std::string> sent;
		auto                     sender = std::make_shared<Osk::Sender>();
		sender->open = [&](const Launcher::OskOpen& o) {
			sent.push_back("open " + std::to_string(o.osk) + " " + o.text + " " +
			               std::to_string(o.cursor) + " " + o.title);
		};
		sender->text = [&](uint32_t osk, uint64_t seq, std::string_view text, uint32_t cursor) {
			sent.push_back("text " + std::to_string(osk) + " " + std::to_string(seq) + " " +
			               std::string(text) + " " + std::to_string(cursor));
		};
		sender->closed = [&](uint32_t osk, std::string_view result) {
			sent.push_back("closed " + std::to_string(osk) + " " + std::string(result));
		};
		FakeIme ime;
		ime.deferred = deferred;
		Osk::Bridge bridge(false);
		bridge.SetSender(sender);
		bridge.SetIme(ime.Make());
		const auto edit = [&](uint64_t seq, std::string op, std::string text = {}, int by = 0) {
			Launcher::OskEdit e;
			e.op   = std::move(op);
			e.text = std::move(text);
			e.by   = by;
			bridge.Edit(1, seq, e);
		};

		// Not offered: the game's own
		ime.generation = 5;
		ime.text       = u"Ann";
		ime.cursor     = 3;
		bridge.Opened(ime.Field());
		Check(sent.empty() && !bridge.Owns(5), "no keyboard of the shell's before it's offered");
		// Offered with one open: it's the shell's from there
		bridge.Available(true);
		Check(sent == std::vector<std::string> {"open 1 Ann 3 Character name"} && bridge.Owns(5),
		      "an open keyboard goes to the shell once it's offered");
		sent.clear();

		edit(1, "insert", "e");
		edit(2, "move", {}, -2);
		edit(3, "backspace");
		if (deferred) {
			// Nothing's applied yet, so the text the shell has stands
			bridge.Poll();
			Check(sent.empty(), "nothing goes back before the game applies the keys");
			ime.Update();
		}
		bridge.Poll();
		Check(ime.text == u"Ane" && ime.cursor == 1, "keys edit the game's text");
		Check(!sent.empty() && sent.back() == "text 1 3 Ane 1",
		      "the text goes back with the last key it includes");
		sent.clear();
		bridge.Poll();
		Check(sent.empty(), "the same text isn't sent twice");

		// A real keyboard types too: sent with the keys it includes
		ime.text   = u"Anex";
		ime.cursor = 4;
		bridge.Poll();
		Check(sent == std::vector<std::string> {"text 1 3 Anex 4"}, "a change the shell didn't make goes back");
		sent.clear();

		// Done: the IME accepts; its closing tells the launcher
		bridge.Finish(1, true);
		Check(ime.finished == "accept", "done accepts the game's keyboard");
		ime.generation = 0;
		bridge.Closed();
		Check(sent == std::vector<std::string> {"closed 1 accepted"} && !bridge.Owns(5),
		      "it closes as the player finished it");
		sent.clear();

		// The next opens straight away; the offer ending gives it back as aborted
		ime.generation = 6;
		bridge.Opened(ime.Field());
		Check(bridge.Owns(6) && sent.size() == 1 && sent[0].rfind("open 2", 0) == 0, "the next opens");
		bridge.Available(false);
		Check(!bridge.Owns(6) && sent.back() == "closed 2 aborted", "it's the game's own again");
		bridge.Closed();
		Check(sent.size() == 2, "and nothing more goes out for it");
	}
}

} // namespace

int main(int argc, char* argv[]) {
	TestTrophyList();
	TestSettings();
	TestLiveOptions();
	TestPluginRows();
	TestLibrary();
	TestHostMessages();
	TestOsk();

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
