#include "common/launcher/settings.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <variant>

namespace Common::Launcher::Settings {

using Config::ConfigOptions;
using Json = nlohmann::json;

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

using Kind = std::variant<Toggle, Number<uint32_t>, Number<int32_t>, Text, PresentModeChoice,
                          Resolution>;

struct Entry {
	std::string_view key;
	std::string_view label;
	std::string_view section;
	Kind             kind;
	bool             live = false;
};

constexpr std::array<std::string_view, 3> PRESENT_MODES = {"Fifo", "Mailbox", "Immediate"};

constexpr std::array<std::string_view, 6> RESOLUTIONS = {"1280x720",  "1600x900",  "1920x1080",
                                                         "2560x1440", "3200x1800", "3840x2160"};

// The options worth a row in the launcher; debugging aids (dumps, validation, logs) are left out.
const std::array ENTRIES = {
    // Display
    Entry {"resolution", "Window size", "Display", Resolution {}},
    Entry {"fullscreen", "Fullscreen", "Display", Toggle {&ConfigOptions::fullscreen_enabled}},
    Entry {"hdr", "HDR", "Display", Toggle {&ConfigOptions::hdr_enabled}},
    Entry {"stretch", "Stretch to window", "Display", Toggle {&ConfigOptions::stretch_enabled}},
    Entry {"present-mode", "Present mode", "Display", PresentModeChoice {}},
    Entry {"vblank-frequency", "Vblank frequency", "Display",
           Number<uint32_t> {&ConfigOptions::vblank_frequency, 30, 240}},
    Entry {"hide-cursor", "Hide the cursor", "Display",
           Toggle {&ConfigOptions::hide_cursor_enabled}},
    Entry {"splash", "Splash screen", "Display",
           Toggle {&ConfigOptions::splash_enabled, false}},
    Entry {"perf-overlay", "Performance overlay", "Display",
           Toggle {&ConfigOptions::perf_overlay_enabled}, true},
    // Graphics
    Entry {"gpu", "GPU (-1 picks one)", "Graphics",
           Number<int32_t> {&ConfigOptions::gpu_index, -1, 7}},
    Entry {"shader-cache", "Keep compiled shaders", "Graphics",
           Toggle {&ConfigOptions::shader_cache_enabled}},
    Entry {"tessellation", "Tessellation", "Graphics",
           Toggle {&ConfigOptions::tessellation_enabled}},
    Entry {"occlusion-queries", "Occlusion queries", "Graphics",
           Toggle {&ConfigOptions::occlusion_queries_enabled}},
    Entry {"readback-linear-images", "Read back linear images", "Graphics",
           Toggle {&ConfigOptions::readback_linear_images}},
    Entry {"readback-tiled-images", "Read back tiled images", "Graphics",
           Toggle {&ConfigOptions::readback_tiled_images}},
    Entry {"sync-raw-image-buffers", "Synchronize raw image reads", "Graphics",
           Toggle {&ConfigOptions::sync_raw_image_buffers}},
    // System
    Entry {"console-language", "Console language", "System",
           Number<uint32_t> {&ConfigOptions::console_language, 0, Config::MAX_CONSOLE_LANGUAGE}},
    Entry {"user-name", "User name", "System", Text {&ConfigOptions::user_name}},
    Entry {"network-connected", "Network", "System", Toggle {&ConfigOptions::network_connected}},
    Entry {"skip-notice-screen", "Skip notice screens", "System",
           Toggle {&ConfigOptions::skip_notice_screen}},
    Entry {"playgo-hack", "PlayGo fallback", "System", Toggle {&ConfigOptions::playgo_hack_enabled}},
    Entry {"amd-cpu", "AMD CPU patches", "System", Toggle {&ConfigOptions::amd_cpu_enabled}},
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
    Entry {"redzone", "Red zone protection", "System",
           Toggle {&ConfigOptions::red_zone_protection_enabled}},
#endif
    // Controls
    Entry {"controller-volume", "Controller speaker volume", "Controls",
           Number<uint32_t> {&ConfigOptions::controller_speaker_volume, 0, 100}},
    Entry {"controller-vibration", "Controller vibration", "Controls",
           Number<uint32_t> {&ConfigOptions::controller_vibration_intensity, 0, 100}},
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
		    } else {
			    return std::to_string(cfg.*kind.field);
		    }
	    },
	    e.kind);
}

Json KindJson(const Entry& e) {
	return std::visit(
	    [&](const auto& kind) -> Json {
		    using K = std::decay_t<decltype(kind)>;
		    if constexpr (std::is_same_v<K, Toggle>) {
			    return "toggle";
		    } else if constexpr (std::is_same_v<K, Text>) {
			    return {{"text", Json::object()}};
		    } else if constexpr (std::is_same_v<K, PresentModeChoice>) {
			    return {{"choice", PRESENT_MODES}};
		    } else if constexpr (std::is_same_v<K, Resolution>) {
			    return {{"choice", RESOLUTIONS}};
		    } else {
			    return {{"number", {{"min", kind.min}, {"max", kind.max}}}};
		    }
	    },
	    e.kind);
}

} // namespace

std::string Describe() {
	const ConfigOptions defaults;
	Json                rows = Json::array();
	for (const auto& e: ENTRIES) {
		rows.push_back({{"key", e.key},
		                {"label", e.label},
		                {"section", e.section},
		                {"kind", KindJson(e)},
		                {"default", ValueOf(e, defaults)},
		                {"live", e.live}});
	}
	return rows.dump(1, '\t');
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
