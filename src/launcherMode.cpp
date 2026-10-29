#include "launcherMode.h"

#include "common/launcher/session.h"
#include "common/launcher/settings.h"
#include "common/logging/log.h"
#include "graphics/presentation/performanceOverlay.h"
#include "graphics/presentation/window.h"

#include <chrono>
#include <cstdlib>
#include <string>

namespace Emulator::LauncherMode {

namespace Launcher = Common::Launcher;

// How long boot waits for the launcher's configure message, counted from connecting.
constexpr std::chrono::milliseconds CONFIGURE_TIMEOUT {2000};

constexpr std::string_view PERF_OVERLAY = "perf-overlay";

static void SetPerformanceOverlay(bool enabled) {
	if (Libs::Graphics::PerformanceOverlayEnabled() != enabled) {
		Libs::Graphics::TogglePerformanceOverlay();
	}
}

static void ApplyLive(const std::string& key, const std::optional<std::string>& value) {
	if (key == PERF_OVERLAY) {
		SetPerformanceOverlay(value.value_or("false") == "true");
	} else {
		LOGF("[Launcher] '%s' can't change while a game is running\n", key.c_str());
	}
}

static Launcher::Handlers MakeHandlers() {
	Launcher::Handlers handlers;

	handlers.quit = []() {
		// The window loop saves the pipeline cache and exits. One that arrives before the window
		// exists ends the loop as soon as it starts.
		LOGF("[Launcher] quit\n");
		Libs::Graphics::WindowRequestQuit();
	};

	handlers.overlay = [](bool open) {
		// The present thread keeps showing the last frame while the guest's vblanks stop.
		Libs::Graphics::WindowSetLauncherPaused(open);
		if (auto* session = Launcher::Session::Get(); session != nullptr) {
			session->Paused(open);
		}
	};

	// Live settings come from the launcher's settings page, which keeps their value. Kyty puts no
	// rows of its own in the quick menu: a toggle there would change the overlay behind the stored
	// setting's back, and the next boot's configure would undo it.
	handlers.setting = [](const std::string& key, const std::optional<std::string>& value) {
		if (Launcher::Settings::IsLive(key)) {
			ApplyLive(key, value);
		}
	};

	return handlers;
}

bool Start(Config::ConfigOptions& config) {
	if (!Launcher::EnvPresent()) {
		return false;
	}

	auto* session = Launcher::Session::Connect(MakeHandlers());
	if (session == nullptr) {
		LOGF("[Launcher] running in launcher mode without a connection to the launcher\n");
		return false;
	}

	session->Booting();

	// Only the options in memory change; Kyty has no config file of its own to write.
	std::string errors;
	if (const auto values = session->WaitForConfigure(CONFIGURE_TIMEOUT)) {
		for (const auto& [key, value]: *values) {
			if (const auto error = Launcher::Settings::Apply(config, key, value); !error.empty()) {
				LOGF("[Launcher] configure: %s\n", error.c_str());
				errors += (errors.empty() ? "" : "\n") + error;
			}
		}
	}
	if (!errors.empty()) {
		session->Notify("Some settings weren't applied", errors, true);
	}

	// The launcher shows trophy unlocks itself (libTrophy.cpp reports them).
	config.trophy_enabled = false;
	return true;
}

void Fail(std::string_view reason, std::string_view code) {
	auto* session = Launcher::Session::Get();
	if (session == nullptr) {
		session = Launcher::Session::Connect({});
	}
	if (session != nullptr) {
		session->Failed(reason, code);
	}
}

} // namespace Emulator::LauncherMode
