// A whole session against a fake launcherd: the handshake, configure, what goes out, and what
// comes back through the callbacks.
#include "check.h"
#include "fake_launcher.h"
#include "launcher_client.h"

#include <atomic>
#include <map>

namespace {

struct Seen {
	std::mutex                         mutex;
	std::condition_variable            cv;
	std::atomic<int>                   quit {0};
	std::vector<bool>                  overlay;
	std::map<std::string, std::string> invoked;
	std::map<std::string, std::string> settings;

	template <typename F>
	bool wait(F done) {
		std::unique_lock lock(mutex);
		return cv.wait_for(lock, std::chrono::seconds(2), done);
	}
};

lc_callbacks callbacks(Seen& seen) {
	lc_callbacks cb {};
	cb.user = &seen;
	cb.log  = [](void*, lc_log_level, const char*) {};
	cb.quit = [](void* user) {
		auto* s = static_cast<Seen*>(user);
		s->quit++;
		s->cv.notify_all();
	};
	cb.overlay = [](void* user, bool opened) {
		auto*           s = static_cast<Seen*>(user);
		std::lock_guard lock(s->mutex);
		s->overlay.push_back(opened);
		s->cv.notify_all();
	};
	cb.invoke = [](void* user, const char* key, const char* value) {
		auto*           s = static_cast<Seen*>(user);
		std::lock_guard lock(s->mutex);
		s->invoked[key] = value != nullptr ? value : "(null)";
		s->cv.notify_all();
	};
	cb.setting = [](void* user, const char* key, const char* value) {
		auto*           s = static_cast<Seen*>(user);
		std::lock_guard lock(s->mutex);
		s->settings[key] = value != nullptr ? value : "(null)";
		s->cv.notify_all();
	};
	return cb;
}

void session() {
	FakeLauncher launcher({
	    R"({"type":"accepted","api":2,"game":7,"title":"Demon's Souls"})",
	    R"({"type":"configure","values":{"Video.Renderer":"Vulkan","Video.Scale":{"Text":"150"},"Audio.Off":null}})",
	});
	Seen          seen;
	const auto    cb = callbacks(seen);
	lc_connect_info info {};
	info.name = "Test Emu 1.0";
	char        error[256] {};
	lc_session* s = lc_connect(&info, &cb, error, sizeof(error));
	CHECK(s != nullptr);
	if (s == nullptr) {
		FAIL("lc_connect: %s", error);
		return;
	}
	CHECK(lc_get() == s);

	// The hello: the token, the API, and only "session" without the overlay
	const auto hello = launcher.hello();
	CHECK(hello && hello->find("plugin")->text("token") == "test-token");
	CHECK(hello && hello->find("plugin")->find("api")->as_uint() == LC_PLUGIN_API);
	CHECK(hello && hello->find("plugin")->find("capabilities")->items().size() == 1);
	CHECK(hello && hello->text("client") == "Test Emu 1.0");

	const lc_kv* values = nullptr;
	size_t       count  = 0;
	CHECK(lc_wait_configure(s, 2000, &values, &count));
	CHECK(count == 3);
	if (count == 3) {
		CHECK(std::string(values[0].key) == "Video.Renderer" && std::string(values[0].value) == "Vulkan");
		CHECK(std::string(values[1].key) == "Video.Scale" && std::string(values[1].value) == "150");
		CHECK(std::string(values[2].key) == "Audio.Off" && values[2].value == nullptr);
	}
	CHECK(std::string(lc_title(s)) == "Demon's Souls");
	CHECK(lc_api(s) == 2);

	lc_booting(s);
	lc_activity(s, "shaders", "Compiling shaders", 0.1f);
	// Coalesced or throttled: the same label within 100 ms
	lc_activity(s, "shaders", "Compiling shaders", 0.2f);
	lc_activity(s, "shaders", "Compiling shaders", 0.3f);
	lc_activity_done(s, "shaders");
	// Not started, so not sent
	lc_activity_done(s, "never");
	lc_ready(s, 0, 0);
	lc_paused(s, true);
	lc_presence(s, "Boletaria");
	lc_notify(s, "State saved", "Slot 1", false);
	lc_achievement a {};
	a.id   = "1";
	a.name = "First";
	lc_achievements(s, &a, 1);
	lc_unlocked(s, "1", LC_UNLOCKED_NOW);
	lc_achievement_progress(s, "2", 3, 10);
	lc_setting item {};
	item.key  = "save";
	item.kind = LC_SETTING_ACTION;
	lc_overlay_items(s, &item, 1);
	CHECK(lc_flush(s, 2000));

	CHECK(launcher.wait_for("overlay_items").has_value());
	CHECK(launcher.count("activity") == 1);
	CHECK(launcher.count("activity_done") == 1);
	const auto ready = launcher.wait_for("ready");
	CHECK(ready && ready->find("pid")->as_uint() == static_cast<uint64_t>(::getpid()));
	CHECK(ready && ready->find("window")->is_null());
	const std::vector<std::string> want {"booting",      "activity",     "activity_done", "ready",
	                                     "paused",       "presence",     "notify",        "achievements",
	                                     "unlocked",     "achievement_progress", "overlay_items"};
	CHECK(launcher.types() == want);

	// What the launcher asks for
	launcher.send(R"({"type":"invoke","key":"save","value":null,"game":null})");
	launcher.send(R"({"type":"invoke","key":"slot","value":{"Text":"2"}})");
	launcher.send(R"({"type":"setting","key":"perf-overlay","value":{"Bool":true}})");
	launcher.send(R"({"type":"overlay_opened"})");
	launcher.send(R"({"type":"something_newer","x":1})");
	launcher.send(R"({"type":"overlay_closed"})");
	launcher.send(R"({"type":"quit"})");
	CHECK(seen.wait([&]() { return seen.quit.load() == 1; }));
	{
		std::lock_guard lock(seen.mutex);
		CHECK(seen.invoked["save"] == "(null)");
		CHECK(seen.invoked["slot"] == "2");
		CHECK(seen.settings["perf-overlay"] == "true");
		CHECK(seen.overlay == std::vector<bool>({true, false}));
	}

	// failed goes out once, and is written before it returns
	lc_failed(s, "It broke.", "fatal_error");
	lc_failed(s, "Again.", nullptr);
	CHECK(launcher.wait_for("failed").has_value());
	CHECK(launcher.count("failed") == 1);

	lc_close(s);
	CHECK(lc_get() == nullptr);
}

void no_configure() {
	FakeLauncher launcher;
	Seen         seen;
	const auto   cb = callbacks(seen);
	lc_session*  s  = lc_connect(nullptr, &cb, nullptr, 0);
	CHECK(s != nullptr);
	const auto start = std::chrono::steady_clock::now();
	CHECK(!lc_wait_configure(s, 200, nullptr, nullptr));
	CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(1));
	lc_close(s);
}

