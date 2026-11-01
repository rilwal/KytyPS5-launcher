#include "common/launcher/osk.h"

#include <algorithm>
#include <chrono>
#include <utility>

namespace Common::Launcher::Osk {

namespace {

// How often the text is checked while the shell's keyboard is up, for what changes it besides
// the shell's keys (a real keyboard, the game's filter, an IME applying keys on the game's thread)
constexpr std::chrono::milliseconds POLL {16};

// The most steps a replace takes (it's left undone past that): the core IME queues 128 keys
constexpr uint32_t MAX_REPLACE_STEPS = 120;

bool High(char16_t c) {
	return c >= 0xd800 && c <= 0xdbff;
}

bool Low(char16_t c) {
	return c >= 0xdc00 && c <= 0xdfff;
}

} // namespace

std::u16string Utf16(std::string_view text) {
	std::u16string out;
	out.reserve(text.size());
	for (size_t i = 0; i < text.size();) {
		const auto   c    = static_cast<unsigned char>(text[i]);
		const size_t size = c < 0x80           ? 1
		                    : (c >> 5) == 0x6  ? 2
		                    : (c >> 4) == 0xe  ? 3
		                    : (c >> 3) == 0x1e ? 4
		                                       : 0;
		if (size == 0 || i + size > text.size()) {
			// Not UTF-8: skipped
			i++;
			continue;
		}
		uint32_t cp = size == 1 ? c : size == 2 ? (c & 0x1f) : size == 3 ? (c & 0x0f) : (c & 0x07);
		bool     ok = true;
		for (size_t k = 1; k < size; k++) {
			const auto next = static_cast<unsigned char>(text[i + k]);
			ok              = ok && (next & 0xc0) == 0x80;
			cp              = (cp << 6) | (next & 0x3f);
		}
		i += size;
		if (!ok || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) {
			continue;
		}
		if (cp >= 0x10000) {
			cp -= 0x10000;
			out.push_back(static_cast<char16_t>(0xd800 + (cp >> 10)));
			out.push_back(static_cast<char16_t>(0xdc00 + (cp & 0x3ff)));
		} else {
			out.push_back(static_cast<char16_t>(cp));
		}
	}
	return out;
}

std::string Utf8(std::u16string_view text) {
	std::string out;
	out.reserve(text.size());
	for (size_t i = 0; i < text.size(); i++) {
		uint32_t cp = text[i];
		if (High(text[i]) && i + 1 < text.size() && Low(text[i + 1])) {
			cp = 0x10000 + ((cp - 0xd800) << 10) + (text[i + 1] - 0xdc00);
			i++;
		} else if (High(text[i]) || Low(text[i])) {
			cp = 0xfffd;
		}
		if (cp < 0x80) {
			out.push_back(static_cast<char>(cp));
		} else if (cp < 0x800) {
			out.push_back(static_cast<char>(0xc0 | (cp >> 6)));
			out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
		} else if (cp < 0x10000) {
			out.push_back(static_cast<char>(0xe0 | (cp >> 12)));
			out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
			out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
		} else {
			out.push_back(static_cast<char>(0xf0 | (cp >> 18)));
			out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3f)));
			out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
			out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
		}
	}
	return out;
}

uint32_t Characters(std::u16string_view text, uint32_t units) {
	uint32_t   n   = 0;
	const auto end = std::min<size_t>(units, text.size());
	for (size_t i = 0; i < end; i++) {
		if (!(Low(text[i]) && i > 0 && High(text[i - 1]))) {
			n++;
		}
	}
	return n;
}

uint32_t Units(std::u16string_view text, uint32_t characters) {
	size_t i = 0;
	for (uint32_t n = 0; n < characters && i < text.size(); n++) {
		i += High(text[i]) && i + 1 < text.size() && Low(text[i + 1]) ? 2 : 1;
	}
	return static_cast<uint32_t>(i);
}

Bridge::Bridge(bool poll): m_poll(poll) {}

Bridge::~Bridge() {
	{
		std::scoped_lock lock(m_mutex);
		m_stop = true;
	}
	m_wake.notify_all();
	if (m_thread.joinable()) {
		m_thread.join();
	}
}

void Bridge::SetIme(std::shared_ptr<const Ime> ime) {
	std::scoped_lock lock(m_mutex);
	m_ime = std::move(ime);
	if (m_ime == nullptr) {
		// Nothing draws the game's keyboards without the overlay
		CloseLocked();
		m_ime_open = 0;
	}
}

void Bridge::SetSender(std::shared_ptr<const Sender> sender) {
	std::scoped_lock lock(m_mutex);
	m_sender = std::move(sender);
}

