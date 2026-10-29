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

} // namespace Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_WINDOW_H_ */
