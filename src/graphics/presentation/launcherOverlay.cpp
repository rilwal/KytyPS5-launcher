#include "graphics/presentation/launcherOverlay.h"

#include "common/logging/log.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <mutex>
#include <optional>
#include <random>
#include <utility>

#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX
#include <sys/mman.h>
#include <sys/stat.h>
#endif

namespace Libs::Graphics::LauncherOverlay {

namespace {

// Surfaces from SetSurfaces, until the present thread takes them
struct Config {
	std::vector<std::unique_ptr<DmaBuf>> buffers;
	std::unique_ptr<DmaBuf>              header;
	uint32_t                             width  = 0;
	uint32_t                             height = 0;
};

// Presents still needed after a change, so the screen catches up even with no game frames
// (hiding the overlay, say)
constexpr uint32_t PRESENTS_AFTER_CHANGE = 3;

std::mutex                                   g_mutex;
std::optional<Config>                        g_pending;
std::function<void(std::unique_ptr<DmaBuf>)> g_capture_callback;
std::function<void(uint32_t, uint32_t)>      g_view_callback;
std::function<void()>                        g_ready_callback;
std::atomic<bool>                            g_ready {false};
std::atomic<bool>                            g_visible {false};
std::atomic<bool>                            g_capture_requested {false};
std::atomic<bool>                            g_capture_reporting {false};
std::atomic<uint32_t>                        g_presents_owed {0};

constexpr uint64_t DRM_FORMAT_MOD_LINEAR = 0;
constexpr auto     FORMAT                = vk::Format::eB8G8R8A8Unorm;

// With queue families, an ownership transfer from or to the shell's process
void Barrier(vk::CommandBuffer command, vk::Image image, vk::ImageLayout from, vk::ImageLayout to,
             vk::AccessFlags src_access, vk::AccessFlags dst_access,
             vk::PipelineStageFlags src_stage, vk::PipelineStageFlags dst_stage,
             uint32_t src_queue = VK_QUEUE_FAMILY_IGNORED,
             uint32_t dst_queue = VK_QUEUE_FAMILY_IGNORED) {
	vk::ImageMemoryBarrier barrier {};
	barrier.srcAccessMask       = src_access;
	barrier.dstAccessMask       = dst_access;
	barrier.oldLayout           = from;
	barrier.newLayout           = to;
	barrier.srcQueueFamilyIndex = src_queue;
	barrier.dstQueueFamilyIndex = dst_queue;
	barrier.image               = image;
	barrier.subresourceRange    = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
	command.pipelineBarrier(src_stage, dst_stage, {}, 0, nullptr, 0, nullptr, 1, &barrier);
}

} // namespace

void SetSurfaces(std::vector<std::unique_ptr<DmaBuf>> buffers, std::unique_ptr<DmaBuf> header,
                 uint32_t width, uint32_t height) {
	Config config;
	config.buffers = std::move(buffers);
	config.header  = std::move(header);
	config.width   = width;
	config.height  = height;

	// Replacing a config the present thread hasn't taken yet closes its surfaces
	std::lock_guard lock(g_mutex);
	g_pending       = std::move(config);
	g_ready         = false;
	g_presents_owed = PRESENTS_AFTER_CHANGE;
}

void SetVisible(bool visible) {
	if (g_visible.exchange(visible) != visible) {
		g_presents_owed = PRESENTS_AFTER_CHANGE;
	}
}

void RequestCapture() {
	g_capture_requested = true;
}

void CancelCapture() {
	g_capture_requested = false;
	// A copy still in flight isn't reported either
	g_capture_reporting = false;
}

void SetCaptureCallback(std::function<void(std::unique_ptr<DmaBuf>)> callback) {
	std::lock_guard lock(g_mutex);
	g_capture_callback = std::move(callback);
}

void SetViewCallback(std::function<void(uint32_t, uint32_t)> callback) {
	std::lock_guard lock(g_mutex);
	g_view_callback = std::move(callback);
}

void SetReadyCallback(std::function<void()> callback) {
	std::lock_guard lock(g_mutex);
	g_ready_callback = std::move(callback);
}

bool SurfacesReady() {
	return g_ready;
}

bool WantsPresents() {
	return g_visible || g_capture_requested || g_capture_reporting || g_presents_owed != 0;
}

#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX

// A BGRA8 image whose memory is a linear dma-buf, the shell's (imported) or Kyty's (exported)
struct SharedImage {
	vk::Image        image  = nullptr;
	vk::DeviceMemory memory = nullptr;
	vk::ImageView    view   = nullptr;
	vk::ImageLayout  layout = vk::ImageLayout::eUndefined;
	uint32_t         id     = 0; // The shell's id for it, or the capture's own
	uint32_t         width  = 0;
	uint32_t         height = 0;
	uint32_t         stride = 0; // Its one plane's layout
	uint64_t         offset = 0;