void Bridge::OpenLocked(const Field& field) {
	if (m_open && m_open->generation == field.generation) {
		return;
	}
	CloseLocked();
	if (!m_available || m_sender == nullptr || m_ime == nullptr) {
		return;
	}
	if (++m_next_id == 0) {
		m_next_id = 1;
	}
	OskOpen msg;
	msg.osk         = m_next_id;
	msg.kind        = field.kind;
	msg.title       = Utf8(field.title);
	msg.placeholder = Utf8(field.placeholder);
	msg.text        = Utf8(field.text);
	msg.cursor      = Characters(field.text, field.cursor);
	if (field.max_length != 0) {
		msg.max_length = field.max_length;
	}
	msg.multiline = field.multiline;
	msg.password  = field.password;
	msg.enter     = field.enter;
	msg.anchor    = field.anchor;
	m_sender->open(msg);
	m_open             = Open {};
	m_open->id         = m_next_id;
	m_open->generation = field.generation;
	m_open->text       = field.text;
	m_open->cursor     = field.cursor;
	m_owned.store(field.generation, std::memory_order_release);
	if (m_poll && !m_thread.joinable()) {
		m_thread = std::thread([this]() { PollThread(); });
	}
	m_wake.notify_all();
}

void Bridge::CloseLocked() {
	if (!m_open) {
		return;
	}
	if (m_sender != nullptr) {
		m_sender->closed(m_open->id, m_open->result);
	}
	m_open.reset();
	m_owned.store(0, std::memory_order_release);
}

void Bridge::Opened(const Field& field) {
	std::scoped_lock lock(m_mutex);
	m_ime_open = field.generation;
	OpenLocked(field);
}

void Bridge::Closed() {
	std::scoped_lock lock(m_mutex);
	m_ime_open = 0;
	CloseLocked();
}

void Bridge::Available(bool available) {
	std::shared_ptr<const Ime> ime;
	std::function<void()>      changed;
	{
		std::scoped_lock lock(m_mutex);
		m_available = available;
		if (!available) {
			// The game shows its own from here; the player didn't finish it, whatever they pressed
			if (m_open) {
				m_open->result = "aborted";
				CloseLocked();
				changed = m_owner_changed;
			}
		} else if (!m_open && m_ime_open != 0) {
			ime = m_ime;
		}
	}
	// One that was already open goes to the shell, with its text as it is now
	Field field;
	if (ime != nullptr && ime->snapshot(&field)) {
		std::scoped_lock lock(m_mutex);
		if (m_ime_open == field.generation && !m_open) {
			OpenLocked(field);
			changed = m_open ? m_owner_changed : nullptr;
		}
	}
	// Outside the lock: the overlay asks the bridge who has it
	if (changed) {
		changed();
	}
}

void Bridge::SetOwnerChanged(std::function<void()> changed) {
	std::scoped_lock lock(m_mutex);
	m_owner_changed = std::move(changed);
}

void Bridge::Edit(uint32_t osk, uint64_t seq, const OskEdit& edit) {
	{
		std::scoped_lock lock(m_mutex);
		if (!m_open || m_open->id != osk) {
			return;
		}
		m_open->keys.emplace_back(seq, edit);
	}
	Poll();
}

void Bridge::HandOver(const Ime& ime, uint64_t generation, const OskEdit& edit) {
	// The IME's text, which is all of it by now for the keys that need it
	const auto field = [&]() -> std::optional<Field> {
		Field f;
		if (ime.snapshot(&f) && f.generation == generation) {
			return f;
		}
		return std::nullopt;
	};
	if (edit.op == "insert") {
		ime.insert(generation, Utf16(edit.text));
	} else if (edit.op == "backspace") {
		ime.backspace(generation);
	} else if (edit.op == "move") {
		if (edit.by != 0) {
			ime.move(generation, edit.by);
		}
	} else if (edit.op == "delete") {
		// The character after the cursor: step over it and take it back
		if (const auto f = field(); f && f->cursor < f->text.size()) {
			if (ime.move(generation, 1)) {
				ime.backspace(generation);
			}
		}
	} else if (edit.op == "replace") {
		// All of it: from the end, take back every character, type the new text, and step back to
		// where the cursor goes
		if (const auto f = field()) {
			const auto size  = static_cast<uint32_t>(f->text.size());
			const auto total = Characters(f->text, size);
			const auto at    = Characters(f->text, f->cursor);
			const auto text  = Utf16(edit.text);
			const auto chars = Characters(text, static_cast<uint32_t>(text.size()));
			// A step each, and the core IME queues no more than 128 at a time
			if ((total - at) + total + 1 + chars > MAX_REPLACE_STEPS) {
				return;
			}
			if (total > at) {
				ime.move(generation, static_cast<int>(total - at));
			}
			for (uint32_t n = total; n > 0; n--) {
				ime.backspace(generation);
			}
			if (!text.empty()) {
				ime.insert(generation, text);
			}
			if (edit.cursor < chars) {
				ime.move(generation, -static_cast<int>(chars - edit.cursor));
			}
		}
	}
	// Anything else, from a newer launcher, is a key that does nothing here
}

