#include "common/launcher/session.h"

#include "common/logging/log.h"
#include "kytyGitVersion.h"

#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <thread>

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#include <windows.h>
#else
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace Common::Launcher {

static constexpr const char* ENV_ENDPOINT = "LAUNCHER_ENDPOINT";
static constexpr const char* ENV_SESSION  = "LAUNCHER_SESSION";

// The plugin API version Kyty speaks. It uses unlocked.at and messages newer than API 1, and the
// on-screen keyboard (osk) from API 4.
static constexpr int PLUGIN_API = 4;

// Minimum time between two updates of one activity.
static constexpr auto ACTIVITY_INTERVAL = std::chrono::milliseconds(100);

// Frames bigger than this are a protocol error, not a message.
static constexpr uint32_t MAX_FRAME = 16 * 1024 * 1024;

static std::atomic<Session*> g_session {nullptr};

static std::string GetEnv(const char* name) {
	const char* value = std::getenv(name);
	return value != nullptr ? std::string(value) : std::string {};
}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
static std::wstring Widen(const std::string& s) {
	if (s.empty()) {
		return {};
	}
	const int    size = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
	                                          nullptr, 0);
	std::wstring out(static_cast<size_t>(size), L'\0');
	::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), size);
	return out;
}

// One read or write on the pipe. It's opened for overlapped I/O because synchronous I/O on a
// handle is serialised: the reader's pending ReadFile would hold up every write.
static bool PipeIo(void* pipe, bool write, void* data, DWORD len, DWORD& done) {
	// One event per thread (the handshake's, the reader's and the writer's), kept until the
	// process exits like the session. ReadFile and WriteFile reset it as they start.
	static thread_local const HANDLE event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
	if (event == nullptr) {
		return false;
	}
	OVERLAPPED ov {};
	ov.hEvent         = event;
	const BOOL started = write ? ::WriteFile(pipe, data, len, nullptr, &ov)
	                           : ::ReadFile(pipe, data, len, nullptr, &ov);
	return (started != 0 || ::GetLastError() == ERROR_IO_PENDING) &&
	       ::GetOverlappedResult(pipe, &ov, &done, TRUE) != 0 && done > 0;
}

// The user a process runs as, in `buf`.
static PSID UserOf(HANDLE process, std::vector<uint8_t>& buf) {
	HANDLE token {};
	if (::OpenProcessToken(process, TOKEN_QUERY, &token) == 0) {
		return nullptr;
	}
	DWORD size = 0;
	::GetTokenInformation(token, TokenUser, nullptr, 0, &size);
	buf.resize(size);
	const bool ok =
	    size != 0 && ::GetTokenInformation(token, TokenUser, buf.data(), size, &size) != 0;
	::CloseHandle(token);
	return ok ? reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid : nullptr;
}

// Whether the pipe's server runs as we do. Anyone can make a pipe by that name if the launcher
// hasn't, and the session token isn't theirs to see; the launcher checks its clients the same way.
static bool ServerIsUs(HANDLE pipe) {
	ULONG pid = 0;
	if (::GetNamedPipeServerProcessId(pipe, &pid) == 0) {
		return false;
	}
	const HANDLE server = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
	if (server == nullptr) {
		return false;
	}
	std::vector<uint8_t> ours;
	std::vector<uint8_t> theirs;
	const PSID           us   = UserOf(::GetCurrentProcess(), ours);
	const PSID           them = UserOf(server, theirs);
	::CloseHandle(server);
	return us != nullptr && them != nullptr && ::EqualSid(us, them) != 0;
}
#endif

bool EnvPresent() {
	return !GetEnv(ENV_ENDPOINT).empty() && !GetEnv(ENV_SESSION).empty();
}

uint32_t ProcessId() {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	return static_cast<uint32_t>(::GetCurrentProcessId());
#else
	return static_cast<uint32_t>(::getpid());
#endif
}

Session* Session::Get() {
	return g_session.load();
}

