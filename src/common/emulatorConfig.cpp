#include "common/emulatorConfig.h"

#include "common/assert.h"
#include "common/debugCounters.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace Config {

static std::unique_ptr<ConfigOptions> g_config;

// The options that can change while a game runs (ChangeLive), read from any thread.
struct LiveOptions {
	// The width in the high half, so the two never tear
	std::atomic<uint64_t> screen_size {};
	std::atomic<uint32_t> controller_speaker_volume {};
	std::atomic<uint32_t> controller_vibration_intensity {};
	std::atomic<PresentMode> present_mode {};
	std::atomic<bool> fullscreen_enabled {};
	std::atomic<bool> hide_cursor_enabled {};
	std::atomic<bool> stretch_enabled {};
	std::atomic<uint32_t> vblank_frequency {};
	std::atomic<bool> network_connected {};
	std::atomic<bool> sync_raw_image_buffers {};
	std::atomic<bool> tessellation_enabled {};
};

static LiveOptions g_live;

// Held while ChangeLive changes them, and while Load and Shutdown make or drop g_config
static std::mutex g_live_mutex;
static bool       g_loaded = false;
// Changes made before Load, which it applies over its options
static std::vector<std::function<void(ConfigOptions&)>> g_pending_live;

static void StoreLive(const ConfigOptions& cfg);

void Initialize() {
	EXIT_IF(g_config != nullptr);

	g_config = std::make_unique<ConfigOptions>();
}

void Shutdown() {
	Common::DebugCounters::SetEnabled(false);
	std::lock_guard lock(g_live_mutex);
	g_loaded = false;
	g_config.reset();
}

void Load(const ConfigOptions& cfg) {
	EXIT_IF(g_config == nullptr);
	EXIT_IF(cfg.user_name.empty() || cfg.user_name.size() > MAX_USER_NAME_LENGTH);
	EXIT_IF(!IsConfiguredUserIdValid(cfg.user_id));
	EXIT_IF(cfg.controller_speaker_volume > 100 || cfg.controller_vibration_intensity > 100);
	EXIT_IF(cfg.keyboard_player > static_cast<uint32_t>(MAX_PLAYERS));

	std::lock_guard lock(g_live_mutex);
	*g_config = cfg;
	auto live = cfg;
	for (const auto& change: std::exchange(g_pending_live, {})) {
		change(live);
	}
	StoreLive(live);
	g_loaded = true;
	Common::DebugCounters::SetEnabled(cfg.perf_overlay_enabled);
}

// The options as they are now
static ConfigOptions Current() {
	ConfigOptions cfg = *g_config;
	const uint64_t size = g_live.screen_size.load(std::memory_order_relaxed);
	cfg.screen_width  = static_cast<uint32_t>(size >> 32u);
	cfg.screen_height = static_cast<uint32_t>(size);
	cfg.controller_speaker_volume = g_live.controller_speaker_volume.load(std::memory_order_relaxed);
	cfg.controller_vibration_intensity = g_live.controller_vibration_intensity.load(std::memory_order_relaxed);
	cfg.present_mode = g_live.present_mode.load(std::memory_order_relaxed);
	cfg.fullscreen_enabled = g_live.fullscreen_enabled.load(std::memory_order_relaxed);
	cfg.hide_cursor_enabled = g_live.hide_cursor_enabled.load(std::memory_order_relaxed);
	cfg.stretch_enabled = g_live.stretch_enabled.load(std::memory_order_relaxed);
	cfg.vblank_frequency = g_live.vblank_frequency.load(std::memory_order_relaxed);
	cfg.network_connected = g_live.network_connected.load(std::memory_order_relaxed);
	cfg.sync_raw_image_buffers = g_live.sync_raw_image_buffers.load(std::memory_order_relaxed);
	cfg.tessellation_enabled = g_live.tessellation_enabled.load(std::memory_order_relaxed);
	return cfg;
}

void ChangeLive(std::function<void(ConfigOptions&)> change) {
	std::lock_guard lock(g_live_mutex);
	if (!g_loaded) {
		g_pending_live.push_back(std::move(change));
		return;
	}
	auto cfg = Current();
	change(cfg);
	StoreLive(cfg);
}

static void StoreLive(const ConfigOptions& cfg) {
	g_live.screen_size.store((static_cast<uint64_t>(cfg.screen_width) << 32u) | cfg.screen_height,
	                         std::memory_order_relaxed);
	g_live.controller_speaker_volume.store(cfg.controller_speaker_volume, std::memory_order_relaxed);
	g_live.controller_vibration_intensity.store(cfg.controller_vibration_intensity, std::memory_order_relaxed);
	g_live.present_mode.store(cfg.present_mode, std::memory_order_relaxed);
	g_live.fullscreen_enabled.store(cfg.fullscreen_enabled, std::memory_order_relaxed);
	g_live.hide_cursor_enabled.store(cfg.hide_cursor_enabled, std::memory_order_relaxed);
	g_live.stretch_enabled.store(cfg.stretch_enabled, std::memory_order_relaxed);
	g_live.vblank_frequency.store(cfg.vblank_frequency, std::memory_order_relaxed);
	g_live.network_connected.store(cfg.network_connected, std::memory_order_relaxed);
	g_live.sync_raw_image_buffers.store(cfg.sync_raw_image_buffers, std::memory_order_relaxed);
	g_live.tessellation_enabled.store(cfg.tessellation_enabled, std::memory_order_relaxed);
}

