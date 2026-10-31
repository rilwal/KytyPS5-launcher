#include "common/launcher/settings.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <fmt/format.h>
#include <variant>

namespace Common::Launcher::Settings {

using Config::ConfigOptions;

namespace {

struct Toggle {
	bool ConfigOptions::* field;
	bool                  inverted = false; // The option turns the field off (splash, trophies)
};

template <typename T>
struct Number {
	T ConfigOptions::* field;
	T                  min;
	T                  max;
};

struct Text {
	std::string ConfigOptions::* field;
};

struct PresentModeChoice {};
struct Resolution {};
struct LanguageChoice {};

using Kind = std::variant<Toggle, Number<uint32_t>, Number<int32_t>, Text, PresentModeChoice,
                          Resolution, LanguageChoice>;

struct Entry {
	std::string_view key;
	std::string_view label;
	std::string_view section;
	Kind             kind;
	std::string_view help;
	bool             live = false;
};

// What a row that can't change while a game runs says
constexpr std::string_view RESTART_NOTE = "Takes effect when the game next starts.";

constexpr std::array<std::string_view, 3> PRESENT_MODES = {"Fifo", "Mailbox", "Immediate"};

constexpr std::array<std::string_view, 6> RESOLUTIONS = {"1280x720",  "1600x900",  "1920x1080",
                                                         "2560x1440", "3200x1800", "3840x2160"};

// By the console's language number (SceSystemParamLang)
constexpr std::array<std::string_view, Config::MAX_CONSOLE_LANGUAGE + 1> LANGUAGES = {
    "Japanese", "English (US)", "French", "Spanish", "German", "Italian", "Dutch",
    "Portuguese (Portugal)", "Russian", "Korean", "Chinese (Traditional)", "Chinese (Simplified)",
    "Finnish", "Swedish", "Danish", "Norwegian", "Polish", "Portuguese (Brazil)", "English (UK)",
    "Turkish", "Spanish (Latin America)", "Arabic", "French (Canada)", "Czech", "Hungarian",
    "Greek", "Romanian", "Thai", "Vietnamese", "Indonesian"};

// The options worth a row in the launcher; debugging aids (dumps, validation, logs) are left out.
// Those marked live change while a game runs (launcherMode.cpp); the rest wait for the next boot.
const std::array ENTRIES = {
    // Display
    Entry {"resolution", "Window size", "Display", Resolution {},
           "The game window's size when it isn't fullscreen. The output resolution games see is set at boot.", true},
    Entry {"fullscreen", "Fullscreen", "Display", Toggle {&ConfigOptions::fullscreen_enabled},
           "Borderless fullscreen on the window's display.", true},
    Entry {"hdr", "HDR", "Display", Toggle {&ConfigOptions::hdr_enabled},
           "Output HDR10 when the game and the display support it."},
    Entry {"stretch", "Stretch to window", "Display", Toggle {&ConfigOptions::stretch_enabled},
           "Fill the window instead of keeping the game's aspect ratio.", true},
    Entry {"present-mode", "Present mode", "Display", PresentModeChoice {},
           "Fifo waits for the display, Mailbox replaces waiting frames, Immediate can tear.",
           true},
    Entry {"vblank-frequency", "Vblank frequency", "Display",
           Number<uint32_t> {&ConfigOptions::vblank_frequency, 30, 240},
           "The emulated display's refresh rate, in Hz. A game picks its own 120 Hz mode at boot.", true},
    Entry {"hide-cursor", "Hide the cursor", "Display",
           Toggle {&ConfigOptions::hide_cursor_enabled},
           "Hide the mouse cursor after 2 seconds without moving.", true},
    Entry {"splash", "Splash screen", "Display",
           Toggle {&ConfigOptions::splash_enabled, false},
           "Show the splash screen while the game starts."},
    Entry {"perf-overlay", "Performance overlay", "Display",
           Toggle {&ConfigOptions::perf_overlay_enabled},
           "Show frame rate and timings over the game.", true},
    // Graphics
    Entry {"gpu", "GPU (-1 picks one)", "Graphics",
           Number<int32_t> {&ConfigOptions::gpu_index, -1, 7},
           "The Vulkan device to use, counted from 0."},
    Entry {"shader-cache", "Keep compiled shaders", "Graphics",
           Toggle {&ConfigOptions::shader_cache_enabled},
           "Keep compiled shaders and pipelines between boots."},
    Entry {"tessellation", "Tessellation", "Graphics",
           Toggle {&ConfigOptions::tessellation_enabled},
           "Draw tessellation patches, which are skipped when off.", true},
    Entry {"occlusion-queries", "Occlusion queries", "Graphics",
           Toggle {&ConfigOptions::occlusion_queries_enabled},
           "Count occlusion query samples on the GPU."},
    Entry {"readback-linear-images", "Read back linear images", "Graphics",
           Toggle {&ConfigOptions::readback_linear_images},
           "Read back writable linear images on submit."},
    Entry {"readback-tiled-images", "Read back tiled images", "Graphics",
           Toggle {&ConfigOptions::readback_tiled_images},
           "Read back small writable tiled images on submit."},
    Entry {"sync-raw-image-buffers", "Synchronize raw image reads", "Graphics",
           Toggle {&ConfigOptions::sync_raw_image_buffers},
           "Synchronize raw reads of GPU images.", true},
    Entry {"ps5-pro", "PS5 Pro mode (experimental)", "Graphics",
           Toggle {&ConfigOptions::pro_enabled},
           "PS5 Pro mode for games that have one (PSSR uses linear scaling)."},
    // System
    Entry {"console-language", "Console language", "System", LanguageChoice {},
           "The console's language, which games and trophy lists use."},
    Entry {"user-name", "User name", "System", Text {&ConfigOptions::user_name},
           "The player's name on the emulated console."},
    Entry {"network-connected", "Network", "System", Toggle {&ConfigOptions::network_connected},
           "Use the host's local network.", true},
    Entry {"emulate-psn", "Simulate PSN sign-in", "System",
           Toggle {&ConfigOptions::psn_emulation_enabled},
           "Simulate a PSN sign-in, which some games need."},
    Entry {"skip-notice-screen", "Skip notice screens", "System",
           Toggle {&ConfigOptions::skip_notice_screen},
           "Skip startup logos and notices in games that support it."},
    Entry {"playgo-hack", "PlayGo fallback", "System", Toggle {&ConfigOptions::playgo_hack_enabled},
           "Use the supplied PlayGo stub as a fallback."},
    Entry {"amd-cpu", "AMD CPU patches", "System", Toggle {&ConfigOptions::amd_cpu_enabled},
           "Apply AMD CPU instruction patches."},
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
    Entry {"redzone", "Red zone protection", "System",
           Toggle {&ConfigOptions::red_zone_protection_enabled},
           "Protect the guest's SysV red zone."},
#endif
    // Controls
    Entry {"controller-volume", "Controller speaker volume", "Controls",
           Number<uint32_t> {&ConfigOptions::controller_speaker_volume, 0, 100},
           "The controller speaker's volume, in percent.", true},
    Entry {"controller-vibration", "Controller vibration", "Controls",
           Number<uint32_t> {&ConfigOptions::controller_vibration_intensity, 0, 100},
           "How strongly the controller vibrates, in percent.", true},
};

const Entry* Find(std::string_view key) {
	const auto it = std::ranges::find(ENTRIES, key, &Entry::key);
	return it != ENTRIES.end() ? &*it : nullptr;
}

std::string_view PresentModeName(Config::PresentMode mode) {
	return PRESENT_MODES.at(static_cast<size_t>(mode));
}

template <typename T>
bool ParseNumber(std::string_view text, T& out) {
	auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), out);
	return error == std::errc {} && end == text.data() + text.size();
}