Session* Session::Connect(Handlers handlers, const std::vector<std::string>& capabilities) {
	if (!EnvPresent()) {
		return nullptr;
	}

	// Lives until the process exits: the reader thread and late callbacks may still use it.
	auto* session       = new Session();
	session->m_handlers = std::move(handlers);

	if (!session->Handshake(capabilities)) {
		delete session;
		return nullptr;
	}

	g_session = session;
	std::thread([session]() { session->Reader(); }).detach();
	std::thread([session]() { session->Writer(); }).detach();
	return session;
}

Session::~Session() {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	if (m_pipe != nullptr) {
		::CloseHandle(m_pipe);
	}
#else
	if (m_fd >= 0) {
		::close(m_fd);
	}
#endif
}

bool Session::OpenEndpoint(const std::string& endpoint) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	// LAUNCHER_ENDPOINT is the pipe's name, \\.\pipe\launcherd-<user>. SECURITY_IDENTIFICATION
	// keeps the launcher from acting as us, which it has no need to.
	const std::wstring name = Widen(endpoint);

	for (int attempt = 0;; attempt++) {
		const HANDLE pipe = ::CreateFileW(
		    name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
		    FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);

		if (pipe != INVALID_HANDLE_VALUE) {
			if (!ServerIsUs(pipe)) {
				LOGF("[Launcher] not connecting to %s: its server doesn't run as this user, or that "
				     "couldn't be checked\n",
				     endpoint.c_str());
				::CloseHandle(pipe);
				return false;
			}
			m_pipe = pipe;
			return true;
		}

		// Every instance is taken by another client until the launcher makes the next one.
		const DWORD error = ::GetLastError();
		if (error != ERROR_PIPE_BUSY || attempt == 2 || ::WaitNamedPipeW(name.c_str(), 1000) == 0) {
			LOGF("[Launcher] could not connect to %s: error %lu\n", endpoint.c_str(), error);
			return false;
		}
	}
#else
	sockaddr_un addr {};
	addr.sun_family = AF_UNIX;

	if (endpoint.size() >= sizeof(addr.sun_path)) {
		LOGF("[Launcher] endpoint path is too long: %s\n", endpoint.c_str());
		return false;
	}

	std::memcpy(addr.sun_path, endpoint.c_str(), endpoint.size() + 1);

	m_fd = ::socket(AF_UNIX, SOCK_STREAM, 0);

	if (m_fd < 0 || ::connect(m_fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
		LOGF("[Launcher] could not connect to %s: %s\n", endpoint.c_str(), std::strerror(errno));
		return false;
	}

#ifdef SO_NOSIGPIPE
	const int one = 1;
	::setsockopt(m_fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
	return true;
#endif
}

bool Session::WaitReadable(int timeout_ms) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	// A pipe can't be waited on for data without starting a read, so look every 10 ms.
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
	while (std::chrono::steady_clock::now() < deadline) {
		DWORD available = 0;
		if (::PeekNamedPipe(m_pipe, nullptr, 0, nullptr, &available, nullptr) == 0) {
			// Closed: the read that follows says so.
			return true;
		}
		if (available != 0) {
			return true;
		}
		::Sleep(10);
	}
	return false;
#else
	pollfd pfd {m_fd, POLLIN, 0};
	return ::poll(&pfd, 1, timeout_ms) > 0;
#endif
}

