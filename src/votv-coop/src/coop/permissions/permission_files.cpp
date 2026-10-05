// coop/permissions/permission_files.cpp -- see coop/permissions/permission_files.h.

#include "coop/permissions/permission_files.h"

#include <nlohmann/json.hpp>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <system_error>
#include <utility>

namespace coop::permissions {
namespace {

namespace fs = std::filesystem;
using Json = nlohmann::json;

std::string Lowered(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    }
    return s;
}

bool IsPlayerIdStem(std::string_view lowered) {
    if (lowered.size() != 32) return false;
    for (char c : lowered) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

// The attributes of one entry, read from the entry object (the `permission` and `group` forms) or
// from the inner object (the one-key form). Fills `n`; false with `*badField` set when a field is
// of the wrong type or a context pair is refused. `value` is read only for a permission entry.
bool ReadAttributes(const Json& obj, bool readValue, Node* n, const char** badField) {
    if (readValue) {
        const auto it = obj.find("value");
        if (it != obj.end()) {
            if (!it->is_boolean()) {
                *badField = "value";
                return false;
            }
            n->value = it->get<bool>();
        }
    }
    const auto expiry = obj.find("expiry");
    if (expiry != obj.end()) {
        const bool fits = expiry->is_number_integer() &&
                          (!expiry->is_number_unsigned() ||
                           expiry->get<uint64_t>() <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max()));
        if (!fits) {
            *badField = "expiry";
            return false;
        }
        n->expiry = expiry->get<int64_t>();
    }
    for (const char* key : {"server", "world"}) {
        const auto it = obj.find(key);
        if (it == obj.end()) continue;
        if (!it->is_string() || !n->contexts.Add(key, it->get<std::string>())) {
            *badField = key;
            return false;
        }
    }
    const auto ctx = obj.find("context");
    if (ctx != obj.end()) {
        if (!ctx->is_object()) {
            *badField = "context";
            return false;
        }
        for (const auto& [ckey, cval] : ctx->items()) {
            if (cval.is_string()) {
                if (!n->contexts.Add(ckey, cval.get<std::string>())) {
                    *badField = "context";
                    return false;
                }
                continue;
            }
            // An empty list adds no pair, which would widen the entry to global: refused.
            if (!cval.is_array() || cval.empty()) {
                *badField = "context";
                return false;
            }
            for (const Json& v : cval) {
                if (!v.is_string() || !n->contexts.Add(ckey, v.get<std::string>())) {
                    *badField = "context";
                    return false;
                }
            }
        }
    }
    return true;
}

// The one-key form: an object with exactly one member, whose value is an
// object. `*name` is that member's key, `*attrs` its object.
bool ReadOneKeyForm(const Json& entry, std::string* name, const Json** attrs) {
    if (entry.size() != 1) return false;
    const auto it = entry.begin();
    if (!it.value().is_object()) return false;
    *name = it.key();
    *attrs = &it.value();
    return true;
}

// Reads one `permissions` entry into `n`; false with `*badField` set when it is refused.
bool ReadPermissionEntry(const Json& entry, Node* n, const char** badField) {
    if (entry.is_string()) {
        n->key = entry.get<std::string>();
        return true;
    }
    if (!entry.is_object()) {
        *badField = "entry";
        return false;
    }
    const auto perm = entry.find("permission");
    if (perm != entry.end()) {
        if (!perm->is_string()) {
            *badField = "permission";
            return false;
        }
        n->key = perm->get<std::string>();
        return ReadAttributes(entry, true, n, badField);
    }
    const Json* attrs = nullptr;
    if (!ReadOneKeyForm(entry, &n->key, &attrs)) {
        *badField = "entry";
        return false;
    }
    return ReadAttributes(*attrs, true, n, badField);
}

// Reads one `parents` entry into `n` as the node `group.<name>`; a parent's `value` is ignored,
// as LuckPerms ignores it.
bool ReadParentEntry(const Json& entry, Node* n, const char** badField) {
    std::string group;
    const Json* attrs = nullptr;
    if (entry.is_string()) {
        group = entry.get<std::string>();
    } else if (entry.is_object()) {
        const auto g = entry.find("group");
        if (g != entry.end()) {
            if (!g->is_string()) {
                *badField = "group";
                return false;
            }
            group = g->get<std::string>();
            attrs = &entry;
        } else if (!ReadOneKeyForm(entry, &group, &attrs)) {
            *badField = "entry";
            return false;
        }
    } else {
        *badField = "entry";
        return false;
    }
    n->key = "group." + group;
    return attrs == nullptr || ReadAttributes(*attrs, false, n, badField);
}

void ReadList(const Json& root, const char* field, const char* what, std::string_view stem,
              bool (*readEntry)(const Json&, Node*, const char**), std::vector<Node>* nodes,
              std::vector<std::string>* problems) {
    const auto it = root.find(field);
    if (it == root.end()) return;
    if (!it->is_array()) {
        problems->push_back(std::string(stem) + ": " + field + " is not a list");
        return;
    }
    size_t index = 0;
    for (const Json& entry : *it) {
        Node n;
        const char* bad = "entry";
        if (readEntry(entry, &n, &bad)) {
            nodes->push_back(std::move(n));
        } else {
            problems->push_back(std::string(stem) + ": " + what + " entry " + std::to_string(index) +
                                " refused (" + bad + ")");
        }
        ++index;
    }
}

bool IsNotFound(const std::error_code& ec) {
    return ec == std::errc::no_such_file_or_directory || ec == std::errc::not_a_directory;
}

// Every `.json` file of `dir` (the folder named `label`), as (lower-cased stem, path), sorted by
// stem. A missing directory gives none; a directory that cannot be read, a listing that fails
// part-way, and a file whose name is not ASCII each add a problem.
std::vector<std::pair<std::string, fs::path>> ListHolders(const fs::path& dir, const char* label,
                                                          LoadReport& report) {
    std::vector<std::pair<std::string, fs::path>> out;
    std::error_code ec;
    const bool isDir = fs::is_directory(dir, ec);
    if (ec) {
        if (!IsNotFound(ec)) report.problems.push_back(std::string(label) + ": could not read the folder");
        return out;
    }
    if (!isDir) return out;
    fs::directory_iterator it(dir, ec);
    for (; !ec && it != fs::directory_iterator(); it.increment(ec)) {
        const fs::path& p = it->path();
        std::string ext;
        if (!NarrowAscii(p.extension().wstring(), &ext) || Lowered(ext) != ".json") continue;
        std::error_code fileEc;
        const bool regular = it->is_regular_file(fileEc);
        if (fileEc) {
            std::string name;
            if (!NarrowAscii(p.filename().wstring(), &name)) name = "a file whose name is not ASCII";
            report.problems.push_back(std::string(label) + ": " + name + " unreadable");
            continue;
        }
        if (!regular) continue;
        std::string stem;
        if (!NarrowAscii(p.stem().wstring(), &stem)) {
            report.problems.push_back(std::string(label) + ": a file whose name is not ASCII, skipped");
            continue;
        }
        out.emplace_back(Lowered(stem), p);
    }
    if (ec) report.problems.push_back(std::string(label) + ": could not list the folder");
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    return out;
}

bool ReadFile(const fs::path& path, std::string* text) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    text->assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return !f.bad();
}

