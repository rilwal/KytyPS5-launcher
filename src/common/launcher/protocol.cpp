#include "common/launcher/protocol.h"

#include <algorithm>
#include <fmt/format.h>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>

namespace Common::Launcher {

using Json = nlohmann::json;

static Message Make(Json obj) {
	Message msg;
	msg.type = obj.value("type", "");
	msg.json = obj.dump(-1, ' ', false, Json::error_handler_t::replace);
	return msg;
}

static Json Optional(const auto& value) {
	return value ? Json(*value) : Json(nullptr);
}

Message Hello(std::string_view token, int api, const std::vector<std::string>& caps,
              std::string_view name) {
	Json capabilities = Json::array();
	capabilities.push_back("session");
	for (const auto& cap: caps) {
		capabilities.push_back(cap);
	}
	Json hello = {
	    {"schema_hash", 0},
	    {"encodings", {"json"}},
	    {"client", std::string(name)},
	    {"plugin",
	     {{"token", std::string(token)},
	      {"api", api},
	      {"capabilities", capabilities},
	      {"name", std::string(name)}}},
	};
	Message msg = Make(std::move(hello));
	msg.type    = "hello";
	return msg;
}

Message Booting() {
	return Make({{"type", "booting"}});
}

Message Ready(std::optional<uint32_t> pid, std::optional<uint64_t> window) {
	return Make({{"type", "ready"}, {"pid", Optional(pid)}, {"window", Optional(window)}});
}

Message Paused(bool paused) {
	return Make({{"type", "paused"}, {"paused", paused}});
}

Message Failed(std::string_view reason, std::string_view code) {
	Json msg = {{"type", "failed"}, {"reason", std::string(reason)}};
	if (!code.empty()) {
		msg["code"] = std::string(code);
	}
	return Make(std::move(msg));
}

Message ActivityMsg(const std::string& id, const std::string& label,
                    std::optional<float> progress) {
	Message msg = Make({{"type", "activity"},
	                    {"id", id},
	                    {"label", label},
	                    {"progress", progress ? Json(std::clamp(*progress, 0.0f, 1.0f))
	                                          : Json(nullptr)}});
	msg.activity_id = id;
	return msg;
}

Message ActivityDoneMsg(const std::string& id) {
	Message msg     = Make({{"type", "activity_done"}, {"id", id}});
	msg.activity_id = id;
	return msg;
}

Message Notify(std::string_view title, std::string_view body, bool error) {
	return Make({{"type", "notify"},
	             {"title", std::string(title)},
	             {"body", std::string(body)},
	             {"level", error ? "error" : "info"}});
}

static Json AchievementJson(const Achievement& a) {
	Json obj = {{"id", a.id},
	            {"name", a.name},
	            {"description", a.description},
	            {"hidden", a.hidden},
	            {"dlc", a.dlc},
	            {"unlocked_at", Optional(a.unlocked_at)}};
	if (!a.grade.empty()) {
		obj["grade"] = a.grade;
	}
	return obj;
}

static Json AchievementArray(const std::vector<Achievement>& list) {
	Json items = Json::array();
	for (const auto& a: list) {
		items.push_back(AchievementJson(a));
	}
	return items;
}

Message AchievementsMsg(const std::vector<Achievement>& list) {
	return Make({{"type", "achievements"}, {"list", AchievementArray(list)}});
}

std::string AchievementListJson(const std::vector<Achievement>& list) {
	return AchievementArray(list).dump(-1, ' ', false, Json::error_handler_t::replace);
}

Message UnlockedMsg(std::string_view id, std::optional<int64_t> at) {
	return Make({{"type", "unlocked"}, {"id", std::string(id)}, {"at", Optional(at)}});
}

// A SettingValue: {"Bool": true} or {"Text": "..."}
static Json SettingValue(const Setting& s, const std::string& value) {
	if (s.kind == Setting::Kind::Toggle) {
		return {{"Bool", value == "true"}};
	}
	return {{"Text", value}};
}

static Json SettingJson(const Setting& s) {
	Json kind;
	switch (s.kind) {
		case Setting::Kind::Toggle: kind = "Toggle"; break;
		case Setting::Kind::Action: kind = "Action"; break;
		case Setting::Kind::Choice: {
			Json options = Json::array();
			for (const auto& o: s.options) {
				options.push_back({{"value", o.value}, {"label", o.label}});
			}
			kind = {{"Choice", {{"options", std::move(options)}}}};
			break;
		}
	}
	return {{"key", s.key},
	        {"group", ""},
	        {"page", ""},
	        {"section", s.section},
	        {"label", s.label},
	        {"help", s.help},
	        {"kind", std::move(kind)},
	        {"value", SettingValue(s, s.value)},
	        {"default", SettingValue(s, s.default_value)}};
}

Message OverlayItemsMsg(const std::vector<Setting>& items) {
	Json list = Json::array();
	for (const auto& s: items) {
		list.push_back(SettingJson(s));
	}
	return Make({{"type", "overlay_items"}, {"items", std::move(list)}});
}

// A SettingValue as text: externally tagged ({"Text": "150"}, {"Bool": true}), or a plain JSON
// value. Null means the default.
static std::optional<std::string> ValueText(const Json& value) {
	if (value.is_null()) {
		return std::nullopt;
	}
	if (value.is_object() && value.size() == 1) {
		return ValueText(value.begin().value());
	}
	if (value.is_string()) {
		return value.get<std::string>();
	}
	if (value.is_boolean()) {
		return value.get<bool>() ? "true" : "false";
	}
	if (value.is_number_integer()) {
		return std::to_string(value.get<int64_t>());
	}
	if (value.is_number()) {
		return fmt::format("{}", value.get<double>());
	}
	return std::nullopt;
}

HostMessage ParseHost(std::string_view frame) {
	HostMessage msg;
	const Json  obj = Json::parse(frame, nullptr, false);
	if (!obj.is_object()) {
		msg.ok = false;
		return msg;
	}
	const auto text = [&](const char* name) {
		const auto it = obj.find(name);
		return it != obj.end() && it->is_string() ? it->get<std::string>() : std::string {};
	};
	msg.type     = text("type");
	msg.title    = text("title");
	msg.key      = text("key");
	msg.server   = text("server");
	msg.encoding = text("encoding");
	if (const auto it = obj.find("api"); it != obj.end() && it->is_number_integer()) {
		msg.api = it->get<int>();
	}
	if (const auto it = obj.find("value"); it != obj.end()) {
		msg.value = ValueText(*it);
	}
	if (const auto it = obj.find("values"); it != obj.end() && it->is_object()) {
		for (const auto& [key, value]: it->items()) {
			msg.values[key] = ValueText(value);
		}
	}
	return msg;
}

static std::string_view TypeName(const Json& v) {
	switch (v.type()) {
		case Json::value_t::boolean: return "a bool";
		case Json::value_t::number_integer:
		case Json::value_t::number_unsigned:
		case Json::value_t::number_float: return "a number";
		case Json::value_t::string: return "a string";
		case Json::value_t::array: return "an array";
		case Json::value_t::object: return "an object";
		default: return "null";
	}
}

static bool SameType(const Json& a, const Json& b) {
	return TypeName(a) == TypeName(b);
}

// examples/<side>/<type>.json, which has to be an object of that type
static std::optional<Json> Load(const std::string& dir, std::string_view side,
                                std::string_view type, std::vector<std::string>& errors) {
	const std::string name = fmt::format("{}/{}.json", side, type);
	std::ifstream     file(dir + "/" + name, std::ios::binary);
	if (!file) {
		errors.push_back(name + ": missing");
		return std::nullopt;
	}
	std::stringstream text;
	text << file.rdbuf();
	Json doc = Json::parse(text.str(), nullptr, false);
	if (!doc.is_object()) {
		errors.push_back(name + ": not a JSON object");
		return std::nullopt;
	}
	if (doc.value("type", "") != type) {
		errors.push_back(name + ": another type");
		return std::nullopt;
	}
	return doc;
}

// What Kyty builds may only have fields the example has, of the same JSON types, and has to have
// every field the example sets, except `optional` ones it never sends. Objects, and the first
// object in arrays, are compared the same way.
static void Compare(const std::string& where, const Json& built, const Json& example,
                    const std::vector<std::string_view>& optional,
                    std::vector<std::string>& errors) {
	for (const auto& [key, mine]: built.items()) {
		const auto theirs = example.find(key);
		if (theirs == example.end()) {
			errors.push_back(fmt::format("{}: sends '{}', which the launcher doesn't know", where, key));
		} else if (!mine.is_null() && !theirs->is_null() && !SameType(mine, *theirs)) {
			errors.push_back(fmt::format("{}.{}: sends {}, the launcher wants {}", where, key,
			                             TypeName(mine), TypeName(*theirs)));
		} else if (mine.is_object() && theirs->is_object()) {
			Compare(where + "." + key, mine, *theirs, optional, errors);
		} else if (mine.is_array() && theirs->is_array() && !mine.empty() && !theirs->empty() &&
		           mine.front().is_object() && theirs->front().is_object()) {
			Compare(where + "." + key + "[0]", mine.front(), theirs->front(), optional, errors);
		}
	}
	for (const auto& [key, value]: example.items()) {
		if (!value.is_null() && !built.contains(key) &&
		    std::find(optional.begin(), optional.end(), key) == optional.end()) {
			errors.push_back(
			    fmt::format("{}: doesn't send '{}', which the launcher's example has", where, key));
		}
	}
}

std::vector<std::string> SelfTest(const std::string& dir) {
	std::vector<std::string> errors;

	// Everything Kyty sends, against the launcher's example of it
	const auto check = [&](const Message& built, std::vector<std::string_view> optional = {}) {
		if (const auto example = Load(dir, "plugin", built.type, errors)) {
			Compare(built.type, Json::parse(built.json), *example, optional, errors);
		}
	};

	check(Booting());
	check(Ready(4242, 77));
	check(Paused(true));
	check(Failed("The PS3 firmware isn't installed.", "firmware_missing"));
	check(ActivityMsg("shaders", "Compiling shaders", 0.5f));
	check(ActivityDoneMsg("shaders"));
	check(Notify("State saved", "Slot 1", false));
	// Trophies have no points, and Kyty doesn't know their rarity
	check(AchievementsMsg({{"1", "First Steps", "Leave the starting village.", false, "bronze",
	                        false, 1700000000}}),
	      {"points", "rarity"});
	check(UnlockedMsg("1", 1700000000));
	Setting slot;
	slot.key           = "slot";
	slot.section       = "Game";
	slot.label         = "Slot";
	slot.help          = "Which save-state slot to use.";
	slot.kind          = Setting::Kind::Choice;
	slot.options       = {{"1", "Slot 1"}};
	slot.value         = "1";
	slot.default_value = "1";
	check(OverlayItemsMsg({slot}));

	// Everything Kyty reads, from the launcher's examples. The values are the ones its
	// crates/proto/tests/examples.rs writes: a field it renamed reads as empty here.
	const auto host = [&](std::string_view type) -> std::optional<HostMessage> {
		if (const auto doc = Load(dir, "host", type, errors)) {
			return ParseHost(doc->dump());
		}
		return std::nullopt;
	};
	const auto expect = [&](std::string_view type, bool ok) {
		if (!ok) {
			errors.push_back(fmt::format("host/{}.json: doesn't read as it should", type));
		}
	};

	if (const auto msg = host("accepted")) {
		// The API Kyty speaks (session.cpp)
		expect("accepted", msg->api == 2 && msg->title == "Demon's Souls");
	}
	if (const auto msg = host("setting")) {
		expect("setting", msg->key == "api_key" && msg->value == "0123456789ABCDEF");
	}
	if (const auto msg = host("invoke")) {
		expect("invoke", msg->key == "slot" && msg->value == "2");
	}
	if (const auto msg = host("configure")) {
		const ConfigureValues want {{"Video.Renderer", "Vulkan"}, {"Video.Resolution Scale", "150"}};
		expect("configure", msg->values == want);
	}
	// Only their type is read
	for (std::string_view type: {"quit", "overlay_opened", "overlay_closed"}) {
		host(type);
	}

	return errors;
}

} // namespace Common::Launcher