bool Session::Handshake(const std::vector<std::string>& capabilities) {
	const std::string endpoint = GetEnv(ENV_ENDPOINT);

	if (!OpenEndpoint(endpoint)) {
		return false;
	}

	const std::string name  = std::string("Kyty ") + KYTY_BUILD_LABEL;
	const Message     hello = Hello(GetEnv(ENV_SESSION), PLUGIN_API, capabilities, name);

	if (!WriteFrame(hello.json)) {
		LOGF("[Launcher] could not send the hello\n");
		return false;
	}

	m_connected_at = std::chrono::steady_clock::now();

	// The launcher answers straight away. Don't hold up the boot if it doesn't.
	if (!WaitReadable(3000)) {
		LOGF("[Launcher] no welcome\n");
		return false;
	}

	std::string frame;
	if (!ReadFrame(frame)) {
		LOGF("[Launcher] the launcher closed the connection during the handshake\n");
		return false;
	}

	const HostMessage welcome = ParseHost(frame);

	// Older launchers spell it "Json"
	std::string encoding = welcome.encoding;
	for (auto& c: encoding) {
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	if (encoding != "json") {
		LOGF("[Launcher] the launcher chose an encoding Kyty doesn't speak: %s\n", frame.c_str());
		return false;
	}

	LOGF("[Launcher] connected to %s at %s\n", welcome.server.c_str(), endpoint.c_str());
	return true;
}

bool Session::WriteFrame(const std::string& payload) {
	const auto    size      = static_cast<uint32_t>(payload.size());
	const uint8_t header[4] = {static_cast<uint8_t>(size), static_cast<uint8_t>(size >> 8u),
	                           static_cast<uint8_t>(size >> 16u), static_cast<uint8_t>(size >> 24u)};

	// Only the handshake (before the writer starts) and the writer write. One buffer, so one
	// write call per frame.
	std::string frame(reinterpret_cast<const char*>(header), sizeof(header));
	frame += payload;

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	if (m_pipe == nullptr) {
		return false;
	}
	for (size_t written = 0; written < frame.size();) {
		DWORD n = 0;
		if (!PipeIo(m_pipe, true, frame.data() + written, static_cast<DWORD>(frame.size() - written),
		            n)) {
			return false;
		}
		written += n;
	}
	return true;
#else
	if (m_fd < 0) {
		return false;
	}
#ifdef MSG_NOSIGNAL
	constexpr int flags = MSG_NOSIGNAL;
#else
	constexpr int flags = 0;
#endif
	const char* ptr = frame.data();
	size_t      len = frame.size();
	while (len != 0) {
		const ssize_t n = ::send(m_fd, ptr, len, flags);
		if (n < 0 && errno == EINTR) {
			continue;
		}
		if (n <= 0) {
			return false;
		}
		ptr += n;
		len -= static_cast<size_t>(n);
	}
	return true;
#endif
}

bool Session::ReadFrame(std::string& out) {
	const auto read_all = [this](void* data, size_t len) {
		char* ptr = static_cast<char*>(data);
		while (len != 0) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
			DWORD n = 0;
			if (!PipeIo(m_pipe, false, ptr, static_cast<DWORD>(len), n)) {
				return false;
			}
#else
			const ssize_t n = ::recv(m_fd, ptr, len, 0);
			if (n < 0 && errno == EINTR) {
				continue;
			}
			if (n <= 0) {
				return false;
			}
#endif
			ptr += n;
			len -= static_cast<size_t>(n);
		}
		return true;
	};

	uint8_t header[4] {};
	if (!read_all(header, sizeof(header))) {
		return false;
	}

	const uint32_t size = header[0] | (header[1] << 8u) | (header[2] << 16u) |
	                      (static_cast<uint32_t>(header[3]) << 24u);
	if (size > MAX_FRAME) {
		LOGF("[Launcher] frame too big (%u bytes)\n", size);
		return false;
	}

	out.resize(size);
	return read_all(out.data(), size);
}