// A key as the log prints it: quoted, with `"`, `\` and the control bytes (< 0x20, 0x7F) escaped,
// so an empty key is visible and no byte splits the line.
std::string QuotedKey(const std::string& key) {
    static const char kHex[] = "0123456789abcdef";
    std::string out = "\"";
    for (const char c : key) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (c == '"' || c == '\\') {
            out.push_back('\\');
            out.push_back(c);
        } else if (u < 0x20 || u == 0x7F) {
            out += "\\x";
            out.push_back(kHex[u >> 4]);
            out.push_back(kHex[u & 0xF]);
        } else {
            out.push_back(c);
        }
    }
    out.push_back('"');
    return out;
}

void ReportRefusedNodes(const std::vector<size_t>& refused, const std::vector<Node>& nodes,
                        const std::string& stem, LoadReport& report) {
    for (size_t i : refused) {
        report.problems.push_back(stem + ": key " + QuotedKey(nodes[i].key) + " refused");
    }
}

}  // namespace

bool NarrowAscii(const std::wstring& name, std::string* out) {
    out->clear();
    for (const wchar_t c : name) {
        if (c > 0x7F) {
            out->clear();
            return false;
        }
        out->push_back(static_cast<char>(c));
    }
    return true;
}

bool ParseHolderText(std::string_view text, std::string_view stem, std::string* primaryGroup,
                     std::vector<Node>* nodes, std::vector<std::string>* problems) {
    const Json root = Json::parse(text.data(), text.data() + text.size(), nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        problems->push_back(std::string(stem) + ": not a JSON object");
        return false;
    }
    nodes->clear();
    *primaryGroup = std::string(kDefaultGroup);
    const auto pg = root.find("primaryGroup");
    if (pg != root.end()) {
        if (pg->is_string()) {
            *primaryGroup = pg->get<std::string>();
        } else {
            problems->push_back(std::string(stem) + ": primaryGroup is not a string");
        }
    }
    ReadList(root, "permissions", "permission", stem, &ReadPermissionEntry, nodes, problems);
    ReadList(root, "parents", "parent", stem, &ReadParentEntry, nodes, problems);
    return true;
}