void Bridge::Finish(uint32_t osk, bool accepted) {
	{
		std::scoped_lock lock(m_mutex);
		if (!m_open || m_open->id != osk || m_open->finish) {
			return;
		}
		m_open->result = accepted ? "accepted" : "cancelled";
		// After the keys before it, which may still be waiting for the IME
		m_open->finish = accepted;
	}
	Poll();
}

bool Bridge::Owns(uint64_t generation) const {
	return generation != 0 && m_owned.load(std::memory_order_acquire) == generation;
}

void Bridge::Poll() {
	std::scoped_lock           poll(m_poll_mutex);
	std::shared_ptr<const Ime> ime;
	uint32_t                   id         = 0;
	uint64_t                   generation = 0;
	{
		std::scoped_lock lock(m_mutex);
		if (!m_open || m_ime == nullptr) {
			return;
		}
		ime        = m_ime;
		id         = m_open->id;
		generation = m_open->generation;
	}

	// The shell's keys, in order. Delete and replace need the IME's text as it will be, so they
	// (and the keys after them) wait until the IME has applied every key before them.
	for (;;) {
		std::optional<std::pair<uint64_t, OskEdit>> key;
		{
			std::scoped_lock lock(m_mutex);
			if (!m_open || m_open->id != id || m_open->keys.empty()) {
				break;
			}
			key = m_open->keys.front();
		}
		const bool needs_text = key->second.op == "delete" || key->second.op == "replace";
		if (needs_text && ime->pending(generation) != 0) {
			break;
		}
		// Outside the lock: the IME may call back into the overlay, and the overlay into the bridge
		HandOver(*ime, generation, key->second);
		std::scoped_lock lock(m_mutex);
		if (!m_open || m_open->id != id) {
			return;
		}
		m_open->keys.pop_front();
		m_open->applied = std::max(m_open->applied, key->first);
	}

	// Done or cancel, once every key before it is handed over. The IME closing reaches Closed
	// through the overlay, which says so to the launcher.
	std::optional<bool> finish;
	{
		std::scoped_lock lock(m_mutex);
		if (!m_open || m_open->id != id) {
			return;
		}
		if (m_open->keys.empty() && m_open->finish && !m_open->finished_at) {
			finish              = m_open->finish;
			m_open->finished_at = std::chrono::steady_clock::now();
		}
	}
	if (finish) {
		if (*finish) {
			ime->accept(generation);
		} else {
			ime->cancel(generation);
		}
	}

	// The text, and the last key it's sure to include: one handed over before the IME said it had
	// nothing waiting, read before the text
	uint64_t applied = 0;
	{
		std::scoped_lock lock(m_mutex);
		if (!m_open || m_open->id != id) {
			return;
		}
		applied = m_open->applied;
	}
	const bool caught_up = ime->pending(generation) == 0;
	Field      field;
	if (!ime->snapshot(&field) || field.generation != generation) {
		return;
	}
	std::scoped_lock lock(m_mutex);
	if (!m_open || m_open->id != id || m_sender == nullptr) {
		return;
	}
	auto& open = *m_open;
	// Finished, but the game kept it open: the shell's again, as a new keyboard
	if (open.finished_at &&
	    std::chrono::steady_clock::now() - *open.finished_at >= FINISH_TIMEOUT) {
		CloseLocked();
		if (m_ime_open == field.generation) {
			OpenLocked(field);
		}
		return;
	}
	const auto reported = caught_up ? std::max(open.reported, applied) : open.reported;
	if (field.text == open.text && field.cursor == open.cursor && reported == open.reported) {
		return;
	}
	open.text     = field.text;
	open.cursor   = field.cursor;
	open.reported = reported;
	m_sender->text(id, reported, Utf8(field.text), Characters(field.text, field.cursor));
}

void Bridge::PollThread() {
	std::unique_lock lock(m_mutex);
	while (!m_stop) {
		if (!m_open) {
			m_wake.wait(lock, [this]() { return m_stop || m_open.has_value(); });
			continue;
		}
		m_wake.wait_for(lock, POLL);
		lock.unlock();
		Poll();
		lock.lock();
	}
}

Bridge& Get() {
	// Never destroyed: emulator threads may still ask at exit
	static auto* bridge = new Bridge(true);
	return *bridge;
}

} // namespace Common::Launcher::Osk
