#include "launcherMode.h"

#include "common/launcher/overlayPorts.h"
#include "common/launcher/session.h"
#include "common/launcher/settings.h"
#include "common/logging/log.h"
#include "graphics/presentation/launcherOverlay.h"
#include "graphics/presentation/performanceOverlay.h"
#include "graphics/presentation/window.h"
#include "libs/controller.h"

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
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

// What the window has to do after a live setting changed (Libs::Graphics::WindowFollowConfig)
static uint32_t WindowChanges(std::string_view key) {
	namespace WindowConfig = Libs::Graphics::WindowConfig;
	if (key == "resolution") {
		return WindowConfig::Size;
	}
	if (key == "fullscreen") {
		return WindowConfig::Fullscreen;
	}
	if (key == "hide-cursor") {
		return WindowConfig::Cursor;
	}
	// The rest are read where they're used, each time: the present mode by the next present
	return 0;
}

// The launcher sends only live settings while the game runs (Settings::IsLive). They go into
// Config's live options, which the emulator reads as it goes; one that comes before the emulator
// loads its options waits for that.
static void ApplyLive(const std::string& key, const std::optional<std::string>& value) {
	if (key == PERF_OVERLAY) {
		SetPerformanceOverlay(value.value_or("false") == "true");
		return;
	}
	// Whether the value is valid doesn't depend on the other options
	Config::ConfigOptions check;
	if (const auto error = Launcher::Settings::Apply(check, key, value); !error.empty()) {
		LOGF("[Launcher] setting: %s\n", error.c_str());
		if (auto* session = Launcher::Session::Get(); session != nullptr) {
			session->Notify("A setting wasn't applied", error, true);
		}
		return;
	}
	LOGF("[Launcher] setting '%s' changed while running\n", key.c_str());
	Config::ChangeLive([key, value](Config::ConfigOptions& cfg) {
		(void)Launcher::Settings::Apply(cfg, key, value);
	});
	if (const auto changes = WindowChanges(key); changes != 0) {
		Libs::Graphics::WindowFollowConfig(changes);
	}
}

