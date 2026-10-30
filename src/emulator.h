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
	// The launcher's settings came on the command line (--launcher-setting), from Kyty's own
	// plugin, so launcher mode doesn't wait for configure.
	bool launcher_settings = false;
};

void Run(const RunOptions& options);
int LoadExec(const char* path, const char* const argv[]);

} // namespace Emulator

#endif /* EMULATOR_INCLUDE_EMULATOR_EMULATOR_H_ */