	static vk::ImageCreateInfo ImageInfo(const void* next, uint32_t width, uint32_t height,
	                                     vk::ImageUsageFlags usage) {
		vk::ImageCreateInfo info {};
		info.pNext         = next;
		info.imageType     = vk::ImageType::e2D;
		info.format        = FORMAT;
		info.extent        = vk::Extent3D {width, height, 1};
		info.mipLevels     = 1;
		info.arrayLayers   = 1;
		info.samples       = vk::SampleCountFlagBits::e1;
		info.tiling        = vk::ImageTiling::eDrmFormatModifierEXT;
		info.usage         = usage;
		info.sharingMode   = vk::SharingMode::eExclusive;
		info.initialLayout = vk::ImageLayout::eUndefined;
		return info;
	}

	// The first device-local memory type of `bits`, or else the first of them
	static uint32_t PickMemoryType(const GraphicContext& graphics, uint32_t bits) {
		const auto& props = graphics.physical_device_memory_properties;
		for (uint32_t i = 0; i < props.memoryTypeCount; i++) {
			if ((bits & (1u << i)) != 0 &&
			    (props.memoryTypes[i].propertyFlags & vk::MemoryPropertyFlagBits::eDeviceLocal)) {
				return i;
			}
		}
		return static_cast<uint32_t>(std::countr_zero(bits));
	}

	bool CreateView(GraphicContext& graphics) {
		vk::ImageViewCreateInfo info {};
		info.image            = image;
		info.viewType         = vk::ImageViewType::e2D;
		info.format           = FORMAT;
		info.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
		return graphics.device.createImageView(&info, nullptr, &view) == vk::Result::eSuccess;
	}

