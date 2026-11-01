#include "common/launcher/overlayPorts.h"

#include "common/common.h"

#include <mutex>

#if KYTY_DMA_BUF_OVERLAY
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <iterator>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace Common::Launcher::OverlayPorts {

#if KYTY_DMA_BUF_OVERLAY

static std::mutex g_mutex;

DmaBuf::~DmaBuf() {
	if (fd >= 0) {
		::close(fd);
	}
}

// Messages begin with four ASCII bytes; the rest is little-endian.
static constexpr char MSG_GET[4]      = {'L', 'O', 'V', 'g'}; // Send the surfaces
static constexpr char MSG_SURFACES[4] = {'L', 'O', 'V', 's'}; // u32 count, a layout each, fds
static constexpr char MSG_CAPTURE[4]  = {'L', 'O', 'V', 'c'}; // The capture's layout and fd

static constexpr int    TIMEOUT_MS  = 500;
static constexpr size_t MAX_FDS     = 4;
static constexpr size_t LAYOUT_SIZE = 24; // {id, stride: u32, offset, modifier: u64}

static std::string g_name;
static int         g_socket = -1;

template <typename T>
static void Put(std::vector<uint8_t>& out, T value) {
	const size_t at = out.size();
	out.resize(at + sizeof(T));
	std::memcpy(out.data() + at, &value, sizeof(T));
}

template <typename T>
static T Get(const uint8_t* at) {
	T value;
	std::memcpy(&value, at, sizeof(T));
	return value;
}

static bool SendMsg(const std::vector<uint8_t>& data, int fd) {
	iovec                 iov {const_cast<uint8_t*>(data.data()), data.size()};
	alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))] {};

	msghdr msg {};
	msg.msg_iov    = &iov;
	msg.msg_iovlen = 1;

	if (fd >= 0) {
		msg.msg_control    = control;
		msg.msg_controllen = sizeof(control);
		cmsghdr* c         = CMSG_FIRSTHDR(&msg);
		c->cmsg_level      = SOL_SOCKET;
		c->cmsg_type       = SCM_RIGHTS;
		c->cmsg_len        = CMSG_LEN(sizeof(int));
		std::memcpy(CMSG_DATA(c), &fd, sizeof(int));
	}

	return ::sendmsg(g_socket, &msg, MSG_NOSIGNAL) == static_cast<ssize_t>(data.size());
}

bool Supported() {
	return true;
}

bool SetService(const std::string& service, std::string& error) {
	std::lock_guard lock(g_mutex);

	if (service == g_name && (g_socket >= 0 || service.empty())) {
		return true;
	}
	if (g_socket >= 0) {
		::close(g_socket);
		g_socket = -1;
	}
	g_name.clear();
	if (service.empty()) {
		return true;
	}

	// An abstract socket: its name follows a NUL
	sockaddr_un addr {};
	addr.sun_family = AF_UNIX;
	if (service.size() + 1 > sizeof(addr.sun_path)) {
		error = "the socket's name is too long";
		return false;
	}
	std::memcpy(addr.sun_path + 1, service.data(), service.size());
	const auto len = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + 1 + service.size());

	g_socket = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
	if (g_socket < 0 || ::connect(g_socket, reinterpret_cast<const sockaddr*>(&addr), len) != 0) {
		error = "couldn't connect to " + service + ": " + std::strerror(errno);
		if (g_socket >= 0) {
			::close(g_socket);
			g_socket = -1;
		}
		return false;
	}

	g_name = service;
	return true;
}

bool HasService() {
	std::lock_guard lock(g_mutex);
	return g_socket >= 0;
}