// The current value of an entry in `cfg`, as text.
std::string ValueOf(const Entry& e, const ConfigOptions& cfg) {
	return std::visit(
	    [&](const auto& kind) -> std::string {
		    using K = std::decay_t<decltype(kind)>;
		    if constexpr (std::is_same_v<K, Toggle>) {
			    return (cfg.*kind.field != kind.inverted) ? "true" : "false";
		    } else if constexpr (std::is_same_v<K, Text>) {
			    return cfg.*kind.field;
		    } else if constexpr (std::is_same_v<K, PresentModeChoice>) {
			    return std::string(PresentModeName(cfg.present_mode));
		    } else if constexpr (std::is_same_v<K, Resolution>) {
			    return fmt::format("{}x{}", cfg.screen_width, cfg.screen_height);
		    } else if constexpr (std::is_same_v<K, LanguageChoice>) {
			    return std::string(LANGUAGES.at(cfg.console_language));
		    } else {
			    return std::to_string(cfg.*kind.field);
		    }
	    },
	    e.kind);
}

} // namespace

std::vector<Row> PluginRows() {
	const ConfigOptions defaults;
	std::vector<Row>    rows;
	for (const auto& entry: ENTRIES) {
		const Entry* e = &entry;
		Row          row;
		row.key           = std::string(e->key);
		row.label         = std::string(e->label);
		row.section       = std::string(e->section);
		row.default_value = ValueOf(*e, defaults);
		row.live          = e->live;
		row.help          = std::string(e->help);
		if (!e->live) {
			row.help += fmt::format("{}{}", row.help.empty() ? "" : " ", RESTART_NOTE);
		}
		row.runner        = e->key == "console-language";
		std::visit(
		    [&](const auto& kind) {
			    using K = std::decay_t<decltype(kind)>;
			    if constexpr (std::is_same_v<K, Toggle>) {
				    row.kind = Row::Kind::Toggle;
			    } else if constexpr (std::is_same_v<K, Text>) {
				    row.kind   = Row::Kind::Text;
				    row.runner = true;
			    } else if constexpr (std::is_same_v<K, PresentModeChoice>) {
				    row.kind = Row::Kind::Choice;
				    row.choices.assign(PRESENT_MODES.begin(), PRESENT_MODES.end());
			    } else if constexpr (std::is_same_v<K, Resolution>) {
				    row.kind = Row::Kind::Choice;
				    row.choices.assign(RESOLUTIONS.begin(), RESOLUTIONS.end());
			    } else if constexpr (std::is_same_v<K, LanguageChoice>) {
				    row.kind = Row::Kind::Choice;
				    row.choices.assign(LANGUAGES.begin(), LANGUAGES.end());
			    } else {
				    // At most 25 steps, always with the default among them
				    row.kind       = Row::Kind::Choice;
				    const auto lo  = static_cast<int64_t>(kind.min);
				    const auto hi  = static_cast<int64_t>(kind.max);
				    const auto gap = std::max<int64_t>(1, (hi - lo + 23) / 24);
				    for (int64_t v = lo; v <= hi; v += gap) {
					    row.choices.push_back(std::to_string(v));
				    }
				    if (row.choices.back() != std::to_string(hi)) {
					    row.choices.push_back(std::to_string(hi));
				    }
				    if (std::ranges::find(row.choices, row.default_value) == row.choices.end()) {
					    row.choices.push_back(row.default_value);
					    std::ranges::sort(row.choices, {}, [](const std::string& c) { return std::stoll(c); });
				    }
			    }
		    },
		    e->kind);
		rows.push_back(std::move(row));
	}
	return rows;
}

