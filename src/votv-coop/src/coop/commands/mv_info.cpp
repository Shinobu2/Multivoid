// coop/commands/mv_info.cpp -- the lines of `/mv ... info` and `/mv listgroups`: the STORED holder of
// the live model, never a resolved answer. Shapes: LuckPerms' UserInfo / GroupInfo (Message.java:
// 3188-3328: a header, the parent groups, the permissions) and ListGroups (ListGroups.java:55-72),
// reduced to what a chat line carries (MIT, THIRD-PARTY-NOTICES.md, LuckPerms).

#include "coop/commands/mv_commands.h"

#include "coop/permissions/inheritance.h"
#include "coop/permissions/model.h"
#include "coop/permissions/node.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace coop::commands::mv {
namespace {

namespace perm = coop::permissions;

// The node lines of `info` and the group lines of `listgroups` stop here, then `... and N more`.
constexpr size_t kLineCap = 20;

// A node's marks, each only when it applies: ` = false`, ` (until <expiry s>)` or ` (expired)`,
// ` [<k>=<v>, <k>=<v>]` (the contexts in ContextSet::Pairs() order).
std::string Marks(const perm::Node& n, int64_t now) {
    std::string out;
    if (!n.value) out += " = false";
    if (n.expiry != 0) out += n.expiry < now ? " (expired)" : " (until " + std::to_string(n.expiry) + ")";
    if (!n.contexts.Empty()) {
        out += " [";
        bool first = true;
        for (const auto& pair : n.contexts.Pairs()) {
            if (!first) out += ", ";
            first = false;
            out += pair.first + "=" + pair.second;
        }
        out += "]";
    }
    return out;
}

// `lines` then, past the cap, only the first kLineCap and the count of the rest.
void AppendCapped(std::vector<std::string>& out, std::vector<std::string> lines) {
    const size_t shown = std::min(lines.size(), kLineCap);
    for (size_t i = 0; i < shown; ++i) out.push_back(std::move(lines[i]));
    if (lines.size() > kLineCap) out.push_back("... and " + std::to_string(lines.size() - kLineCap) + " more");
}

// `weight <n>` for the highest weight that applies at `now`, else `no weight`.
std::string GroupWeightWords(const perm::Holder& g, int64_t now) {
    int weight = 0;
    return perm::CurrentWeight(g, now, &weight) ? "weight " + std::to_string(weight) : "no weight";
}

}  // namespace

std::vector<std::string> InfoLines(const perm::Model& m, const perm::HolderKey& key,
                                   const std::string& displayName, int64_t now) {
    const bool isUser = key.kind == perm::HolderKind::User;
    const perm::Holder* h = isUser ? m.FindUser(key.name) : m.FindGroup(key.name);
    if (h == nullptr) {
        if (isUser) return {displayName + " has nothing set: the default group applies."};
        return {"No group named " + key.name + "."};
    }

    std::vector<std::string> out;
    out.push_back(displayName + (isUser ? ": primary group " + h->primaryGroup : ": " + GroupWeightWords(*h, now)));

    // The parent groups, by group name (a stable sort keeps the NodeMap's order for one group).
    std::vector<const perm::Node*> parents;
    std::vector<std::string> others;
    for (const perm::Node& n : h->nodes.Nodes()) {
        const perm::NodeKind kind = perm::KindOf(n.key);
        if (kind == perm::NodeKind::Inheritance) parents.push_back(&n);
        else if (kind == perm::NodeKind::Permission) others.push_back(n.key + Marks(n, now));
    }
    std::stable_sort(parents.begin(), parents.end(), [](const perm::Node* a, const perm::Node* b) {
        return perm::GroupOf(a->key) < perm::GroupOf(b->key);
    });
    std::string line = "Parents: ";
    for (size_t i = 0; i < parents.size(); ++i) {
        if (i != 0) line += ", ";
        line += std::string(perm::GroupOf(parents[i]->key)) + Marks(*parents[i], now);
    }
    out.push_back(parents.empty() ? "Parents: none" : line);

    if (others.empty()) out.push_back("Nothing else is set.");
    else AppendCapped(out, std::move(others));
    return out;
}

std::vector<std::string> ListGroupLines(const perm::Model& m, int64_t now) {
    std::vector<std::string> lines;
    m.ForEachGroup([&](const perm::Holder& g) {
        int weight = 0;
        lines.push_back(perm::CurrentWeight(g, now, &weight) ? g.name + " (weight " + std::to_string(weight) + ")"
                                                              : g.name);
    });
    std::vector<std::string> out;
    AppendCapped(out, std::move(lines));
    return out;
}

}  // namespace coop::commands::mv