std::optional<Surfaces> Fetch(const std::vector<uint32_t>& buffer_ids, uint32_t header_id,
                              std::string& error) {
	std::lock_guard lock(g_mutex);

	const size_t count = buffer_ids.size() + (header_id != 0 ? 1 : 0);
	if (g_socket < 0 || count == 0 || count > MAX_FDS) {
		error = g_socket < 0 ? "no service" : "wrong number of surfaces";
		return std::nullopt;
	}

	if (!SendMsg(std::vector<uint8_t>(std::begin(MSG_GET), std::end(MSG_GET)), -1)) {
		error = std::string("couldn't ask the launcher: ") + std::strerror(errno);
		return std::nullopt;
	}

	pollfd p {g_socket, POLLIN, 0};
	if (::poll(&p, 1, TIMEOUT_MS) != 1) {
		error = "no answer from the launcher";
		return std::nullopt;
	}

	uint8_t               data[8 + MAX_FDS * LAYOUT_SIZE] {};
	iovec                 iov {data, sizeof(data)};
	alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int) * MAX_FDS)] {};

	msghdr msg {};
	msg.msg_iov        = &iov;
	msg.msg_iovlen     = 1;
	msg.msg_control    = control;
	msg.msg_controllen = sizeof(control);

	const ssize_t n = ::recvmsg(g_socket, &msg, MSG_CMSG_CLOEXEC);

	// Whatever came is owned here, and closed unless it's what was asked for
	std::vector<std::unique_ptr<DmaBuf>> got;
	for (cmsghdr* c = n >= 0 ? CMSG_FIRSTHDR(&msg) : nullptr; c != nullptr;
	     c          = CMSG_NXTHDR(&msg, c)) {
		if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) {
			continue;
		}
		const size_t fds = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
		for (size_t i = 0; i < fds; i++) {
			auto buf = std::make_unique<DmaBuf>();
			std::memcpy(&buf->fd, CMSG_DATA(c) + i * sizeof(int), sizeof(int));
			got.push_back(std::move(buf));
		}
	}

	if (n < 8 || std::memcmp(data, MSG_SURFACES, 4) != 0) {
		error = n <= 0 ? "the launcher hung up" : "the launcher's answer doesn't match";
		return std::nullopt;
	}

	const auto listed = Get<uint32_t>(data + 4);
	if (listed == 0 || got.empty()) {
		error = "the launcher refused the surfaces";
		return std::nullopt;
	}
	if (listed != count || got.size() != count ||
	    static_cast<size_t>(n) < 8 + count * LAYOUT_SIZE) {
		error = "the launcher's answer doesn't match";
		return std::nullopt;
	}

	for (size_t i = 0; i < count; i++) {
		const uint8_t* at  = data + 8 + i * LAYOUT_SIZE;
		DmaBuf&        buf = *got[i];
		buf.id             = Get<uint32_t>(at);
		buf.stride         = Get<uint32_t>(at + 4);
		buf.offset         = Get<uint64_t>(at + 8);
		buf.modifier       = Get<uint64_t>(at + 16);

		const uint32_t want =
		    header_id != 0 ? (i == 0 ? header_id : buffer_ids[i - 1]) : buffer_ids[i];
		if (buf.id != want) {
			error = "the launcher sent other surfaces than it named";
			return std::nullopt;
		}
	}

	Surfaces out;
	size_t   next = 0;
	if (header_id != 0) {
		out.header = std::move(got[next++]);
	}
	for (; next < got.size(); next++) {
		out.buffers.push_back(std::move(got[next]));
	}
	return out;
}

bool SendCapture(const DmaBuf& capture, std::string& error) {
	std::lock_guard lock(g_mutex);

	if (g_socket < 0 || capture.fd < 0) {
		error = g_socket < 0 ? "no service" : "no capture";
		return false;
	}

	std::vector<uint8_t> data(std::begin(MSG_CAPTURE), std::end(MSG_CAPTURE));
	Put(data, capture.id);
	Put(data, capture.width);
	Put(data, capture.height);
	Put(data, capture.stride);
	Put(data, capture.offset);
	Put(data, capture.modifier);

	if (!SendMsg(data, capture.fd)) {
		error = std::string("couldn't send the capture: ") + std::strerror(errno);
		return false;
	}
	return true;
}

#else

DmaBuf::~DmaBuf() = default;

bool Supported() {
	return false;
}

bool SetService(const std::string& service, std::string& error) {
	error = "not supported on this platform";
	return service.empty();
}

bool HasService() {
	return false;
}

std::optional<Surfaces> Fetch(const std::vector<uint32_t>& /*buffer_ids*/, uint32_t /*header_id*/,
                              std::string& error) {
	error = "not supported on this platform";
	return std::nullopt;
}

bool SendCapture(const DmaBuf& /*capture*/, std::string& error) {
	error = "not supported on this platform";
	return false;
}

#endif

} // namespace Common::Launcher::OverlayPorts
