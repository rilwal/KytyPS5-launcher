#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_WINDOW_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_WINDOW_H_

#include "common/abi.h"
#include "common/common.h"

namespace Libs::Graphics {

class Presenter;

[[nodiscard]] Presenter& WindowInit(uint32_t width, uint32_t height);
void                     WindowRun();
void                     WindowShutdown();

// Launcher mode (common/launcher/session.h). Safe to call from any thread.
void WindowRequestQuit();
void WindowSetLauncherPaused(bool paused);

// What WindowFollowConfig can follow, as a mask.
namespace WindowConfig {
inline constexpr uint32_t Size       = 1u << 0u; // Config::GetScreenWidth/Height
inline constexpr uint32_t Fullscreen = 1u << 1u; // Config::FullscreenEnabled
inline constexpr uint32_t Cursor     = 1u << 2u; // Config::HideCursorEnabled
} // namespace WindowConfig

// After Config::ChangeLive changed those options: the window follows them on the main thread.
// Before the window exists, nothing needs doing, since it's made with the options as they are.
// Safe to call from any thread.
void WindowFollowConfig(uint32_t changes);

// Nits of the display showing the game window; the defaults apply when the host reports none.
struct DisplayLuminance {
	float max_full_frame = 80.0f;
	float max            = 1000.0f;
	float min            = 0.0f;
};

[[nodiscard]] DisplayLuminance WindowGetDisplayLuminance();

} // namespace Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_WINDOW_H_ */