	// Imports the shell's dma-buf, `width` x `height`. Vulkan takes buf.fd if this works.
	bool Import(GraphicContext& graphics, DmaBuf& buf, uint32_t w, uint32_t h) {
		auto device = graphics.device;
		id          = buf.id;
		width       = w;
		height      = h;
		stride      = buf.stride;
		offset      = buf.offset;

		vk::SubresourceLayout plane {};
		plane.offset   = buf.offset;
		plane.rowPitch = buf.stride;

		vk::ImageDrmFormatModifierExplicitCreateInfoEXT modifier {};
		modifier.drmFormatModifier           = buf.modifier;
		modifier.drmFormatModifierPlaneCount = 1;
		modifier.pPlaneLayouts               = &plane;

		vk::ExternalMemoryImageCreateInfo external {};
		external.pNext       = &modifier;
		external.handleTypes = vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT;

		const auto info = ImageInfo(&external, width, height, vk::ImageUsageFlagBits::eSampled);
		if (const auto result = device.createImage(&info, nullptr, &image);
		    result != vk::Result::eSuccess) {
			LOGF("[Launcher] overlay: can't use dma-buf %u (stride %u, modifier 0x%llx): %s\n",
			     buf.id, buf.stride, static_cast<unsigned long long>(buf.modifier),
			     vk::to_string(result).c_str());
			image = nullptr;
			return false;
		}

		vk::MemoryRequirements req {};
		device.getImageMemoryRequirements(image, &req);

		vk::MemoryFdPropertiesKHR fd_props {};
		if (device.getMemoryFdPropertiesKHR(vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT,
		                                    buf.fd, &fd_props) != vk::Result::eSuccess) {
			LOGF("[Launcher] overlay: can't tell what memory dma-buf %u is\n", buf.id);
			return false;
		}
		const uint32_t types = req.memoryTypeBits & fd_props.memoryTypeBits;
		if (types == 0) {
			LOGF("[Launcher] overlay: dma-buf %u's memory can't back an image\n", buf.id);
			return false;
		}

		vk::ImportMemoryFdInfoKHR import {};
		import.handleType = vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT;
		import.fd         = buf.fd;

		vk::MemoryDedicatedAllocateInfo dedicated {};
		dedicated.pNext = &import;
		dedicated.image = image;

		vk::MemoryAllocateInfo alloc {};
		alloc.pNext           = &dedicated;
		alloc.allocationSize  = req.size;
		alloc.memoryTypeIndex = PickMemoryType(graphics, types);

		if (const auto result = device.allocateMemory(&alloc, nullptr, &memory);
		    result != vk::Result::eSuccess) {
			LOGF("[Launcher] overlay: importing dma-buf %u failed: %s\n", buf.id,
			     vk::to_string(result).c_str());
			memory = nullptr;
			return false;
		}
		// Vulkan owns the descriptor now
		buf.fd = -1;
		return device.bindImageMemory(image, memory, 0) == vk::Result::eSuccess &&
		       CreateView(graphics);
	}

	// A linear image whose memory can go to the shell as a dma-buf (ExportFd)
	bool CreateExportable(GraphicContext& graphics, uint32_t w, uint32_t h) {
		auto device = graphics.device;
		width       = w;
		height      = h;

		const auto usage = vk::ImageUsageFlagBits::eTransferDst;

		vk::PhysicalDeviceImageDrmFormatModifierInfoEXT modifier_info {};
		modifier_info.drmFormatModifier = DRM_FORMAT_MOD_LINEAR;
		modifier_info.sharingMode       = vk::SharingMode::eExclusive;

		vk::PhysicalDeviceExternalImageFormatInfo external_info {};
		external_info.pNext      = &modifier_info;
		external_info.handleType = vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT;

		vk::PhysicalDeviceImageFormatInfo2 format_info {};
		format_info.pNext  = &external_info;
		format_info.format = FORMAT;
		format_info.type   = vk::ImageType::e2D;
		format_info.tiling = vk::ImageTiling::eDrmFormatModifierEXT;
		format_info.usage  = usage;

		vk::ExternalImageFormatProperties external_props {};
		vk::ImageFormatProperties2        props {};
		props.pNext = &external_props;

		const auto result =
		    graphics.physical_device.getImageFormatProperties2(&format_info, &props);
		const auto limit = props.imageFormatProperties.maxExtent;
		if (result != vk::Result::eSuccess ||
		    !(external_props.externalMemoryProperties.externalMemoryFeatures &
		      vk::ExternalMemoryFeatureFlagBits::eExportable) ||
		    limit.width < w || limit.height < h) {
			LOGF("[Launcher] overlay: this GPU can't export a linear %ux%u BGRA8 dma-buf (%s)\n", w,
			     h, vk::to_string(result).c_str());
			return false;
		}

		const uint64_t                              modifiers[] = {DRM_FORMAT_MOD_LINEAR};
		vk::ImageDrmFormatModifierListCreateInfoEXT modifier_list {};
		modifier_list.drmFormatModifierCount = 1;
		modifier_list.pDrmFormatModifiers    = modifiers;

		vk::ExternalMemoryImageCreateInfo external {};
		external.pNext       = &modifier_list;
		external.handleTypes = vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT;

		const auto info = ImageInfo(&external, width, height, usage);
		if (device.createImage(&info, nullptr, &image) != vk::Result::eSuccess) {
			LOGF("[Launcher] overlay: creating a linear %ux%u dma-buf failed\n", w, h);
			image = nullptr;
			return false;
		}

		vk::MemoryRequirements req {};
		device.getImageMemoryRequirements(image, &req);

		vk::ExportMemoryAllocateInfo export_info {};
		export_info.handleTypes = vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT;

		vk::MemoryDedicatedAllocateInfo dedicated {};
		dedicated.pNext = &export_info;
		dedicated.image = image;

		vk::MemoryAllocateInfo alloc {};
		alloc.pNext           = &dedicated;
		alloc.allocationSize  = req.size;
		alloc.memoryTypeIndex = PickMemoryType(graphics, req.memoryTypeBits);

		if (device.allocateMemory(&alloc, nullptr, &memory) != vk::Result::eSuccess) {
			LOGF("[Launcher] overlay: allocating a dma-buf failed\n");
			memory = nullptr;
			return false;
		}
		if (device.bindImageMemory(image, memory, 0) != vk::Result::eSuccess) {
			return false;
		}

		const vk::ImageSubresource plane {vk::ImageAspectFlagBits::eMemoryPlane0EXT, 0, 0};
		vk::SubresourceLayout      layout_info {};
		device.getImageSubresourceLayout(image, &plane, &layout_info);
		stride = static_cast<uint32_t>(layout_info.rowPitch);
		offset = layout_info.offset;

		// Any id but 0, to tell captures apart
		id = std::max<uint32_t>(std::random_device {}(), 1);
		return true;
	}