void Session::Dispatch(const std::string& frame) {
	const HostMessage msg = ParseHost(frame);

	if (!msg.ok) {
		LOGF("[Launcher] bad message: %s\n", frame.c_str());
		return;
	}

	LOGF("[Launcher] received: %s\n", msg.type.c_str());

	if (msg.type == "configure") {
		// Boot waits for this, so it's handed over here rather than through a handler.
		std::lock_guard lock(m_configure_mutex);
		if (m_configure_taken) {
			LOGF("[Launcher] configure came after boot started; ignored\n");
		} else {
			m_configure = msg.values;
			m_configure_cv.notify_all();
		}
	} else if (msg.type == "quit") {
		if (m_handlers.quit) {
			m_handlers.quit();
		}
	} else if (msg.type == "overlay_opened" || msg.type == "overlay_closed") {
		if (m_handlers.overlay) {
			m_handlers.overlay(msg.type == "overlay_opened");
		}
	} else if (msg.type == "overlay_surface") {
		if (m_handlers.overlay_surface) {
			m_handlers.overlay_surface(msg);
		}
	} else if (msg.type == "overlay_visible") {
		if (m_handlers.overlay_visible) {
			m_handlers.overlay_visible(msg.visible);
		}
	} else if (msg.type == "invoke") {
		if (m_handlers.invoke) {
			m_handlers.invoke(msg.key, msg.value);
		}
	} else if (msg.type == "setting") {
		if (m_handlers.setting) {
			m_handlers.setting(msg.key, msg.value);
		}
	} else if (msg.type == "osk_available") {
		if (m_handlers.osk_available) {
			m_handlers.osk_available(msg.available);
		}
	} else if (msg.type == "osk_edit") {
		// A key this Kyty doesn't know still counts as handled
		if (m_handlers.osk_edit) {
			m_handlers.osk_edit(msg.osk, msg.seq, msg.edit);
		}
	} else if (msg.type == "osk_finish") {
		if (m_handlers.osk_finish) {
			m_handlers.osk_finish(msg.osk, msg.accepted);
		}
	}
	// accepted and anything newer are only logged
}

void Session::Reader() {
	std::string frame;
	while (ReadFrame(frame)) {
		Dispatch(frame);
	}

	// The process tree is what the launcher tracks, so losing the connection isn't fatal.
	LOGF("[Launcher] connection closed\n");
	// No shell draws keyboards now: the game shows its own
	if (m_handlers.osk_available) {
		m_handlers.osk_available(false);
	}

	// Nothing more will come, so boot needn't wait for configure.
	std::lock_guard lock(m_configure_mutex);
	m_reader_done = true;
	m_configure_cv.notify_all();
}

std::optional<ConfigureValues> Session::WaitForConfigure(std::chrono::milliseconds timeout) {
	std::unique_lock lock(m_configure_mutex);
	if (!m_configure_cv.wait_until(lock, m_connected_at + timeout,
	                               [this]() { return m_configure.has_value() || m_reader_done; })) {
		LOGF("[Launcher] no configure; using Kyty's own options\n");
	}
	m_configure_taken = true;
	return m_configure;
}

void Session::Writer() {
	for (;;) {
		QueuedFrame frame;
		{
			std::unique_lock lock(m_queue_mutex);
			m_queue_cv.wait(lock, [this]() { return !m_queue.empty(); });
			frame = std::move(m_queue.front());
			m_queue.pop_front();
			m_writing = true;
		}

		// A write fails once the launcher has gone (it may have been restarted); say so once
		if (!WriteFrame(frame.payload) && !m_write_failed.exchange(true)) {
			LOGF("[Launcher] could not send '%s'; later messages are dropped\n",
			     frame.type.c_str());
		}

		{
			std::lock_guard lock(m_queue_mutex);
			m_writing = false;
		}
		m_flushed_cv.notify_all();
	}
}

void Session::Send(const Message& msg) {
	QueuedFrame frame {msg.json, msg.type, msg.activity_id};
	{
		std::lock_guard lock(m_queue_mutex);

		// Only frames still waiting are replaced, so updates coalesce only while the queue backs
		// up. Stop at the id's latest frame: an update after activity_done starts a new activity.
		if (frame.type == "activity") {
			for (auto it = m_queue.rbegin(); it != m_queue.rend(); ++it) {
				if (it->activity_id == frame.activity_id) {
					if (it->type == "activity") {
						it->payload = std::move(frame.payload);
						return;
					}
					break;
				}
			}
		}

		m_queue.push_back(std::move(frame));
	}
	m_queue_cv.notify_one();
}

bool Session::Flush(std::chrono::milliseconds timeout) {
	std::unique_lock lock(m_queue_mutex);
	return m_flushed_cv.wait_for(lock, timeout, [this]() { return m_queue.empty() && !m_writing; });
}

