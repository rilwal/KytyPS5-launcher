#include "launcherPlugin.h"

#include "common/config.h"
#include "common/emulatorConfig.h"
#include "common/launcher/library.h"
#include "common/launcher/overlayPorts.h"
#include "common/launcher/settings.h"
#include "common/launcher/trophyList.h"
#include "common/stringUtils.h"
#include "launcher_runner.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace Emulator::LauncherPlugin {

namespace Launcher = Common::Launcher;
namespace Settings = Common::Launcher::Settings;
namespace fs       = std::filesystem;

namespace {

constexpr std::string_view PLUGIN_FLAG = "--launcher-plugin";
constexpr std::string_view GAME_DIR    = "--game-dir";

// The setting holding the game folders: a folder picker keeping a list, one path per line
constexpr char GAME_DIRS_KEY[] = "game_dirs";
constexpr char PAGE[]          = "Kyty";
constexpr char GROUP[]         = "Emulators";

// launcherd sends a setting's stored value right after the settings are described, and nothing
// for one never set, so the first library waits this long for the game folders.
constexpr std::chrono::milliseconds GAME_DIRS_WAIT {1000};

fs::path SelfPath() {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	std::array<wchar_t, 32768> name {};
	GetModuleFileNameW(nullptr, name.data(), static_cast<DWORD>(name.size()));
	return fs::path(name.data());
#elif defined(__APPLE__)
	std::array<char, 4096> name {};
	auto                   size = static_cast<uint32_t>(name.size());
	_NSGetExecutablePath(name.data(), &size);
	std::error_code error;
	const auto      path = fs::canonical(name.data(), error);
	return error ? fs::path(name.data()) : path;
#else
	std::error_code error;
	return fs::read_symlink("/proc/self/exe", error);
#endif
}

// Folders one per line (a Path setting's list), and with `path_separators` also separated as in
// PATH, ':' (';' on Windows), as --game-dir takes them
std::vector<fs::path> SplitFolders(std::string_view text, bool path_separators) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	const std::string_view SEPARATORS = path_separators ? "\n;" : "\n";
#else
	const std::string_view SEPARATORS = path_separators ? "\n:" : "\n";
#endif
	std::vector<fs::path> out;
	size_t                start = 0;
	while (start <= text.size()) {
		const size_t end  = std::min(text.find_first_of(SEPARATORS, start), text.size());
		auto         part = std::string(text.substr(start, end - start));
		if (!part.empty() && part.back() == '\r') {
			part.pop_back();
		}
		// Only "~" and "~/...", as launcher-core expands them
		if (part == "~" || part.starts_with("~/") || part.starts_with("~\\")) {
			const char* home = std::getenv("HOME");
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
			if (home == nullptr) {
				home = std::getenv("USERPROFILE");
			}
#endif
			if (home != nullptr) {
				part = home + part.substr(1);
			}
		}
		if (!part.empty()) {
			out.push_back(Common::PathFromUtf8(Common::FixFilenameSlash(part)));
		}
		start = end + 1;
	}
	return out;
}

// The executable as launcherd started it: the configured path when that's absolute, so a wrapper
// (an AppImage, a Nix wrapper) is spawned again rather than the binary inside it.
fs::path ExePath(const char* argv0) {
	const fs::path given = argv0 != nullptr ? Common::PathFromUtf8(argv0) : fs::path {};
	std::error_code error;
	if (given.is_absolute() && fs::is_regular_file(given, error)) {
		return given;
	}
	return SelfPath();
}

// {game_dir}, {boot} and {title_id} in a per-game argument, as launcher-core substitutes them
std::string Substitute(std::string arg, const std::string& dir, const std::string& title_id) {
	const std::string boot = Common::PathToString(Common::PathFromUtf8(dir) / "eboot.bin");
	for (const auto& [from, to]: {std::pair<std::string_view, const std::string&> {"{game_dir}", dir},
	                              {"{boot}", boot},
	                              {"{title_id}", title_id}}) {
		for (size_t at = arg.find(from); at != std::string::npos; at = arg.find(from, at + to.size())) {
			arg.replace(at, from.size(), to);
		}
	}
	return arg;
}

class Plugin {
public:
	Plugin(std::vector<fs::path> arg_dirs, fs::path exe): m_arg_dirs(std::move(arg_dirs)), m_exe(std::move(exe)) {
		for (const auto& row: Settings::PluginRows()) {
			if (row.runner) {
				m_own_keys.insert(row.key);
			}
		}
	}