namespace Overlay {

namespace Ports = Launcher::OverlayPorts;

// The capture waits here until the shell's surfaces are imported, so it goes to the shell only
// when Kyty can draw the menu: the shell shows the menu once it has the capture. A thread of its
// own sends it, keeping socket I/O off the present thread.
static std::mutex                     g_mutex;
static std::condition_variable        g_cv;
static bool                           g_wake = false;
static std::unique_ptr<Ports::DmaBuf> g_capture;

// Held while the service or the surfaces change (overlay_surface) and while sending, so the
// capture never goes to a service whose surfaces aren't Kyty's yet
static std::mutex g_io_mutex;

static void Wake() {
	{
		std::lock_guard lock(g_mutex);
		g_wake = true;
	}
	g_cv.notify_one();
}

static void DropCapture() {
	std::lock_guard lock(g_mutex);
	g_capture.reset();
}

static void SendCapture() {
	std::lock_guard                io(g_io_mutex);
	std::unique_ptr<Ports::DmaBuf> capture;
	{
		std::lock_guard lock(g_mutex);
		if (g_capture == nullptr || !Ports::HasService() ||
		    !Libs::Graphics::LauncherOverlay::SurfacesReady()) {
			return;
		}
		capture = std::move(g_capture);
	}
	auto* session = Launcher::Session::Get();
	if (std::string error; !Ports::SendCapture(*capture, error)) {
		LOGF("[Launcher] couldn't hand the capture to the launcher: %s\n", error.c_str());
	} else if (session != nullptr) {
		session->CaptureSurface(capture->id, capture->width, capture->height);
	}
}

[[noreturn]] static void Sender() {
	for (;;) {
		{
			std::unique_lock lock(g_mutex);
			g_cv.wait(lock, []() { return g_wake; });
			g_wake = false;
		}
		SendCapture();
	}
}

static void OnSurfaces(const Launcher::HostMessage& msg) {
	std::lock_guard io(g_io_mutex);

	// No buffers takes the overlay away. The surfaces come from the shell's service; the ids
	// say which they should be.
	std::optional<Ports::Surfaces> surfaces;
	std::string                    error;
	if (msg.buffers.empty()) {
		Ports::SetService({}, error);
		// The menu is closed: a capture still waiting isn't wanted
		DropCapture();
	} else if (msg.service.empty()) {
		LOGF("[Launcher] overlay_surface: no service to get the surfaces from\n");
	} else if (!Ports::SetService(msg.service, error) ||
	           !(surfaces = Ports::Fetch(msg.buffers, msg.header, error))) {
		LOGF("[Launcher] overlay_surface: %s\n", error.c_str());
		// No capture goes out without the surfaces, so the shell comes to the front instead
		Ports::SetService({}, error);
	}

	// The present thread imports them, then says so (SetReadyCallback)
	if (surfaces) {
		Libs::Graphics::LauncherOverlay::SetSurfaces(std::move(surfaces->buffers),
		                                             std::move(surfaces->header), msg.width,
		                                             msg.height);
	} else {
		Libs::Graphics::LauncherOverlay::SetSurfaces({}, nullptr, 0, 0);
	}
}

static void Start() {
	std::thread(Sender).detach();
	Libs::Graphics::LauncherOverlay::SetCaptureCallback(
	    [](std::unique_ptr<Ports::DmaBuf> capture) {
		    {
			    std::lock_guard lock(g_mutex);
			    g_capture = std::move(capture);
		    }
		    Wake();
	    });
	Libs::Graphics::LauncherOverlay::SetReadyCallback(Wake);
	// The window's size, which the shell lays the menu out for. Only a renderer that can draw
	// the overlay reports it, and the first one is what makes the launcher count on it.
	Libs::Graphics::LauncherOverlay::SetViewCallback([](uint32_t width, uint32_t height) {
		if (auto* session = Launcher::Session::Get(); session != nullptr) {
			session->View(width, height);
		}
	});
}

} // namespace Overlay

static Launcher::Handlers MakeHandlers() {
	Launcher::Handlers handlers;

	handlers.quit = []() {
		// The window loop saves the pipeline cache and exits. One that arrives before the window
		// exists ends the loop as soon as it starts.
		LOGF("[Launcher] quit\n");
		Libs::Graphics::WindowRequestQuit();
	};

	handlers.overlay = [](bool open) {
		// The present thread keeps showing the last frame while the guest's vblanks stop. With
		// the composited overlay, the next of those presents copies that frame for the shell.
		Overlay::DropCapture();
		if (open) {
			Libs::Graphics::LauncherOverlay::RequestCapture();
		} else {
			Libs::Graphics::LauncherOverlay::CancelCapture();
		}
		// The pad is the menu's before the game pauses, and the game's again before it resumes
		Libs::Controller::HoldForLauncher(open);
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
		} else {
			LOGF("[Launcher] '%s' can't change while a game is running\n", key.c_str());
		}
	};

	handlers.overlay_surface = Overlay::OnSurfaces;
	handlers.overlay_visible = [](bool visible) {
		Libs::Graphics::LauncherOverlay::SetVisible(visible);
	};

	return handlers;
}

bool Start(Config::ConfigOptions& config) {
	if (!Launcher::EnvPresent()) {
		return false;
	}

	// The launcher counts on the overlay only once a renderer that can draw it sends view.
	std::vector<std::string> capabilities;
	if (Launcher::OverlayPorts::Supported()) {
		capabilities.emplace_back("composited_overlay");
		Overlay::Start();
	}

	auto* session = Launcher::Session::Connect(MakeHandlers(), capabilities);
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
	} else {
		LOGF("[Launcher] no configure within %d ms: booting with the command line's settings\n",
		     static_cast<int>(CONFIGURE_TIMEOUT.count()));
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