	// A new descriptor for the image's memory, or -1
	[[nodiscard]] int ExportFd(GraphicContext& graphics) const {
		vk::MemoryGetFdInfoKHR info {};
		info.memory     = memory;
		info.handleType = vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT;
		int fd          = -1;
		return memory != nullptr &&
		               graphics.device.getMemoryFdKHR(&info, &fd) == vk::Result::eSuccess
		           ? fd
		           : -1;
	}

	void Destroy(GraphicContext& graphics) {
		graphics.device.destroyImageView(view, nullptr);
		graphics.device.destroyImage(image, nullptr);
		graphics.device.freeMemory(memory, nullptr);
		*this = {};
	}
};

// The header the shell and Kyty trade buffers through: the shell's sealed memfd, mapped
struct Header {
	static constexpr uint32_t MAGIC    = 0x4C4F5632; // 'LOV2'
	static constexpr uint32_t NEWER    = 1u << 31;
	static constexpr size_t   MAP_SIZE = 16;

	void*    map          = nullptr;
	uint32_t id           = 0;
	uint32_t mine         = 2;     // The buffer Kyty owns; it starts with buffer 2, empty
	bool     has_content  = false; // Kyty has had a buffer from the shell
	uint64_t sampled_tick = 0;     // The present that last read `mine`, 0 if none

	bool Map(DmaBuf& buf) {
		struct stat st {};
		id = buf.id;
		if (buf.fd < 0 || ::fstat(buf.fd, &st) != 0 || static_cast<size_t>(st.st_size) < MAP_SIZE) {
			return false;
		}
		void* addr = ::mmap(nullptr, MAP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, buf.fd, 0);
		if (addr == MAP_FAILED) {
			return false;
		}
		map = addr;
		return true;
	}

