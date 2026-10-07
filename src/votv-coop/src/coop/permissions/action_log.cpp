// coop/permissions/action_log.cpp -- see coop/permissions/action_log.h.

#include "coop/permissions/action_log.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <system_error>

namespace coop::permissions {

std::string ActionJson(const Action& a, int64_t timestamp) {
    using OJson = nlohmann::ordered_json;
    OJson line = OJson::object();
    line["timestamp"] = timestamp;
    line["source"] = OJson::object();
    line["source"]["id"] = a.sourceId;
    line["source"]["name"] = a.sourceName;
    line["target"] = OJson::object();
    line["target"]["type"] = a.targetType;
    line["target"]["id"] = a.targetId;
    line["target"]["name"] = a.targetName;
    line["description"] = a.description;
    return line.dump(-1, ' ', false, OJson::error_handler_t::replace);
}

bool AppendAction(const std::filesystem::path& folder, const Action& action, int64_t timestamp) {
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    if (ec) return false;
    std::ofstream out(folder / kActionLogFileName, std::ios::app | std::ios::binary);
    if (!out.is_open()) return false;
    out << ActionJson(action, timestamp) << '\n';
    out.flush();
    return out.good();
}

}  // namespace coop::permissions
