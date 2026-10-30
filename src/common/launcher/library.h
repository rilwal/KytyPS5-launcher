#ifndef KYTY_COMMON_LAUNCHER_LIBRARY_H_
#define KYTY_COMMON_LAUNCHER_LIBRARY_H_

#include <filesystem>
#include <string>
#include <vector>

// The games in the launcher's game folders, for Kyty as the launcher's plugin (launcherPlugin.h).
// A game folder has sce_sys/param.json (PS5) or sce_sys/param.sfo (PS4) and an eboot.bin. Each
// folder the player gave is a game folder, or a folder of them, one level deep.
namespace Common::Launcher {

struct LibraryGame {
	std::string           title_id; // Upper case, e.g. PPSA21564
	std::string           title;
	std::string           platform; // "Ps5" or "Ps4", as the launcher spells it
	std::filesystem::path dir;
	std::filesystem::path icon; // Empty if the game has none
	std::filesystem::path hero;
};

struct Library {
	std::vector<LibraryGame> games;    // By title id; the first folder with an id wins
	std::vector<std::string> warnings; // Folders that aren't there, games that can't be read
};

[[nodiscard]] Library ScanLibrary(const std::vector<std::filesystem::path>& folders);

// One game folder; an error message instead if it isn't one Kyty can read.
[[nodiscard]] bool ReadLibraryGame(const std::filesystem::path& dir, LibraryGame& game,
                                   std::string& error);

} // namespace Common::Launcher

#endif // KYTY_COMMON_LAUNCHER_LIBRARY_H_
