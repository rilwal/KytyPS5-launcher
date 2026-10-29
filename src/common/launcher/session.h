#ifndef KYTY_COMMON_LAUNCHER_SESSION_H_
#define KYTY_COMMON_LAUNCHER_SESSION_H_

#include "common/common.h"
#include "common/launcher/protocol.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Launcher mode: when the game launcher (github.com/rilwal/game-launcher) starts Kyty, it sets
// LAUNCHER_ENDPOINT (a Unix socket path, or a pipe name on Windows) and LAUNCHER_SESSION (a token
// for this game session). Kyty connects back and speaks the plugin protocol (the launcher's
// docs/plugin-api.md) as a "session" plugin: it reports status, trophies and pauses, and the
// launcher draws all UI. Frames are a little-endian u32 length followed by a JSON payload.
namespace Common::Launcher {

// True if the launcher started this process (both environment variables are set).
[[nodiscard]] bool EnvPresent();

// This process's id, for ready.
[[nodiscard]] uint32_t ProcessId();

// What the launcher asks of the game. Called on the session's reader thread.
struct Handlers {
	std::function<void()>     quit;
	std::function<void(bool)> overlay; // The quick menu opened (true) or closed
	// An overlay item was chosen: key and value as text (null for an action)
	std::function<void(const std::string&, const std::optional<std::string>&)> invoke;
	// A live setting changed while the game runs: Kyty's key, value as text (null for the default)
	std::function<void(const std::string&, const std::optional<std::string>&)> setting;
};

class Session final {
public:
	// The connected session, or nullptr outside launcher mode or if connecting failed.
	[[nodiscard]] static Session* Get();

	// Connects, sends the hello (declaring "session" plus `capabilities`) and waits for the
	// welcome. Returns the session or nullptr. Lives until the process exits.
	static Session* Connect(Handlers handlers, const std::vector<std::string>& capabilities = {});

	Session(const Session&)            = delete;
	Session& operator=(const Session&) = delete;

	// Queues the message; a writer thread sends it, so emulator threads never wait on the
	// launcher. A queued activity update is replaced by a newer one for the same id.
	void Send(const Message& msg);

	// Waits until everything queued so far is written, or `timeout` passes. False on timeout.
	bool Flush(std::chrono::milliseconds timeout);

	void Booting();
	void Ready(std::optional<uint32_t> pid, std::optional<uint64_t> window);
	void Paused(bool paused);
	// Sent at most once. Blocks until written (2 s at most), since the process exits right after.
	void Failed(std::string_view reason, std::string_view code);
	// Updates are throttled per id; the label changing always goes out.
	void Activity(const std::string& id, const std::string& label, std::optional<float> progress);
	void ActivityDone(const std::string& id);
	void Notify(std::string_view title, std::string_view body, bool error);
	void Achievements(const std::vector<Achievement>& list);
	void Unlocked(std::string_view id, std::optional<int64_t> at);
	void OverlayItems(const std::vector<Setting>& items);

	[[nodiscard]] bool HasFailed() const { return m_failed.load(); }

	// The launcher's configure message, which comes right after the welcome and before boot:
	// values by Kyty's own keys. Waits until `timeout` after connecting at most.
	std::optional<ConfigureValues> WaitForConfigure(std::chrono::milliseconds timeout);

private:
	Session() = default;
	~Session();

	bool Handshake(const std::vector<std::string>& capabilities);
	bool OpenEndpoint(const std::string& endpoint);
	bool WaitReadable(int timeout_ms);
	void Reader();
	[[noreturn]] void Writer();
	bool WriteFrame(const std::string& payload);
	bool ReadFrame(std::string& out);
	void Dispatch(const std::string& frame);

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	void* m_pipe = nullptr; // The pipe's HANDLE, opened for overlapped I/O
#else
	int m_fd = -1;
#endif
	Handlers          m_handlers;
	std::atomic<bool> m_failed {false};

	struct QueuedFrame {
		std::string payload;
		std::string type;
		std::string activity_id; // activity and activity_done only
	};
	std::mutex              m_queue_mutex;
	std::condition_variable m_queue_cv;   // Something was queued
	std::condition_variable m_flushed_cv; // The queue emptied
	std::deque<QueuedFrame> m_queue;
	bool                    m_writing = false; // The writer has taken a frame and is writing it
	std::atomic<bool>       m_write_failed {false};

	std::chrono::steady_clock::time_point m_connected_at;
	std::mutex                            m_configure_mutex;
	std::condition_variable               m_configure_cv;
	std::optional<ConfigureValues>        m_configure;
	bool                                  m_configure_taken = false;
	bool                                  m_reader_done     = false; // The connection closed

	struct ActivityState {
		std::string                           label;
		std::chrono::steady_clock::time_point last_sent;
	};
	std::mutex                           m_activity_mutex;
	std::map<std::string, ActivityState> m_activities;
};

} // namespace Common::Launcher

#endif // KYTY_COMMON_LAUNCHER_SESSION_H_