// A launcherd that stops reading doesn't keep close from returning
void stuck_launcher() {
	FakeLauncher launcher;
	lc_session*  s = lc_connect(nullptr, nullptr, nullptr, 0);
	CHECK(s != nullptr);
	CHECK(launcher.hello().has_value());
	launcher.stop_reading();
	// Far more than the socket buffers hold
	const std::string text(16 * 1024, 'x');
	for (int i = 0; i < 400; i++) {
		lc_notify(s, text.c_str(), nullptr, false);
	}
	const auto start = std::chrono::steady_clock::now();
	lc_close(s);
	CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(3));
}

void outside_launcher() {
	::unsetenv("LAUNCHER_ENDPOINT");
	::unsetenv("LAUNCHER_SESSION");
	CHECK(!lc_env_present());
	char error[128] {};
	CHECK(lc_connect(nullptr, nullptr, error, sizeof(error)) == nullptr);
	CHECK(error[0] != '\0');
	// Calls on no session do nothing
	lc_booting(nullptr);
	lc_ready(nullptr, 0, 0);
	CHECK(lc_flush(nullptr, 0));
	CHECK(!lc_wait_configure(nullptr, 0, nullptr, nullptr));
	lc_close(nullptr);
}

} // namespace

int main() {
	session();
	no_configure();
	stuck_launcher();
	outside_launcher();
	return finish("session");
}
