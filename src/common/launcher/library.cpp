#include "common/launcher/library.h"

#include "common/stringUtils.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <map>
#include <nlohmann/json.hpp>
#include <sstream>

namespace Common::Launcher {

namespace {

namespace fs = std::filesystem;

bool IsFile(const fs::path& path) {
	std::error_code error;
	return fs::is_regular_file(path, error);
}

bool IsGame(const fs::path& dir) {
	const auto sys = dir / "sce_sys";
	return IsFile(sys / "param.json") || IsFile(sys / "param.sfo");
}

fs::path FirstFile(const fs::path& dir, std::initializer_list<const char*> names) {
	for (const char* name: names) {
		if (IsFile(dir / name)) {
			return dir / name;
		}
	}
	return {};
}

std::string ReadAll(const fs::path& path) {
	std::ifstream     file(path, std::ios::binary);
	std::stringstream text;
	text << file.rdbuf();
	return text.str();
}

// PS5 metadata: titleId, and localizedParameters.<lang>.titleName with a defaultLanguage.
bool ParamJson(const fs::path& path, std::string& id, std::string& title, std::string& error) {
	const auto json = nlohmann::json::parse(ReadAll(path), nullptr, false);
	if (!json.is_object()) {
		error = "param.json isn't valid JSON";
		return false;
	}
	if (!json.contains("titleId") || !json["titleId"].is_string()) {
		error = "param.json has no titleId";
		return false;
	}
	id = json["titleId"].get<std::string>();

	const auto name = [](const nlohmann::json& lang) -> std::string {
		if (lang.is_object() && lang.contains("titleName") && lang["titleName"].is_string()) {
			return lang["titleName"].get<std::string>();
		}
		return {};
	};
	title.clear();
	if (const auto it = json.find("localizedParameters"); it != json.end() && it->is_object()) {
		const auto& loc  = *it;
		std::string lang = "en-US";
		if (loc.contains("defaultLanguage") && loc["defaultLanguage"].is_string()) {
			lang = loc["defaultLanguage"].get<std::string>();
		}
		if (loc.contains(lang)) {
			title = name(loc[lang]);
		}
		if (title.empty() && loc.contains("en-US")) {
			title = name(loc["en-US"]);
		}
		for (auto l = loc.begin(); title.empty() && l != loc.end(); ++l) {
			title = name(l.value());
		}
	}
	if (title.empty()) {
		title = "Untitled";
	}
	return true;
}

// PS4 metadata: a PSF file of keys and values; only the UTF-8 strings are kept.
bool ParamSfo(const fs::path& path, std::string& id, std::string& title, std::string& error) {
	const std::string data = ReadAll(path);
	const auto        u32  = [&](size_t at) {
        uint32_t v = 0;
        std::memcpy(&v, data.data() + at, 4); // Little-endian, like every host Kyty runs on
        return v;
	};
	const auto u16 = [&](size_t at) {
		uint16_t v = 0;
		std::memcpy(&v, data.data() + at, 2);
		return v;
	};
	if (data.size() < 20 || data.compare(0, 4, std::string("\0PSF", 4)) != 0) {
		error = "param.sfo isn't a PSF file";
		return false;
	}
	const uint32_t keys  = u32(8);
	const uint32_t vals  = u32(12);
	const uint32_t count = u32(16);
	std::map<std::string, std::string> strings;
	for (uint32_t i = 0; i < count; i++) {
		const size_t entry = 20 + static_cast<size_t>(i) * 16;
		if (entry + 16 > data.size()) {
			break;
		}
		const size_t key_at = static_cast<size_t>(keys) + u16(entry);
		const auto   format = u16(entry + 2);
		const size_t len    = u32(entry + 4);
		const size_t val_at = static_cast<size_t>(vals) + u32(entry + 12);
		if (key_at >= data.size() || val_at + len > data.size() || format != 0x0204) {
			continue;
		}
		std::string key(data.c_str() + key_at);
		std::string value(data.data() + val_at, len);
		value.erase(std::find(value.begin(), value.end(), '\0'), value.end());
		strings[std::move(key)] = std::move(value);
	}
	if (strings["TITLE_ID"].empty()) {
		error = "param.sfo has no TITLE_ID";
		return false;
	}
	id    = strings["TITLE_ID"];
	title = strings["TITLE"].empty() ? "Untitled" : strings["TITLE"];
	return true;
}

} // namespace

bool ReadLibraryGame(const fs::path& dir, LibraryGame& game, std::string& error) {
	const auto sys = dir / "sce_sys";
	std::string id;
	if (IsFile(sys / "param.json")) {
		if (!ParamJson(sys / "param.json", id, game.title, error)) {
			return false;
		}
		game.platform = "Ps5";
	} else if (IsFile(sys / "param.sfo")) {
		if (!ParamSfo(sys / "param.sfo", id, game.title, error)) {
			return false;
		}
		game.platform = "Ps4";
	} else {
		error = "no sce_sys/param.json or param.sfo";
		return false;
	}
	if (!IsFile(dir / "eboot.bin")) {
		error = "no eboot.bin";
		return false;
	}
	std::ranges::transform(id, id.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
	game.title_id = id;
	game.dir      = dir;
	game.icon     = FirstFile(sys, {"icon0.png", "ICON0.PNG"});
	// PS5 keeps its key art in pic0; on PS4 pic0 is a smaller overlay and pic1 the background.
	game.hero = game.platform == "Ps5" ? FirstFile(sys, {"pic0.png", "pic1.png"})
	                                   : FirstFile(sys, {"pic1.png", "PIC1.PNG", "pic0.png"});
	return true;
}

Library ScanLibrary(const std::vector<fs::path>& folders) {
	Library    out;
	const auto read = [&](const fs::path& dir) {
		LibraryGame game;
		std::string error;
		if (!ReadLibraryGame(dir, game, error)) {
			out.warnings.push_back(Common::PathToString(dir) + ": " + error);
		} else if (std::ranges::none_of(out.games, [&](const auto& g) { return g.title_id == game.title_id; })) {
			out.games.push_back(std::move(game));
		}
	};
	for (const auto& folder: folders) {
		std::error_code error;
		if (!fs::is_directory(folder, error)) {
			out.warnings.push_back(Common::PathToString(folder) + " not found");
			continue;
		}
		if (IsGame(folder)) {
			read(folder);
			continue;
		}
		std::vector<fs::path> subdirs;
		for (const auto& entry: fs::directory_iterator(folder, error)) {
			std::error_code e;
			if (entry.is_directory(e)) {
				subdirs.push_back(entry.path());
			}
		}
		std::ranges::sort(subdirs);
		for (const auto& dir: subdirs) {
			if (IsGame(dir)) {
				read(dir);
			}
		}
	}
	std::ranges::sort(out.games, {}, &LibraryGame::title_id);
	return out;
}

} // namespace Common::Launcher