	// The buffer to draw this present, taking a newer one if the shell published one and the GPU
	// is done with Kyty's (`fresh` says so). None until the shell has published anything; false
	// if the shell broke the exchange.
	bool Take(CommandScheduler& scheduler, uint32_t buffer_count, std::optional<uint32_t>& out,
	          bool& fresh) {
		fresh = false;
		if (map != nullptr) {
			auto* words = static_cast<std::atomic<uint32_t>*>(map);
			if (words[0].load(std::memory_order_acquire) == MAGIC &&
			    (words[1].load(std::memory_order_acquire) & NEWER) != 0 &&
			    (sampled_tick == 0 || scheduler.IsFree(sampled_tick))) {
				// Give Kyty's back (bit clear) and take the newer one
				const uint32_t got = words[1].exchange(mine, std::memory_order_acq_rel) & ~NEWER;
				if (got >= buffer_count) {
					// Kyty's buffer is back in the word, so it's no longer Kyty's to draw
					LOGF("[Launcher] overlay: the launcher published buffer %u of %u\n", got,
					     buffer_count);
					return false;
				}
				mine         = got;
				has_content  = true;
				sampled_tick = 0;
				fresh        = true;
			}
		}
		out = has_content ? std::optional(mine) : std::nullopt;
		return true;
	}

	void Destroy() {
		if (map != nullptr) {
			::munmap(map, MAP_SIZE);
		}
		*this = {};
	}
};

static_assert(sizeof(std::atomic<uint32_t>) == sizeof(uint32_t));

// Whether linear BGRA8 dma-bufs can be imported for sampling and exported as copy targets
static bool ProbeSharing(const GraphicContext& graphics) {
	if (!graphics.dma_buf_sharing_enabled) {
		return false;
	}
	const auto can = [&](vk::ImageUsageFlags usage, vk::ExternalMemoryFeatureFlagBits feature) {
		vk::PhysicalDeviceImageDrmFormatModifierInfoEXT modifier {};
		modifier.drmFormatModifier = DRM_FORMAT_MOD_LINEAR;
		modifier.sharingMode       = vk::SharingMode::eExclusive;
		vk::PhysicalDeviceExternalImageFormatInfo external {};
		external.pNext      = &modifier;
		external.handleType = vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT;
		vk::PhysicalDeviceImageFormatInfo2 info {};
		info.pNext  = &external;
		info.format = FORMAT;
		info.type   = vk::ImageType::e2D;
		info.tiling = vk::ImageTiling::eDrmFormatModifierEXT;
		info.usage  = usage;
		vk::ExternalImageFormatProperties external_props {};
		vk::ImageFormatProperties2        props {};
		props.pNext = &external_props;
		return graphics.physical_device.getImageFormatProperties2(&info, &props) ==
		           vk::Result::eSuccess &&
		       (external_props.externalMemoryProperties.externalMemoryFeatures & feature);
	};
	const bool import =
	    can(vk::ImageUsageFlagBits::eSampled, vk::ExternalMemoryFeatureFlagBits::eImportable);
	const bool export_ =
	    can(vk::ImageUsageFlagBits::eTransferDst, vk::ExternalMemoryFeatureFlagBits::eExportable);
	LOGF("[Launcher] overlay: linear BGRA8 dma-buf import %s, export %s\n", import ? "yes" : "no",
	     export_ ? "yes" : "no");
	return import && export_;
}

struct Renderer::Impl {
	explicit Impl(GraphicContext& g): graphics(g), supported(ProbeSharing(g)) {}

	// Surfaces taken away while the GPU may still read them, freed once it's done
	struct Retired {
		std::vector<SharedImage> buffers;
		Header                   header;
		uint64_t                 tick = 0;
	};

	GraphicContext&          graphics;
	CommandScheduler*        scheduler = nullptr;
	std::vector<SharedImage> buffers;
	Header                   header;
	uint64_t                 last_sampled_tick = 0; // Any buffer's last use on the GPU
	std::vector<Retired>     retired;
	bool                     sampled_now = false; // This present draws a buffer

	SharedImage capture;
	VulkanImage staging; // The frame converted to BGRA8, then copied to the linear capture
	uint64_t    capture_tick = 0;
	bool        captured_now = false;

	vk::Extent2D view {};

	const bool supported; // ProbeSharing