void Session::Booting() {
	LOGF("[Launcher] sending booting\n");
	Send(Launcher::Booting());
}

void Session::Ready(std::optional<uint32_t> pid, std::optional<uint64_t> window) {
	LOGF("[Launcher] sending ready (pid=%s, window=%s)\n",
	     pid ? std::to_string(*pid).c_str() : "null",
	     window ? std::to_string(*window).c_str() : "null");
	std::lock_guard lock(m_view_mutex);
	Send(Launcher::Ready(pid, window));
	m_ready_sent = true;
	if (m_view) {
		Send(ViewMsg(m_view->first, m_view->second));
	}
}

void Session::Paused(bool paused) {
	Send(Launcher::Paused(paused));
}

void Session::Failed(std::string_view reason, std::string_view code) {
	if (m_failed.exchange(true)) {
		return;
	}
	LOGF("[Launcher] sending failed: %.*s (%.*s)\n", static_cast<int>(reason.size()),
	     reason.data(), static_cast<int>(code.size()), code.data());
	Send(Launcher::Failed(reason, code));
	if (!Flush(std::chrono::seconds(2))) {
		LOGF("[Launcher] timed out sending failed\n");
	}
}

void Session::Activity(const std::string& id, const std::string& label,
                       std::optional<float> progress) {
	{
		std::lock_guard lock(m_activity_mutex);
		auto&           state = m_activities[id];
		const auto      now   = std::chrono::steady_clock::now();
		if (state.label == label && now - state.last_sent < ACTIVITY_INTERVAL) {
			return;
		}
		state.label     = label;
		state.last_sent = now;
	}
	Send(ActivityMsg(id, label, progress));
}

void Session::ActivityDone(const std::string& id) {
	{
		std::lock_guard lock(m_activity_mutex);
		if (m_activities.erase(id) == 0) {
			return;
		}
	}
	Send(ActivityDoneMsg(id));
}

void Session::Notify(std::string_view title, std::string_view body, bool error) {
	Send(Launcher::Notify(title, body, error));
}

void Session::Achievements(const std::vector<Achievement>& list) {
	LOGF("[Launcher] sending %zu trophies\n", list.size());
	Send(AchievementsMsg(list));
}

void Session::Unlocked(std::string_view id, std::optional<int64_t> at) {
	LOGF("[Launcher] sending unlocked %.*s\n", static_cast<int>(id.size()), id.data());
	Send(UnlockedMsg(id, at));
}

void Session::OverlayItems(const std::vector<Setting>& items) {
	Send(OverlayItemsMsg(items));
}

void Session::View(uint32_t width, uint32_t height) {
	std::lock_guard lock(m_view_mutex);
	if (m_view == std::pair(width, height)) {
		return;
	}
	m_view = std::pair(width, height);
	if (m_ready_sent) {
		LOGF("[Launcher] sending view %ux%u\n", width, height);
		Send(ViewMsg(width, height));
	}
}

void Session::CaptureSurface(uint32_t id, uint32_t width, uint32_t height) {
	LOGF("[Launcher] sending capture_surface %u (%ux%u)\n", id, width, height);
	Send(CaptureSurfaceMsg(id, width, height));
}

void Session::OskOpen(const ::Common::Launcher::OskOpen& open) {
	LOGF("[Launcher] sending osk_open %u (%s)\n", open.osk, open.kind.c_str());
	Send(OskOpenMsg(open));
}

void Session::OskText(uint32_t osk, uint64_t seq, std::string_view text, uint32_t cursor) {
	Send(OskTextMsg(osk, seq, text, cursor));
}

void Session::OskClosed(uint32_t osk, std::string_view result) {
	LOGF("[Launcher] sending osk_closed %u (%.*s)\n", osk, static_cast<int>(result.size()),
	     result.data());
	Send(OskClosedMsg(osk, result));
}

} // namespace Common::Launcher
