#ifndef KYTY_COMMON_LAUNCHER_OVERLAYPORTS_H_
#define KYTY_COMMON_LAUNCHER_OVERLAYPORTS_H_

#include "common/config.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// The launcher's composited overlay (its docs/plugin-api.md, "Composited overlay"): the shell's
// quick menu comes as shared surfaces from the service overlay_surface names, and the game's
// capture goes back the same way. On Linux the service is an abstract SOCK_SEQPACKET socket that
// hands out dma-bufs (and the header's memfd) as descriptors. Other platforms have none yet.
// Any thread; calls are serialised.

// Whether this build shares the overlay as dma-bufs. macOS builds as KYTY_PLATFORM_LINUX but has
// no dma-bufs (nor SOCK_CLOEXEC), so it takes the stubs, as Windows does.
#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX && !defined(__APPLE__)
#define KYTY_DMA_BUF_OVERLAY 1
#else
#define KYTY_DMA_BUF_OVERLAY 0
#endif

namespace Common::Launcher::OverlayPorts {

// A dma-buf (or the header's memfd) and its layout: one plane, `stride` bytes a row from
// `offset`, DRM format modifier `modifier`. It owns `fd`.
struct DmaBuf {
	int      fd       = -1;
	uint32_t id       = 0;
	uint32_t width    = 0;
	uint32_t height   = 0;
	uint32_t stride   = 0;
	uint64_t offset   = 0;
	uint64_t modifier = 0;

	DmaBuf()                         = default;
	DmaBuf(const DmaBuf&)            = delete;
	DmaBuf& operator=(const DmaBuf&) = delete;
	~DmaBuf();
};

// Whether this platform can share surfaces with the launcher at all.
[[nodiscard]] bool Supported();

// Connects to `service`, keeping hold of it until another name comes. An empty name lets go.
bool               SetService(const std::string& service, std::string& error);
[[nodiscard]] bool HasService();

struct Surfaces {
	std::vector<std::unique_ptr<DmaBuf>> buffers; // In the order of the ids asked for
	std::unique_ptr<DmaBuf>              header;  // If a header id was asked for
};

// Asks the service for the surfaces, and checks they're the ones with these ids.
std::optional<Surfaces> Fetch(const std::vector<uint32_t>& buffer_ids, uint32_t header_id,
                              std::string& error);

// Hands the game's capture to the service; send capture_surface after this.
bool SendCapture(const DmaBuf& capture, std::string& error);

} // namespace Common::Launcher::OverlayPorts

#endif // KYTY_COMMON_LAUNCHER_OVERLAYPORTS_H_
