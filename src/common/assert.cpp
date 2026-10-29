#include "common/assert.h"

#include "common/launcher/session.h"
#include "common/logging/log.h"
#include "common/subsystems.h"
#include "kytyGitVersion.h"

#include <cstdio>
#include <cstdlib>
#include <fmt/format.h>
#include <mutex>
#include <string>

namespace Common {

// What the first fatal report said, for the launcher (common/launcher/session.h). In a crash
// cascade several threads report at once; the first is the useful one.
static std::mutex  g_fatal_mutex;
static std::string g_fatal_text;

static void RememberFatal(std::string_view text) {
	std::lock_guard lock(g_fatal_mutex);
	if (g_fatal_text.empty()) {
		g_fatal_text = text;
	}
}

static std::string BuildFatalReport(const char* title, std::string_view text, const char* file,
                                    int line) {
	return fmt::format("--- Build ---\n{}\n{}\n{} in {}:{}\n", KYTY_BUILD_LABEL, title, text, file,
	                   line);
}

static int DbgReport(const char* title, std::string_view text, const char* file, int line) {
	// Failed checks read as code, so the launcher's message says what happened first.
	RememberFatal(fmt::format("Kyty stopped with a fatal error (see the log). {}", text));
	Log::WriteFatal(BuildFatalReport(title, text, file, line));
	Subsystems::EmergencyShutdownActive();
	return 1;
}

int DbgExitIfHandler(const char* expr, const char* file, int line) {
	return DbgReport("--- Fatal Error ---", fmt::format("Error: condition ({}) is true", expr),
	                 file, line);
}

int DbgNotImplementedHandler(const char* expr, const char* file, int line) {
	return DbgReport("--- Fatal Error ---", fmt::format("Not implemented ({})", expr), file, line);
}

int DbgExitHandler(const char* file, int line, std::string_view text) {
	RememberFatal(text);
	Log::WriteFatal(BuildFatalReport("--- Error ---", text, file, line));
	return 1;
}

int DbgExitHandler(const char* file, int line, fmt::text_style style, std::string_view text) {
	RememberFatal(text);
	Log::WriteFatal(style, BuildFatalReport("--- Error ---", text, file, line));
	return 1;
}

void DbgExit(int status) {
	if (auto* session = Launcher::Session::Get(); session != nullptr) {
		// Trailing newlines are for the console.
		std::string text;
		{
			std::lock_guard lock(g_fatal_mutex);
			text = g_fatal_text;
		}
		std::string_view reason = text;
		while (!reason.empty() && (reason.back() == '\n' || reason.back() == '\r')) {
			reason.remove_suffix(1);
		}
		session->Failed(reason.empty() ? "Kyty stopped with a fatal error." : reason, "fatal_error");
	}
	Subsystems::EmergencyShutdownActive();
	std::fflush(nullptr);
	std::_Exit(status);
}

} // namespace Common
