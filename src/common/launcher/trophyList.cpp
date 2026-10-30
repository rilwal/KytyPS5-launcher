#include "common/launcher/trophyList.h"

#include <fmt/format.h>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>

namespace Common::Launcher {

// Trophy sets a game can have: trophy00.ucp and up.
constexpr uint32_t MAX_SERVICE_LABELS = 16;

std::string TrophyId(uint32_t service_label, int trophy_id) {
	return service_label == 0 ? std::to_string(trophy_id)
	                          : fmt::format("{}:{}", service_label, trophy_id);
}

int64_t TickToUnix(uint64_t tick) {
	return tick > Trophies::UnixEpochTick
	           ? static_cast<int64_t>((tick - Trophies::UnixEpochTick) / 1000000u)
	           : 0;
}

static std::string Grade(int grade) {
	switch (grade) {
		case 1: return "platinum";
		case 2: return "gold";
		case 3: return "silver";
		case 4: return "bronze";
		default: return {};
	}
}

std::vector<Achievement> TrophyList(const Trophies::Package&    package,
                                    const Trophies::UnlockData& unlocks, uint32_t service_label) {
	std::vector<Achievement> list;
	list.reserve(package.trophies.size());
	for (const auto& [id, trophy]: package.trophies) {
		Achievement a;
		a.id          = TrophyId(service_label, id);
		a.name        = trophy.name;
		a.description = trophy.description;
		a.hidden      = trophy.hidden;
		a.grade       = Grade(trophy.grade);
		// Group -1 is the base game; numbered groups are add-ons.
		a.dlc = trophy.group_id >= 0;
		if (unlocks.unlocked.contains(id)) {
			const auto at = unlocks.timestamps.find(id);
			// 0: unlocked at a time Kyty doesn't know
			a.unlocked_at = at != unlocks.timestamps.end() ? TickToUnix(at->second) : 0;
		}
		list.push_back(std::move(a));
	}
	return list;
}

// The PS5 title id from sce_sys/param.json, which names the folder Kyty keeps unlocks in.
static std::string TitleId(const std::filesystem::path& game_dir) {
	std::ifstream file(game_dir / "sce_sys" / "param.json", std::ios::binary);
	if (!file) {
		return {};
	}
	std::stringstream text;
	text << file.rdbuf();
	const auto json = nlohmann::json::parse(text.str(), nullptr, false);
	if (!json.is_object() || !json.contains("titleId") || !json["titleId"].is_string()) {
		return {};
	}
	return json["titleId"].get<std::string>();
}

std::vector<Achievement> GameTrophies(const std::filesystem::path& game_dir,
                                      const std::filesystem::path& root, int user_id,
                                      int console_language) {
	const std::string        title_id = TitleId(game_dir);
	std::vector<Achievement> all;
	for (uint32_t label = 0; label < MAX_SERVICE_LABELS; label++) {
		const auto path = game_dir / Trophies::PackagePath(label);
		std::error_code error;
		if (!std::filesystem::is_regular_file(path, error)) {
			continue;
		}
		const auto package = Trophies::LoadPackage(path, console_language);
		const auto unlocks_path = Trophies::UnlocksPath(root, title_id, user_id, label);
		const auto unlocks =
		    unlocks_path.empty() ? Trophies::UnlockData {} : Trophies::LoadUnlockData(unlocks_path);
		auto list = TrophyList(package, unlocks, label);
		all.insert(all.end(), std::make_move_iterator(list.begin()),
		           std::make_move_iterator(list.end()));
	}
	return all;
}

std::string ListGameTrophies(const std::filesystem::path& game_dir,
                             const std::filesystem::path& root, int user_id,
                             int console_language) {
	return AchievementListJson(GameTrophies(game_dir, root, user_id, console_language));
}

} // namespace Common::Launcher
