#ifndef EMULATOR_INCLUDE_EMULATOR_EMULATOR_H_
#define EMULATOR_INCLUDE_EMULATOR_EMULATOR_H_

#include "common/additionalContent.h"
#include "common/emulatorConfig.h"
#include "common/stringUtils.h"

#include <filesystem>

namespace Emulator {

struct RunOptions {
	Config::ConfigOptions                config;
	std::filesystem::path                app0_dir;
	std::filesystem::path                elf;
	std::filesystem::path                game_patch;
	std::filesystem::path                addcont_dir;
	std::vector<Common::AddContentEntry> addcont;
	// Kyty's own plugin started the game, with the launcher's settings on the command line
	// (--launcher-setting), so launcher mode doesn't wait for configure (--launcher-no-configure).
	bool skip_configure = false;
};

void Run(const RunOptions& options);
int LoadExec(const char* path, const char* const argv[]);

} // namespace Emulator

#endif /* EMULATOR_INCLUDE_EMULATOR_EMULATOR_H_ */