	[[nodiscard]] bool Supported() const { return supported; }

	void ReleaseOverlay() {
		g_ready = false;
		if (!buffers.empty() || header.map != nullptr) {
			retired.push_back({std::move(buffers), std::exchange(header, {}), last_sampled_tick});
		}
		buffers.clear();
		last_sampled_tick = 0;
	}

	void FreeRetired(bool all) {
		std::erase_if(retired, [&](Retired& r) {
			if (!all && r.tick != 0 && !scheduler->IsFree(r.tick)) {
				return false;
			}
			for (auto& buffer: r.buffers) {
				buffer.Destroy(graphics);
			}
			r.header.Destroy();
			return true;
		});
	}

	void ApplyPending() {
		std::optional<Config> config;
		{
			std::lock_guard lock(g_mutex);
			config = std::exchange(g_pending, std::nullopt);
		}
		if (!config) {
			return;
		}

		ReleaseOverlay();

		if (config->buffers.empty()) {
			LOGF("[Launcher] overlay: removed\n");
			return;
		}
		if (!Supported()) {
			LOGF("[Launcher] overlay: this GPU can't share images with the launcher\n");
			return;
		}

		for (auto& buf: config->buffers) {
			if (buf == nullptr || buf->fd < 0) {
				LOGF("[Launcher] overlay: a dma-buf is missing\n");
				ReleaseOverlay();
				return;
			}
			if (!buffers.emplace_back().Import(graphics, *buf, config->width, config->height)) {
				ReleaseOverlay();
				return;
			}
		}
		if (config->header != nullptr && !header.Map(*config->header)) {
			LOGF("[Launcher] overlay: header %u is missing or too small\n", header.id);
			ReleaseOverlay();
			return;
		}

		LOGF("[Launcher] overlay: %zu buffer(s) of %ux%u, header %u\n", buffers.size(),
		     config->width, config->height, header.id);

		// Only now may the capture go to the shell: it shows the menu once it has it.
		std::function<void()> callback;
		{
			std::lock_guard lock(g_mutex);
			// A newer set came meanwhile: that one has to be imported first
			if (g_pending) {
				return;
			}
			g_ready  = true;
			callback = g_ready_callback;
		}
		if (callback) {
			callback();
		}
	}

	void ReportCapture() {
		if (!g_capture_reporting || capture_tick == 0 || !scheduler->IsFree(capture_tick)) {
			return;
		}
		g_capture_reporting = false;
		capture_tick        = 0;

		std::function<void(std::unique_ptr<DmaBuf>)> callback;
		{
			std::lock_guard lock(g_mutex);
			callback = g_capture_callback;
		}
		if (!callback) {
			return;
		}
		auto buf      = std::make_unique<DmaBuf>();
		buf->fd       = capture.ExportFd(graphics);
		buf->id       = capture.id;
		buf->width    = capture.width;
		buf->height   = capture.height;
		buf->stride   = capture.stride;
		buf->offset   = capture.offset;
		buf->modifier = DRM_FORMAT_MOD_LINEAR;
		if (buf->fd < 0) {
			LOGF("[Launcher] overlay: couldn't export the capture's dma-buf\n");
			return;
		}
		callback(std::move(buf));
	}

	bool PrepareCapture(uint32_t width, uint32_t height) {
		if (capture.image != nullptr && capture.width == width && capture.height == height) {
			return true;
		}
		capture.Destroy(graphics);
		if (staging.image != nullptr) {
			graphics.DeleteImage(staging);
		}
		if (!capture.CreateExportable(graphics, width, height)) {
			capture.Destroy(graphics);
			return false;
		}
		vk::ImageCreateInfo info {};
		info.imageType   = vk::ImageType::e2D;
		info.format      = FORMAT;
		info.extent      = vk::Extent3D {width, height, 1};
		info.mipLevels   = 1;
		info.arrayLayers = 1;
		info.samples     = vk::SampleCountFlagBits::e1;
		info.tiling      = vk::ImageTiling::eOptimal;
		info.usage = vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst;
		info.sharingMode   = vk::SharingMode::eExclusive;
		info.initialLayout = vk::ImageLayout::eUndefined;
		if (!graphics.CreateImage(info, staging)) {
			LOGF("[Launcher] overlay: couldn't create the capture's staging image\n");
			capture.Destroy(graphics);
			return false;
		}
		return true;
	}