	bool Connect() {
		lc_callbacks cb {};
		cb.user    = this;
		cb.setting = [](void* user, const char* key, const char* value) {
			static_cast<Plugin*>(user)->OnSetting(key, value);
		};
		cb.closed = [](void* user) { static_cast<Plugin*>(user)->OnClosed(); };

		lc_runner_callbacks rc {};
		rc.user = this;
		rc.scan = [](void* user) {
			auto* self = static_cast<Plugin*>(user);
			self->Post([self]() { self->SendLibrary(); });
		};
		rc.launch = [](void* user, const lc_launch_request* request) {
			static_cast<Plugin*>(user)->OnLaunch(*request);
		};
		rc.list_achievements = [](void* user, uint64_t game, const char* external_id,
		                          const char* game_dir) {
			// Reading trophy packages takes a while: on the worker, so launches don't wait
			auto*       self = static_cast<Plugin*>(user);
			std::string id   = external_id != nullptr ? external_id : "";
			std::string dir  = game_dir != nullptr ? game_dir : "";
			self->Post([self, game, id = std::move(id), dir = std::move(dir)]() {
				self->OnListAchievements(game, id, dir);
			});
		};

		const std::string name = std::string("Kyty ") + KYTY_VERSION;
		lc_connect_info   info {};
		info.name         = name.c_str();
		info.capabilities = LC_CAP_LIBRARY | LC_CAP_LAUNCH | LC_CAP_ACHIEVEMENTS | LC_CAP_SETTINGS;
		// The games it starts draw the quick menu themselves (launcherOverlay.h), so launcherd
		// keeps its own overlay out of them. Only where the overlay can be shared at all: elsewhere
		// launcherd's own overlay (its Metal hook or panel) has to stay.
		if (Launcher::OverlayPorts::Supported()) {
			info.capabilities |= LC_CAP_COMPOSITED_OVERLAY;
		}
		std::array<char, 512> error {};
		m_session = lc_runner_connect(&info, &cb, &rc, error.data(), error.size());
		if (m_session == nullptr) {
			::fprintf(stderr, "kyty: can't run as the launcher's plugin: %s\n", error.data());
			return false;
		}
		return true;
	}

	int Run() {
		m_worker = std::thread([this]() { Work(); });
		SendSettings();
		{
			// The first library has the setting's folders in it, so their games don't flicker
			// to not installed on every start
			std::unique_lock lock(m_mutex);
			m_cv.wait_for(lock, GAME_DIRS_WAIT, [this]() { return m_have_game_dirs || m_closed; });
			m_started = true;
		}
		Post([this]() { SendLibrary(); });

		std::unique_lock lock(m_mutex);
		m_cv.wait(lock, [this]() { return m_closed; });
		lock.unlock();
		m_worker.join();
		lc_close(m_session);
		return 0;
	}

private:
	// Scans and trophy lists run one at a time on the worker, in order, so the library sent last
	// is always the newest
	void Post(std::function<void()> job) {
		std::lock_guard lock(m_mutex);
		m_jobs.push_back(std::move(job));
		m_cv.notify_all();
	}

	void Work() {
		std::unique_lock lock(m_mutex);
		for (;;) {
			m_cv.wait(lock, [this]() { return m_closed || !m_jobs.empty(); });
			if (m_closed) {
				return;
			}
			auto job = std::move(m_jobs.front());
			m_jobs.pop_front();
			lock.unlock();
			job();
			lock.lock();
		}
	}

