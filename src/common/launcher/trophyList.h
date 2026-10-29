#ifndef KYTY_COMMON_LAUNCHER_TROPHYLIST_H_
#define KYTY_COMMON_LAUNCHER_TROPHYLIST_H_

#include "common/launcher/protocol.h"
#include "common/trophies.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

// Trophies as the launcher's achievements (common/launcher/session.h).
namespace Common::Launcher {

// The flag the launcher runs Kyty with to list a game's trophies without playing it.
inline constexpr char ListTrophiesFlag[] = "--launcher-list-trophies";

// A trophy's id for the launcher: the trophy id, prefixed with the service label when it isn't 0
// ("1:12"), since a game's trophy sets share ids.
[[nodiscard]] std::string TrophyId(uint32_t service_label, int trophy_id);

// A Kyty tick (microseconds since 0001-01-01) as Unix seconds.
[[nodiscard]] int64_t TickToUnix(uint64_t tick);

// The package's trophies with the user's unlocks.
[[nodiscard]] std::vector<Achievement> TrophyList(const Trophies::Package& package,
                                                  const Trophies::UnlockData& unlocks,
                                                  uint32_t service_label);

// Every trophy set of the game in `game_dir` (sce_sys/trophy2/trophyNN.ucp), with the unlocks
// Kyty keeps for `user_id` under `root` (the folder Kyty runs in), as the launcher's
// achievement_list: a JSON array. Empty if the game has no trophies.
[[nodiscard]] std::string ListGameTrophies(const std::filesystem::path& game_dir,
                                           const std::filesystem::path& root, int user_id,
                                           int console_language);

} // namespace Common::Launcher

#endif // KYTY_COMMON_LAUNCHER_TROPHYLIST_H_