uint32_t GetScreenWidth() {
	return static_cast<uint32_t>(g_live.screen_size.load(std::memory_order_relaxed) >> 32u);
}

uint32_t GetScreenHeight() {
	return static_cast<uint32_t>(g_live.screen_size.load(std::memory_order_relaxed));
}

const std::string& GetUserName() {
	return g_config->user_name;
}

int32_t GetUserId() {
	return g_config->user_id;
}

const std::string& GetAudioInputDevice() {
	return g_config->audio_input_device;
}

const std::optional<ControllerColor>& GetControllerColor() {
	return g_config->controller_color;
}

uint32_t GetControllerSpeakerVolume() {
	return g_live.controller_speaker_volume.load(std::memory_order_relaxed);
}

uint32_t GetControllerVibrationIntensity() {
	return g_live.controller_vibration_intensity.load(std::memory_order_relaxed);
}

uint32_t GetKeyboardPlayer() {
	return g_config->keyboard_player;
}

int32_t GetPlayerUserId(int player) {
	return g_config->user_id + player * PLAYER_USER_ID_STEP;
}

int GetPlayerFromUserId(int32_t user_id) {
	for (int player = 0; player < MAX_PLAYERS; player++) {
		if (user_id == GetPlayerUserId(player)) {
			return player;
		}
	}
	return -1;
}

bool IsGuestUserId(int32_t user_id) {
	return GetPlayerFromUserId(user_id) > 0;
}

PresentMode GetPresentMode() {
	return g_live.present_mode.load(std::memory_order_relaxed);
}

int32_t GetGpuIndex() {
	return g_config->gpu_index;
}

bool FullscreenEnabled() {
	return g_live.fullscreen_enabled.load(std::memory_order_relaxed);
}

bool HdrEnabled() {
	return g_config->hdr_enabled;
}

bool HideCursorEnabled() {
	return g_live.hide_cursor_enabled.load(std::memory_order_relaxed);
}

bool StretchEnabled() {
	return g_live.stretch_enabled.load(std::memory_order_relaxed);
}

bool VrEnabled() {
	return g_config->vr_enabled;
}

bool Ps5ProEnabled() {
	return g_config->pro_enabled;
}

bool AmdCpuEnabled() {
	return g_config->amd_cpu_enabled;
}

bool GuestTscEnabled() {
	return g_config->guest_tsc_enabled;
}

uint32_t GetVblankFrequency() {
	return std::clamp(g_live.vblank_frequency.load(std::memory_order_relaxed), 30u, 360u);
}

uint32_t GetConsoleLanguage() {
	return g_config->console_language;
}

bool VulkanValidationEnabled() {
	return g_config->vulkan_validation_enabled;
}

bool IsNetworkConnected() {
	return g_live.network_connected.load(std::memory_order_relaxed);
}

bool PsnEmulationEnabled() {
	return g_config->psn_emulation_enabled;
}

bool ShaderValidationEnabled() {
	return g_config->shader_validation_enabled;
}

bool ShaderCacheEnabled() {
	return g_config->shader_cache_enabled;
}

ShaderOptimizationType GetShaderOptimizationType() {
	return g_config->shader_optimization_type;
}

LogDirection GetShaderLogDirection() {
	return g_config->shader_log_direction;
}

std::filesystem::path GetShaderLogFolder() {
	return g_config->shader_log_folder;
}

bool CommandBufferDumpEnabled() {
	return g_config->command_buffer_dump_enabled;
}

std::filesystem::path GetCommandBufferDumpFolder() {
	return g_config->command_buffer_dump_folder;
}

bool GraphicsDebugDumpEnabled() {
	return g_config->graphics_debug_dump_enabled;
}

LogDirection GetPrintfDirection() {
	return g_config->printf_direction;
}

std::filesystem::path GetPrintfOutputFile() {
	return g_config->printf_output_file;
}

bool ProfilerEnabled() {
	return g_config->profiler_enabled;
}

bool SpirvDebugPrintfEnabled() {
	return g_config->spirv_debug_printf_enabled;
}

bool GpuAssistedValidationEnabled() {
	return g_config->gpu_assisted_validation_enabled && g_config->vulkan_validation_enabled;
}

bool RenderDocEnabled() {
	return g_config->renderdoc_enabled;
}

bool ReadbackLinearImagesEnabled() {
	return g_config->readback_linear_images;
}

bool ReadbackTiledImagesEnabled() {
	return g_config->readback_tiled_images;
}

bool SyncRawImageBuffersEnabled() {
	return g_live.sync_raw_image_buffers.load(std::memory_order_relaxed);
}

bool TessellationEnabled() {
	return g_live.tessellation_enabled.load(std::memory_order_relaxed);
}

bool OcclusionQueriesEnabled() {
	return g_config->occlusion_queries_enabled;
}

bool TrophyEnabled() {
	return g_config->trophy_enabled;
}

uint32_t GetTrophyDurationSeconds() {
	return std::clamp(g_config->trophy_duration_seconds, 5u, 10u);
}

bool PlayGoHackEnabled() {
	return g_config->playgo_hack_enabled;
}

bool SkipNoticeScreen() {
	return g_config->skip_notice_screen;
}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
bool RedZoneProtectionEnabled() {
	return g_config->red_zone_protection_enabled;
}
#endif

const Keymap& GetKeymap() {
	return g_config->keymap;
}

const std::vector<std::string>& GetGuestArguments() {
	return g_config->guest_arguments;
}

} // namespace Config
