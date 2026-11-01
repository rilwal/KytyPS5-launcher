#include "common/launcher/osk.h"

#include <algorithm>
#include <chrono>
#include <utility>

namespace Common::Launcher::Osk {

namespace {

// How often the text is checked while the shell's keyboard is up, for what changes it besides
// the shell's keys (a real keyboard, the game's filter, an IME applying keys on the game's thread)
constexpr std::chrono::milliseconds POLL {16};

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
	m_open = Open {m_next_id, field.generation, 0, 0, field.text, field.cursor};
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
	{
		std::scoped_lock lock(m_mutex);
		m_available = available;
		if (!available) {
			// The game shows its own from here (the overlay sees Owns turn false)
			CloseLocked();
			return;
		}
		if (m_open || m_ime_open == 0) {
			return;
		}
		ime = m_ime;
	}
	// One that was already open goes to the shell, with its text as it is now
	Field field;
	if (ime == nullptr || !ime->snapshot(&field)) {
		return;
	}
	std::scoped_lock lock(m_mutex);
	if (m_ime_open == field.generation) {
		OpenLocked(field);
	}
}

void Bridge::Edit(uint32_t osk, uint64_t seq, const OskEdit& edit) {
	std::shared_ptr<const Ime> ime;
	uint64_t                   generation = 0;
	{
		std::scoped_lock lock(m_mutex);
		if (!m_open || m_open->id != osk || m_ime == nullptr) {
			return;
		}
		ime        = m_ime;
		generation = m_open->generation;
	}
	// Outside the lock: the IME may call back into the overlay, and the overlay into the bridge
	const auto field = [&]() -> std::optional<Field> {
		Field f;
		if (ime->snapshot(&f) && f.generation == generation) {
			return f;
		}
		return std::nullopt;
	};
	if (edit.op == "insert") {
		ime->insert(generation, Utf16(edit.text));
	} else if (edit.op == "backspace") {
		ime->backspace(generation);
	} else if (edit.op == "delete") {
		// The character after the cursor: step over it and take it back
		if (const auto f = field(); f && f->cursor < f->text.size()) {
			const uint32_t after = Units(f->text, Characters(f->text, f->cursor) + 1);
			if (ime->move(generation, static_cast<int>(after - f->cursor))) {
				ime->backspace(generation);
			}
		}
	} else if (edit.op == "move") {
		if (const auto f = field()) {
			const auto here = static_cast<int64_t>(Characters(f->text, f->cursor));
			const auto to   = std::clamp<int64_t>(
                here + edit.by, 0, Characters(f->text, static_cast<uint32_t>(f->text.size())));
			const auto units = static_cast<int>(Units(f->text, static_cast<uint32_t>(to))) -
			                   static_cast<int>(f->cursor);
			if (units != 0) {
				ime->move(generation, units);
			}
		}
	} else if (edit.op == "replace") {
		// All of it: delete what's there, type the new text, and put the cursor where it says
		if (const auto f = field()) {
			const auto end = static_cast<uint32_t>(f->text.size());
			if (end != f->cursor) {
				ime->move(generation, static_cast<int>(end - f->cursor));
			}
			for (uint32_t n = Characters(f->text, end); n > 0; n--) {
				ime->backspace(generation);
			}
			const auto text = Utf16(edit.text);
			if (!text.empty()) {
				ime->insert(generation, text);
			}
			const auto chars = Characters(text, static_cast<uint32_t>(text.size()));
			if (edit.cursor < chars) {
				ime->move(generation, static_cast<int>(Units(text, edit.cursor)) -
				                          static_cast<int>(text.size()));
			}
		}
	}
	{
		std::scoped_lock lock(m_mutex);
		if (m_open && m_open->id == osk) {
			m_open->applied = std::max(m_open->applied, seq);
		}
	}
	Poll();
}

void Bridge::Finish(uint32_t osk, bool accepted) {
	std::shared_ptr<const Ime> ime;
	uint64_t                   generation = 0;
	{
		std::scoped_lock lock(m_mutex);
		if (!m_open || m_open->id != osk || m_ime == nullptr) {
			return;
		}
		m_open->result = accepted ? "accepted" : "cancelled";
		ime            = m_ime;
		generation     = m_open->generation;
	}
	// The IME closing reaches Closed through the overlay, which says so to the launcher
	if (accepted) {
		ime->accept(generation);
	} else {
		ime->cancel(generation);
	}
}

bool Bridge::Owns(uint64_t generation) const {
	return generation != 0 && m_owned.load(std::memory_order_acquire) == generation;
}

void Bridge::Poll() {
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
	Field field;
	if (!ime->snapshot(&field) || field.generation != generation) {
		return;
	}
	const bool       caught_up = ime->pending(generation) == 0;
	std::scoped_lock lock(m_mutex);
	if (!m_open || m_open->id != id || m_sender == nullptr) {
		return;
	}
	auto&      open     = *m_open;
	const auto reported = caught_up ? open.applied : open.reported;
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
