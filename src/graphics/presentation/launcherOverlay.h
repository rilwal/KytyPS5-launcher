#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_LAUNCHEROVERLAY_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_LAUNCHEROVERLAY_H_

#include "common/common.h"
#include "common/launcher/overlayPorts.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <functional>
#include <memory>
#include <vector>

// The launcher's quick menu, drawn into Kyty's frames (the launcher's docs/plugin-api.md,
// "Composited overlay"). The shell draws the menu into three linear BGRA8 dma-bufs, premultiplied,
// and trades them with Kyty through a header of four u32s:
//   [0] 'LOV2' (0x4C4F5632)   [1] exchange word   [2] the shell's publish count   [3] reserved
// The shell owns one buffer, Kyty one, and the exchange word holds the third: its index, with bit
// 31 set when it's newer than Kyty's. They start as shell 0, exchange 1, Kyty 2. The shell
// publishes by swapping (its buffer | 1<<31) into [1]; Kyty, seeing bit 31, swaps its own index
// in once no GPU work still reads that buffer, and draws the one it got. Without a header, the one
// buffer is drawn as it is.
//
// When the menu opens, Kyty copies the frame it paused on into a dma-buf of its own, which goes to
// the shell as the menu's background (capture_surface).
namespace Libs::Graphics {

class CommandScheduler;
struct GraphicContext;

namespace LauncherOverlay {

using Common::Launcher::OverlayPorts::DmaBuf;

// Any thread. Takes the surfaces over; no buffers removes the overlay.
void SetSurfaces(std::vector<std::unique_ptr<DmaBuf>> buffers, std::unique_ptr<DmaBuf> header,
                 uint32_t width, uint32_t height);
void SetVisible(bool visible);

// Copies the next presented game frame for the shell. Any thread.
void RequestCapture();
void CancelCapture();

// Called on the present thread: with the capture (a new dma-buf for the callee) once its copy
// has finished on the GPU, and with the window's size whenever it changes, if this GPU can draw
// the overlay at all.
void SetCaptureCallback(std::function<void(std::unique_ptr<DmaBuf>)> callback);
void SetViewCallback(std::function<void(uint32_t width, uint32_t height)> callback);

// Whether presents are needed to show the overlay or finish a capture, though no game frame
// comes (e.g. while loading). Any thread.
[[nodiscard]] bool WantsPresents();

// The Vulkan side, used by the swapchain on the present thread.
class Renderer final {
public:
	explicit Renderer(GraphicContext& graphics);
	~Renderer();
	KYTY_CLASS_NO_COPY(Renderer);

	// Before recording a present: takes new surfaces, frees old ones the GPU is done with, and
	// reports a finished capture.
	void BeginPresent(CommandScheduler& scheduler, vk::Extent2D window);

	// Copies `source` (in TransferSrcOptimal) into the capture if one was asked for.
	void Capture(vk::CommandBuffer command, vk::Image source, vk::Extent2D extent);

	// The buffer to draw over this present, in ShaderReadOnlyOptimal, or null for none.
	[[nodiscard]] vk::ImageView Acquire(vk::CommandBuffer command);

	// After submitting the present: the tick its GPU work signals.
	void EndPresent(uint64_t tick);

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

} // namespace LauncherOverlay
} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_LAUNCHEROVERLAY_H_