bool IsLive(std::string_view key) {
	const Entry* e = Find(key);
	return e != nullptr && e->live;
}

std::string Apply(ConfigOptions& cfg, std::string_view key,
                  const std::optional<std::string>& value_or_default) {
	const Entry* e = Find(key);
	if (e == nullptr) {
		return fmt::format("Kyty has no setting '{}'.", key);
	}

	const std::string value   = value_or_default.value_or(ValueOf(*e, ConfigOptions {}));
	const auto        invalid = [&]() {
		return fmt::format("'{}' isn't a valid value for '{}'.", value, e->label);
	};

	return std::visit(
	    [&](const auto& kind) -> std::string {
		    using K = std::decay_t<decltype(kind)>;
		    if constexpr (std::is_same_v<K, Toggle>) {
			    if (value != "true" && value != "false") {
				    return invalid();
			    }
			    cfg.*kind.field = (value == "true") != kind.inverted;
		    } else if constexpr (std::is_same_v<K, Text>) {
			    cfg.*kind.field = value;
		    } else if constexpr (std::is_same_v<K, PresentModeChoice>) {
			    const auto it = std::ranges::find(PRESENT_MODES, value);
			    if (it == PRESENT_MODES.end()) {
				    return invalid();
			    }
			    cfg.present_mode =
			        static_cast<Config::PresentMode>(std::distance(PRESENT_MODES.begin(), it));
		    } else if constexpr (std::is_same_v<K, Resolution>) {
			    const auto x      = value.find('x');
			    uint32_t   width  = 0;
			    uint32_t   height = 0;
			    if (x == std::string::npos ||
			        !ParseNumber(std::string_view(value).substr(0, x), width) ||
			        !ParseNumber(std::string_view(value).substr(x + 1), height) || width == 0 ||
			        height == 0) {
				    return invalid();
			    }
			    cfg.screen_width  = width;
			    cfg.screen_height = height;
		    } else if constexpr (std::is_same_v<K, LanguageChoice>) {
			    // The name, or the number as on the command line
			    uint32_t   number = 0;
			    const auto it     = std::ranges::find(LANGUAGES, value);
			    if (it != LANGUAGES.end()) {
				    number = static_cast<uint32_t>(std::distance(LANGUAGES.begin(), it));
			    } else if (!ParseNumber(value, number) || number > Config::MAX_CONSOLE_LANGUAGE) {
				    return invalid();
			    }
			    cfg.console_language = number;
		    } else {
			    std::remove_cvref_t<decltype(cfg.*kind.field)> number {};
			    if (!ParseNumber(value, number) || number < kind.min || number > kind.max) {
				    return invalid();
			    }
			    cfg.*kind.field = number;
		    }
		    return {};
	    },
	    e->kind);
}

} // namespace Common::Launcher::Settings
