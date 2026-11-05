#include "common/assert.h"

#include "common/launcher/session.h"
#include "common/subsystems.h"
#include "kytyGitVersion.h"

#include <cstdio>
#include <cstdlib>
#include <fmt/format.h>
#include <mutex>

namespace Common {

// Tells the launcher (common/launcher/session.h) why Kyty is stopping. In a crash cascade several
// threads report at once; the first is the useful one, and the others wait here until it is sent so
// none of them exits first. Recursive so a check failing inside Failed() on the same thread skips
// the report instead of deadlocking.
static void ReportFatalToLauncher(std::string_view text) {
	static std::recursive_mutex mutex;
	static bool                 sent = false;
	std::lock_guard             lock(mutex);
	auto*                       session = Launcher::Session::Get();
	if (sent || session == nullptr) {
		return;
	}
	sent = true;
	// Trailing newlines are for the console.
	while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
		text.remove_suffix(1);
	}
	session->Failed(text.empty() ? "Kyty stopped with a fatal error." : text, "fatal_error");
}

[[noreturn]] static void DbgReport(const char* title, std::string_view text, const char* file,
                                   int line, std::string_view launcher_text) {
	Log::WriteToConsoleAndLog(fmt::format("--- Build ---\n{}\n{}\n{} in {}:{}\n", KYTY_BUILD_LABEL,
	                                      title, text, file, line));
	ReportFatalToLauncher(launcher_text);
	Subsystems::EmergencyShutdownActive();
	std::fflush(nullptr);
	std::_Exit(321);
}

void DbgExitIfHandler(const char* expr, const char* file, int line) {
	const auto text = fmt::format("Error: condition ({}) is true", expr);
	// Failed checks read as code, so the launcher's message says what happened first.
	DbgReport("--- Fatal Error ---", text, file, line,
	          fmt::format("Kyty stopped with a fatal error (see the log). {}", text));
}

void DbgNotImplementedHandler(const char* expr, const char* file, int line) {
	const auto text = fmt::format("Not implemented ({})", expr);
	DbgReport("--- Fatal Error ---", text, file, line,
	          fmt::format("Kyty stopped with a fatal error (see the log). {}", text));
}

void DbgExitHandler(const char* file, int line, std::string_view text) {
	DbgReport("--- Error ---", text, file, line, text);
}

} // namespace Common