	// Its own settings (the game folders, and the rows marked runner) on Kyty's page, and the rest
	// as game settings, which the launcher also offers per game and sends to the games it starts
	void SendSettings() {
		const auto rows = Settings::PluginRows();

		// Kept alive until lc_settings and lc_game_settings have encoded them
		std::vector<std::vector<lc_option>> options(rows.size());
		std::vector<lc_setting>             own;
		std::vector<lc_setting>             game;

		lc_path_pick pick {};
		pick.file     = false;
		pick.title    = "Choose a folder of PS5 games";
		pick.start    = "~";
		pick.multiple = true;

		lc_setting dirs {};
		dirs.key           = GAME_DIRS_KEY;
		dirs.section       = "Games";
		dirs.label         = "Game folders";
		dirs.help          = "Each a game's folder, or a folder of them, one level deep.";
		dirs.kind          = LC_SETTING_PATH;
		dirs.path          = &pick;
		dirs.value         = "";
		dirs.default_value = "";
		dirs.group         = GROUP;
		own.push_back(dirs);

		for (size_t i = 0; i < rows.size(); i++) {
			const auto& row = rows[i];
			lc_setting  s {};
			s.key           = row.key.c_str();
			s.section       = row.section.c_str();
			s.label         = row.label.c_str();
			s.help          = row.help.c_str();
			s.value         = row.default_value.c_str();
			s.default_value = row.default_value.c_str();
			s.group         = GROUP;
			s.live          = row.live;
			switch (row.kind) {
				case Settings::Row::Kind::Toggle: s.kind = LC_SETTING_TOGGLE; break;
				case Settings::Row::Kind::Text: s.kind = LC_SETTING_TEXT; break;
				case Settings::Row::Kind::Choice:
					s.kind = LC_SETTING_CHOICE;
					for (const auto& c: row.choices) {
						options[i].push_back({c.c_str(), c.c_str()});
					}
					s.options      = options[i].data();
					s.option_count = options[i].size();
					break;
			}
			if (row.runner) {
				// Apart from the game settings' System section, which the launcher lists after it
				s.section = "Profile";
			}
			(row.runner ? own : game).push_back(s);
		}
		lc_settings(m_session, PAGE, own.data(), own.size());
		lc_game_settings(m_session, PAGE, game.data(), game.size());
	}

	void SendLibrary() {
		std::vector<fs::path> folders;
		{
			std::lock_guard lock(m_mutex);
			folders = m_arg_dirs;
			folders.insert(folders.end(), m_setting_dirs.begin(), m_setting_dirs.end());
		}
		auto library = Launcher::ScanLibrary(folders);
		if (folders.empty()) {
			library.warnings.emplace_back("No game folders yet: add them on Kyty's settings page.");
		}

		std::vector<std::string> paths; // UTF-8 copies the games point into
		paths.reserve(library.games.size() * 3);
		std::vector<lc_game> games;
		for (const auto& g: library.games) {
			lc_game game {};
			game.external_id = g.title_id.c_str();
			game.title       = g.title.c_str();
			game.platform    = g.platform.c_str();
			game.installed   = true;
			paths.push_back(Common::PathToString(g.dir));
			game.game_dir = paths.back().c_str();
			if (!g.icon.empty()) {
				paths.push_back(Common::PathToString(g.icon));
				game.art.icon = paths.back().c_str();
			}
			if (!g.hero.empty()) {
				paths.push_back(Common::PathToString(g.hero));
				game.art.hero = paths.back().c_str();
			}
			games.push_back(game);
		}
		std::vector<const char*> warnings;
		for (const auto& w: library.warnings) {
			warnings.push_back(w.c_str());
		}
		lc_library(m_session, games.data(), games.size(), warnings.data(), warnings.size());

		std::lock_guard lock(m_mutex);
		m_library = std::move(library);
	}

	void OnSetting(const char* key, const char* value) {
		const std::string k = key != nullptr ? key : "";
		bool              rescan = false;
		{
			std::lock_guard lock(m_mutex);
			if (k == GAME_DIRS_KEY) {
				m_setting_dirs   = SplitFolders(value != nullptr ? value : "", false);
				m_have_game_dirs = true;
				rescan           = m_started; // Before that, the first library has them
				m_cv.notify_all();
			} else if (!m_own_keys.contains(k)) {
				// Not one of its own settings, such as a value stored when every option was one:
				// the game settings reach the games in configure, not on their command line
			} else if (value != nullptr) {
				m_values[k] = value;
			} else {
				m_values.erase(k);
			}
		}
		if (rescan) {
			Post([this]() { SendLibrary(); });
		}
	}

	// Its own settings as Kyty applies them, from the stored values
	Config::ConfigOptions Options() {
		Config::ConfigOptions cfg;
		for (const auto& [key, value]: m_values) {
			(void)Settings::Apply(cfg, key, value);
		}
		return cfg;
	}

