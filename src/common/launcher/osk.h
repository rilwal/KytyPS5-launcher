#ifndef KYTY_COMMON_LAUNCHER_OSK_H_
#define KYTY_COMMON_LAUNCHER_OSK_H_

#include "common/launcher/protocol.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

// The launcher's on-screen keyboard (its docs/plugin-api.md, "On-screen keyboard"): while the
// shell draws the game's keyboards (osk_available), a keyboard the game opens is drawn by the shell
// into the game's frames instead of Kyty's own. The game's IME keeps the text, so its rules (the
// length, the game's filter) apply; the shell's keys are edits to it, and its text goes back to the
// shell after each change. A real keyboard still types into the IME as before.
//
// The bridge knows the IME only through `Ime` (the system overlay's, which drives both the
// dialog and the core IME), and the launcher only through `Sender`, so it can be tested alone.
namespace Common::Launcher::Osk {

// The game's keyboard as the IME has it. Text and cursor are UTF-16, as the IME keeps them.
struct Field {
	uint64_t                       generation = 0; // Which keyboard: the overlay's session
	std::u16string                 text;
	uint32_t                       cursor     = 0; // In UTF-16 units
	std::string                    kind       = "text";
	std::string                    enter      = "default";
	bool                           multiline  = false;
	bool                           password   = false;
	uint32_t                       max_length = 0; // 0 for none
	std::u16string                 title;
	std::u16string                 placeholder;
	std::optional<OskOpen::Anchor> anchor;
};

// What the bridge does to the IME. Each returns false when the keyboard `generation` isn't open
// (or the edit changed nothing). Some IMEs apply edits later, on the game's thread: `pending`
// says how many are still waiting. `move` goes by characters, a step at a time, as the arrow keys
// do (over a surrogate pair in one step).
struct Ime {
	std::function<bool(Field*)>                        snapshot;
	std::function<bool(uint64_t, std::u16string_view)> insert;
	std::function<bool(uint64_t)>                      backspace;
	std::function<bool(uint64_t, int)>                 move; // By characters
	std::function<bool(uint64_t)>                      accept;
	std::function<bool(uint64_t)>                      cancel;
	std::function<size_t(uint64_t)>                    pending;
};

// The messages to the launcher (Session's).
struct Sender {
	std::function<void(const OskOpen&)>                                 open;
	std::function<void(uint32_t, uint64_t, std::string_view, uint32_t)> text;
	std::function<void(uint32_t, std::string_view)>                     closed;
};

class Bridge {
public:
	// `poll`: check the text on a thread of its own while a keyboard is the shell's; without it
	// (tests), Poll does.
	explicit Bridge(bool poll);
	~Bridge();
	Bridge(const Bridge&)            = delete;
	Bridge& operator=(const Bridge&) = delete;

	void SetIme(std::shared_ptr<const Ime> ime);
	void SetSender(std::shared_ptr<const Sender> sender);
	// Called when the launcher's offer coming or going moves an open keyboard between the shell and
	// the overlay (outside the bridge's lock), so the overlay can say who reads the controller.
	void SetOwnerChanged(std::function<void()> changed);

	// The system overlay: the game opened keyboard `field.generation` (or the overlay noticed
	// it), or none is open now.
	void Opened(const Field& field);
	void Closed();

	// The launcher (osk_available, osk_edit, osk_finish).
	void Available(bool available);
	void Edit(uint32_t osk, uint64_t seq, const OskEdit& edit);
	void Finish(uint32_t osk, bool accepted);

	// Whether keyboard `generation` is the shell's to draw and drive: the overlay draws nothing
	// for it and ignores the controller (which the shell reads), but takes the real keyboard.
	[[nodiscard]] bool Owns(uint64_t generation) const;

	// Hands the shell's keys to the IME (those that need the IME's text once it has applied the
	// rest), and sends the text if it changed since it was last sent.
	void Poll();

	// How long a keyboard the player finished may stay open before it's the shell's again (a game
	// that takes Done as a key and carries on)
	static constexpr std::chrono::milliseconds FINISH_TIMEOUT {1000};

private:
	struct Open {
		uint32_t       id         = 0;
		uint64_t       generation = 0;
		uint64_t       applied    = 0; // The last edit handed to the IME
		uint64_t       reported   = 0; // The last one the shell was told the text includes
		std::u16string text;           // As the shell was last told
		uint32_t       cursor = 0;
		std::string    result = "aborted"; // How it closes: what the player chose, else aborted
		std::deque<std::pair<uint64_t, OskEdit>>             keys; // Not handed to the IME yet
		std::optional<bool> finish; // Done (true) or cancel, handed over after the keys before it
		std::optional<std::chrono::steady_clock::time_point> finished_at; // When it was handed over
	};

	void OpenLocked(const Field& field);
	void CloseLocked();
	void PollThread();
	void HandOver(const Ime& ime, uint64_t generation, const OskEdit& edit);

	std::mutex            m_poll_mutex; // One Poll at a time, so texts go out in order
	std::function<void()> m_owner_changed;

	mutable std::mutex            m_mutex;
	std::shared_ptr<const Ime>    m_ime;
	std::shared_ptr<const Sender> m_sender;
	bool                          m_available = false;
	uint64_t                      m_ime_open  = 0; // The IME's keyboard, the shell's or not
	std::optional<Open>           m_open;
	uint32_t                      m_next_id = 0;
	std::atomic<uint64_t>         m_owned {0};

	bool                    m_poll;
	bool                    m_stop = false;
	std::condition_variable m_wake;
	std::thread             m_thread;
};

// The emulator's one bridge.
[[nodiscard]] Bridge& Get();

// UTF-8 and UTF-16, and positions between UTF-16 units and characters (scalar values).
[[nodiscard]] std::u16string Utf16(std::string_view text);
[[nodiscard]] std::string    Utf8(std::u16string_view text);
[[nodiscard]] uint32_t       Characters(std::u16string_view text, uint32_t units);
[[nodiscard]] uint32_t       Units(std::u16string_view text, uint32_t characters);

} // namespace Common::Launcher::Osk

#endif // KYTY_COMMON_LAUNCHER_OSK_H_
