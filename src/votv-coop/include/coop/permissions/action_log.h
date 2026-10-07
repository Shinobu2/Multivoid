// coop/permissions/action_log.h -- the action log's record, its JSON line and its writer -- engine-free:
// no log line, no clock; the caller passes the time and reports a failure.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace coop::permissions {

// The action log's file name, inside the folder the caller names.
inline constexpr wchar_t kActionLogFileName[] = L"actions.jsonl";

// One admin action as the action log keeps it -- LuckPerms' record: who acted, on what, and the
// command line that did it. The fields are LuckPerms' LoggedAction (LoggedAction.java:69-72): who
// (the proved id and the nick), what it acted on (its type, its id or name, its display name) and
// the description.
struct Action {
    std::string sourceId, sourceName, targetType, targetId, targetName, description;
};

// The action log's line without its `\n`: the compact `ordered_json` `{"timestamp", "source": {"id",
// "name"}, "target": {"type", "id", "name"}, "description"}` in that key order, `timestamp` in epoch
// seconds, a byte that is not UTF-8 replaced (ActionJsonSerializer's keys). PURE.
std::string ActionJson(const Action& a, int64_t timestamp);

// Appends one line to `folder`/actions.jsonl, creating the folder; false when the folder cannot be
// made or the line was not written whole.
bool AppendAction(const std::filesystem::path& folder, const Action& action, int64_t timestamp);

}  // namespace coop::permissions