	void Destroy() {
		ReleaseOverlay();
		if (scheduler != nullptr) {
			FreeRetired(true);
		}
		capture.Destroy(graphics);
		if (staging.image != nullptr) {
			graphics.DeleteImage(staging);
		}
	}
};

Renderer::Renderer(GraphicContext& graphics): m_impl(std::make_unique<Impl>(graphics)) {}

// The swapchain waits for the queue to idle before this
Renderer::~Renderer() {
	m_impl->Destroy();
	// Imported again by the next renderer
	std::lock_guard lock(g_mutex);
	g_pending.reset();
}

void Renderer::BeginPresent(CommandScheduler& scheduler, vk::Extent2D window) {
	auto& impl     = *m_impl;
	impl.scheduler = &scheduler;
	impl.ApplyPending();
	impl.FreeRetired(false);
	impl.ReportCapture();

	if (uint32_t owed = g_presents_owed.load(); owed != 0) {
		g_presents_owed.compare_exchange_strong(owed, owed - 1);
	}

	// Only a renderer that can draw the overlay says how big it should be
	if (impl.Supported() && window != impl.view && window.width != 0 && window.height != 0) {
		impl.view = window;
		std::function<void(uint32_t, uint32_t)> callback;
		{
			std::lock_guard lock(g_mutex);
			callback = g_view_callback;
		}
		if (callback) {
			callback(window.width, window.height);
		}
	}
}

void Renderer::Capture(vk::CommandBuffer command, vk::Image source, vk::Extent2D extent) {
	auto& impl = *m_impl;
	if (!g_capture_requested || source == nullptr || extent.width == 0 || extent.height == 0) {
		return;
	}
	// Reporting before the request clears, so WantsPresents() stays true throughout
	g_capture_reporting = true;
	g_capture_requested = false;

	if (!impl.Supported() || !impl.PrepareCapture(extent.width, extent.height)) {
		LOGF("[Launcher] overlay: can't capture the game's frame for the launcher\n");
		g_capture_reporting = false;
		return;
	}

	// The frame, converted to BGRA8 by a blit, then copied to the linear image: a blit into a
	// linear image isn't supported everywhere, a copy is.
	const auto staging = impl.staging.image;
	Barrier(command, staging, vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal, {},
	        vk::AccessFlagBits::eTransferWrite, vk::PipelineStageFlagBits::eTransfer,
	        vk::PipelineStageFlagBits::eTransfer);
	vk::ImageBlit blit {};
	blit.srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
	blit.dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
	blit.srcOffsets[1] =
	    vk::Offset3D {static_cast<int32_t>(extent.width), static_cast<int32_t>(extent.height), 1};
	blit.dstOffsets[1] = blit.srcOffsets[1];
	command.blitImage(source, vk::ImageLayout::eTransferSrcOptimal, staging,
	                  vk::ImageLayout::eTransferDstOptimal, 1, &blit, vk::Filter::eNearest);
	Barrier(command, staging, vk::ImageLayout::eTransferDstOptimal,
	        vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits::eTransferWrite,
	        vk::AccessFlagBits::eTransferRead, vk::PipelineStageFlagBits::eTransfer,
	        vk::PipelineStageFlagBits::eTransfer);

	// Linear, so taking it from UNDEFINED loses nothing it needs
	Barrier(command, impl.capture.image, vk::ImageLayout::eUndefined,
	        vk::ImageLayout::eTransferDstOptimal, {}, vk::AccessFlagBits::eTransferWrite,
	        vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eTransfer);
	vk::ImageCopy copy {};
	copy.srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
	copy.dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
	copy.extent         = vk::Extent3D {extent.width, extent.height, 1};
	command.copyImage(staging, vk::ImageLayout::eTransferSrcOptimal, impl.capture.image,
	                  vk::ImageLayout::eTransferDstOptimal, 1, &copy);
	// Released to the shell's process
	Barrier(command, impl.capture.image, vk::ImageLayout::eTransferDstOptimal,
	        vk::ImageLayout::eGeneral, vk::AccessFlagBits::eTransferWrite, {},
	        vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eBottomOfPipe,
	        impl.graphics.queue_family, VK_QUEUE_FAMILY_EXTERNAL);

	impl.captured_now = true;
	LOGF("[Launcher] overlay: capturing the game's frame (%ux%u) into surface %u\n", extent.width,
	     extent.height, impl.capture.id);
}

vk::ImageView Renderer::Acquire(vk::CommandBuffer command) {
	auto& impl = *m_impl;
	if (!g_visible || impl.buffers.empty()) {
		return nullptr;
	}
	// With a header, the buffer the exchange gave Kyty (nothing until one comes); without, the
	// only one
	const auto              count = static_cast<uint32_t>(impl.buffers.size());
	std::optional<uint32_t> index;
	// Without a header the shell may redraw the one buffer at any time
	bool fresh = true;
	if (impl.header.map == nullptr) {
		index = 0;
	} else if (!impl.header.Take(*impl.scheduler, count, index, fresh)) {
		impl.ReleaseOverlay();
		return nullptr;
	}
	if (!index) {
		return nullptr;
	}
	auto& buffer = impl.buffers[*index];
	// The shell wrote it from its own process: acquire it from there, which makes its writes
	// visible. It's linear, so leaving UNDEFINED loses nothing.
	if (fresh || buffer.layout != vk::ImageLayout::eShaderReadOnlyOptimal) {
		Barrier(command, buffer.image, vk::ImageLayout::eUndefined,
		        vk::ImageLayout::eShaderReadOnlyOptimal, {}, vk::AccessFlagBits::eShaderRead,
		        vk::PipelineStageFlagBits::eTopOfPipe, vk::PipelineStageFlagBits::eFragmentShader,
		        VK_QUEUE_FAMILY_EXTERNAL, impl.graphics.queue_family);
		buffer.layout = vk::ImageLayout::eShaderReadOnlyOptimal;
	}
	impl.sampled_now = true;
	return buffer.view;
}

void Renderer::EndPresent(uint64_t tick) {
	auto& impl = *m_impl;
	if (std::exchange(impl.sampled_now, false)) {
		impl.header.sampled_tick = tick;
		impl.last_sampled_tick   = tick;
	}
	if (std::exchange(impl.captured_now, false)) {
		impl.capture_tick = tick;
	}
}

#else

struct Renderer::Impl {};

Renderer::Renderer(GraphicContext& /*graphics*/): m_impl(std::make_unique<Impl>()) {}
Renderer::~Renderer() = default;
void Renderer::BeginPresent(CommandScheduler& /*scheduler*/, vk::Extent2D /*window*/) {
	if (uint32_t owed = g_presents_owed.load(); owed != 0) {
		g_presents_owed.compare_exchange_strong(owed, owed - 1);
	}
	// Nothing to draw it with: let a request lapse, so the launcher falls back to its window
	g_capture_requested = false;
}
void          Renderer::Capture(vk::CommandBuffer /*command*/, vk::Image /*source*/,
                                vk::Extent2D /*extent*/) {}
vk::ImageView Renderer::Acquire(vk::CommandBuffer /*command*/) {
	return nullptr;
}
void Renderer::EndPresent(uint64_t /*tick*/) {}

#endif

} // namespace Libs::Graphics::LauncherOverlay