bool LoadHolderText(std::string_view text, std::string_view stem, bool group, Model& m, LoadReport& report) {
    const std::string name(stem);
    std::string primary;
    std::vector<Node> nodes;
    if (!ParseHolderText(text, stem, &primary, &nodes, &report.problems)) return false;
    std::vector<size_t> refused;
    const bool loaded = group ? m.LoadGroup(name, nodes, &refused) : m.LoadUser(name, primary, nodes, &refused);
    if (!loaded) {
        report.problems.push_back(name + ": refused");
        return false;
    }
    ReportRefusedNodes(refused, nodes, name, report);
    if (group) {
        ++report.groups;
    } else {
        ++report.users;
        // The primary AS WRITTEN: LoadUser's default step has rewritten the stored one by now.
        if (m.FindGroup(primary) == nullptr) {
            report.problems.push_back(name + ": primary group " + QuotedKey(primary) + " does not exist");
        }
    }
    return true;
}

void ReportMissingGroups(const Model& m, std::string_view stem, bool group, LoadReport& report) {
    const Holder* holder = group ? m.FindGroup(stem) : m.FindUser(stem);
    if (holder == nullptr) return;
    const std::string name(stem);
    static constexpr std::string_view kGroupPrefix = "group.";
    for (const Node& n : holder->nodes.Nodes()) {
        // A false `group.<g>` node is no parent (Parents skips it), so a missing <g> is no problem.
        if (!n.value || n.key.compare(0, kGroupPrefix.size(), kGroupPrefix) != 0) continue;
        const std::string parent = n.key.substr(kGroupPrefix.size());
        if (m.FindGroup(parent) == nullptr) {
            report.problems.push_back(name + ": parent group " + QuotedKey(parent) + " does not exist");
        }
    }
}

bool ReadStoreTexts(const fs::path& dir, std::vector<HolderText>* texts, LoadReport* report) {
    texts->clear();
    std::error_code ec;
    const bool exists = fs::exists(dir, ec);
    if (ec) {
        if (IsNotFound(ec)) return true;
        report->problems.push_back("permissions: could not read the folder");
        return false;
    }
    if (!exists) return true;

    for (const bool groups : {true, false}) {
        for (const auto& [stem, path] : ListHolders(dir / (groups ? "groups" : "users"), groups ? "groups" : "users", *report)) {
            if (groups ? !IsValidGroupName(stem) : !IsPlayerIdStem(stem)) {
                report->problems.push_back(stem + (groups ? ": not a valid group name, file skipped"
                                                          : ": not a 32 hex player id, file skipped"));
                continue;
            }
            std::string text;
            if (!ReadFile(path, &text)) {
                report->problems.push_back(stem + ": unreadable");
                continue;
            }
            texts->push_back(HolderText{stem, groups, std::move(text)});
        }
    }
    return true;
}

