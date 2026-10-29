#include "launcherMode.h"

#include "common/launcher/session.h"
#include "common/launcher/settings.h"
#include "common/logging/log.h"
#include "graphics/presentation/performanceOverlay.h"
#include "graphics/presentation/window.h"

#include <chrono>
#include <cstdlib>
#include <string>
#include <vector>

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

// The quick menu's items. Values are read when the menu opens, since F2 also toggles the panel.
static void SendOverlayItems(Launcher::Session& session, bool perf_overlay) {
	Launcher::Setting perf;
	perf.key           = PERF_OVERLAY;
	perf.section       = "Kyty";
	perf.label         = "Performance overlay";
	perf.help          = "Frame times and emulator counters in the corner of the game.";
	perf.kind          = Launcher::Setting::Kind::Toggle;
	perf.value         = perf_overlay ? "true" : "false";
	perf.default_value = "false";
	session.OverlayItems({perf});
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
		// The window loop saves the pipeline cache and exits. Before the window exists there is
		// nothing to save.
		LOGF("[Launcher] quit\n");
		Libs::Graphics::WindowRequestQuit();
	};

	handlers.overlay = [](bool open) {
		auto* session = Launcher::Session::Get();
		if (open && session != nullptr) {
			SendOverlayItems(*session, Libs::Graphics::PerformanceOverlayEnabled());
		}
		// The present thread keeps showing the last frame while the guest's vblanks stop.
		Libs::Graphics::WindowSetLauncherPaused(open);
		if (session != nullptr) {
			session->Paused(open);
		}
	};

	handlers.invoke = [](const std::string& key, const std::optional<std::string>& value) {
		ApplyLive(key, value);
		if (auto* session = Launcher::Session::Get(); session != nullptr) {
			SendOverlayItems(*session, Libs::Graphics::PerformanceOverlayEnabled());
		}
	};

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

	SendOverlayItems(*session, config.perf_overlay_enabled);
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