	void OnLaunch(const lc_launch_request& request) {
		std::string dir = request.game_dir != nullptr ? request.game_dir : "";
		if (dir.empty()) {
			std::lock_guard lock(m_mutex);
			for (const auto& g: m_library.games) {
				if (g.title_id == request.external_id) {
					dir = Common::PathToString(g.dir);
				}
			}
		}
		if (dir.empty()) {
			lc_launch_error(m_session, request.launch, "Kyty doesn't know where this game is.");
			return;
		}

		// The player's per-game arguments replace the default ones, as for launcher-core's runners
		std::vector<std::string> args;
		const std::string        title_id = request.external_id != nullptr ? request.external_id : "";
		for (size_t i = 0; i < request.arg_count; i++) {
			args.push_back(Substitute(request.args[i], dir, title_id));
		}
		if (std::ranges::find(args, std::string("--game")) == args.end()) {
			args.insert(args.begin(), {"--game", dir});
		}
		{
			// Its own settings; the game settings come to the game from launcherd, in configure
			std::lock_guard lock(m_mutex);
			for (const auto& [key, value]: m_values) {
				// Only those Kyty knows: a value it rejects would stop the game from starting
				Config::ConfigOptions cfg;
				if (Settings::Apply(cfg, key, value).empty()) {
					args.emplace_back(Settings::SettingFlag);
					args.push_back(key + "=" + value);
				}
			}
		}
		const std::string        exe = Common::PathToString(m_exe);
		const std::string        cwd = Common::PathToString(m_exe.parent_path());
		std::vector<const char*> argv;
		for (const auto& a: args) {
			argv.push_back(a.c_str());
		}
		lc_spawn spawn {};
		spawn.exe       = exe.c_str();
		spawn.args      = argv.data();
		spawn.arg_count = argv.size();
		// The executable's folder, where Kyty keeps saves, trophies and caches, as for
		// launcher-core's runners (launcherd itself would leave its own)
		spawn.cwd = cwd.c_str();
		lc_launch_spawn(m_session, request.launch, &spawn);
	}

	void OnListAchievements(uint64_t game, const std::string& external_id, const std::string& game_dir) {
		fs::path dir = Common::PathFromUtf8(game_dir);
		Config::ConfigOptions cfg;
		{
			std::lock_guard lock(m_mutex);
			cfg = Options();
			if (dir.empty()) {
				for (const auto& g: m_library.games) {
					if (g.title_id == external_id) {
						dir = g.dir;
					}
				}
			}
		}
		if (dir.empty()) {
			return;
		}
		// Unlocks are where a game started by this plugin keeps them: under the executable's folder
		const auto trophies = Launcher::GameTrophies(dir, m_exe.parent_path(), cfg.user_id,
		                                             static_cast<int>(cfg.console_language));
		if (trophies.empty()) {
			return; // A PS4 dump, or no trophy set: nothing to say, and nothing to wipe
		}
		std::vector<lc_achievement> list;
		for (const auto& t: trophies) {
			lc_achievement a {};
			a.id          = t.id.c_str();
			a.name        = t.name.c_str();
			a.description = t.description.c_str();
			a.hidden      = t.hidden;
			a.dlc         = t.dlc;
			a.grade       = t.grade.empty() ? nullptr : t.grade.c_str();
			a.unlocked    = t.unlocked_at.has_value();
			a.unlocked_at = t.unlocked_at.value_or(0);
			list.push_back(a);
		}
		lc_achievement_list(m_session, game, list.data(), list.size());
	}

	void OnClosed() {
		std::lock_guard lock(m_mutex);
		m_closed = true;
		m_cv.notify_all();
	}

	std::vector<fs::path> m_arg_dirs;
	fs::path              m_exe;
	lc_session*           m_session = nullptr;

	std::mutex                         m_mutex;
	std::condition_variable            m_cv; // Closed, game folders came, or a job
	bool                               m_closed         = false;
	bool                               m_have_game_dirs = false;
	bool                               m_started        = false; // The first library is queued
	std::deque<std::function<void()>>  m_jobs;
	std::thread                        m_worker;
	std::vector<fs::path>              m_setting_dirs;
	std::set<std::string>              m_own_keys; // The rows marked runner, besides game_dirs
	std::map<std::string, std::string> m_values;   // Its own settings' values, by Kyty's keys
	Launcher::Library                  m_library;
};

} // namespace

bool IsPluginLaunch(int argc, char* argv[]) {
	return (argc >= 2 && argv[1] == PLUGIN_FLAG) || (argc < 2 && lc_env_present());
}

int Run(int argc, char* argv[]) {
	std::vector<fs::path> dirs;
	for (int i = 2; i < argc; i++) {
		if (argv[i] == GAME_DIR && i + 1 < argc) {
			auto more = SplitFolders(argv[++i], true);
			dirs.insert(dirs.end(), more.begin(), more.end());
		} else {
			::fprintf(stderr, "kyty: unknown plugin option: %s\n", argv[i]);
			return 1;
		}
	}
	Plugin plugin(std::move(dirs), ExePath(argv[0]));
	if (!plugin.Connect()) {
		return 1;
	}
	return plugin.Run();
}

} // namespace Emulator::LauncherPlugin