LoadReport LoadHolders(const std::vector<HolderText>& texts, Model& m) {
    LoadReport report;
    std::vector<std::pair<std::string, bool>> loadedHolders;
    for (const bool groups : {true, false}) {
        for (const HolderText& t : texts) {
            if (t.group != groups) continue;
            if (LoadHolderText(t.text, t.stem, t.group, m, report)) loadedHolders.emplace_back(t.stem, t.group);
        }
    }
    for (const auto& [stem, groups] : loadedHolders) ReportMissingGroups(m, stem, groups, report);
    return report;
}

LoadReport LoadStore(const fs::path& dir, Model& m) {
    LoadReport report;
    std::vector<HolderText> texts;
    if (!ReadStoreTexts(dir, &texts, &report)) return report;
    LoadReport loaded = LoadHolders(texts, m);
    report.groups = loaded.groups;
    report.users = loaded.users;
    report.problems.insert(report.problems.end(), std::make_move_iterator(loaded.problems.begin()),
                           std::make_move_iterator(loaded.problems.end()));
    return report;
}

std::string SerializeHolder(const Holder& h, int64_t now, bool pruneExpired) {
    using OJson = nlohmann::ordered_json;
    OJson root = OJson::object();
    if (h.kind == HolderKind::User) root["primaryGroup"] = h.primaryGroup;
    OJson list = OJson::array();
    for (const Node& n : h.nodes.Nodes()) {
        if (pruneExpired && n.expiry != 0 && n.expiry < now) continue;
        if (n.value && n.expiry == 0 && n.contexts.Empty()) {
            list.push_back(n.key);
            continue;
        }
        OJson entry = OJson::object();
        entry["permission"] = n.key;
        if (!n.value) entry["value"] = false;
        if (n.expiry != 0) entry["expiry"] = n.expiry;
        if (!n.contexts.Empty()) {
            // The pairs are sorted by key then value, so one key's values are adjacent.
            OJson ctx = OJson::object();
            const auto& pairs = n.contexts.Pairs();
            for (size_t i = 0; i < pairs.size();) {
                size_t end = i + 1;
                while (end < pairs.size() && pairs[end].first == pairs[i].first) ++end;
                if (end - i == 1) {
                    ctx[pairs[i].first] = pairs[i].second;
                } else {
                    OJson values = OJson::array();
                    for (size_t j = i; j < end; ++j) values.push_back(pairs[j].second);
                    ctx[pairs[i].first] = std::move(values);
                }
                i = end;
            }
            entry["context"] = std::move(ctx);
        }
        list.push_back(std::move(entry));
    }
    root["permissions"] = std::move(list);
    return root.dump(2, ' ', false, OJson::error_handler_t::replace) + "\n";
}

bool IsDefaultUser(const Holder& user, int64_t now, bool ignoreExpired) {
    if (user.primaryGroup != kDefaultGroup) return false;
    const std::string defaultKey = std::string("group.") + std::string(kDefaultGroup);
    size_t counted = 0;
    for (const Node& n : user.nodes.Nodes()) {
        if (ignoreExpired && n.expiry != 0 && n.expiry < now) continue;
        if (n.key != defaultKey || !n.value || n.expiry != 0 || !n.contexts.Empty()) return false;
        ++counted;
    }
    return counted == 1;
}

bool IsDefaultGroup(const Holder& g) {
    return g.kind == HolderKind::Group && g.name == kDefaultGroup && g.nodes.Nodes().empty();
}

atomic_file::Result WriteHolderFile(const fs::path& dir, bool group, std::string_view stem, const std::string* text) {
    const fs::path folder = dir / (group ? L"groups" : L"users");
    const fs::path path = folder / (std::wstring(stem.begin(), stem.end()) + L".json");
    if (text == nullptr) {
        if (::DeleteFileW(path.c_str()) != 0) return atomic_file::Result{};
        const DWORD error = ::GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return atomic_file::Result{};
        return atomic_file::Result{atomic_file::Step::Move, error};
    }
    std::error_code ec;
    fs::create_directories(folder, ec);
    if (ec) return atomic_file::Result{atomic_file::Step::Open, static_cast<unsigned long>(ec.value())};
    return atomic_file::Write(path, *text, atomic_file::Mode::Replace, atomic_file::Sync::ToDisk);
}

bool ShouldLoad(const LoadReport& report) { return report.problems.empty(); }

}  // namespace coop::permissions
