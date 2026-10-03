// coop/permissions/node_map.cpp -- see coop/permissions/node_map.h.

#include "coop/permissions/node_map.h"

#include <algorithm>

namespace coop::permissions {
namespace {

bool IsTemporary(const Node& n) { return n.expiry != 0; }

// The identity of a node in the store: key, contexts, and temporary-or-permanent. Value and the
// expiry instant are not part of it.
bool SameIdentity(const Node& a, const Node& b) {
    return a.key == b.key && a.contexts == b.contexts && IsTemporary(a) == IsTemporary(b);
}

// Store order: true when `a` sorts before `b`.
bool Before(const Node& a, const Node& b) {
    const int c = CompareSpecificity(a.contexts, b.contexts);
    if (c != 0) return c > 0;
    if (IsTemporary(a) != IsTemporary(b)) return IsTemporary(a);
    if (IsTemporary(a) && a.expiry != b.expiry) return a.expiry < b.expiry;
    if (a.key != b.key) return a.key < b.key;
    return a.value < b.value;
}

}  // namespace

// Ported from LuckPerms common/src/main/java/me/lucko/luckperms/common/model/nodemap/NodeMapMutable.java:144-148,195-209 (MIT, THIRD-PARTY-NOTICES.md).
bool NodeMap::Set(const Node& n) {
    for (auto it = nodes_.begin(); it != nodes_.end(); ++it) {
        if (!SameIdentity(*it, n)) continue;
        if (it->value == n.value && it->expiry == n.expiry) return false;
        nodes_.erase(it);
        break;
    }
    nodes_.insert(std::lower_bound(nodes_.begin(), nodes_.end(), n, Before), n);
    return true;
}

// Ported from LuckPerms common/src/main/java/me/lucko/luckperms/common/model/nodemap/NodeMapMutable.java:195-209 (MIT, THIRD-PARTY-NOTICES.md).
bool NodeMap::Unset(const Node& n) {
    for (auto it = nodes_.begin(); it != nodes_.end(); ++it) {
        if (!SameIdentity(*it, n)) continue;
        nodes_.erase(it);
        return true;
    }
    return false;
}

// Ported from LuckPerms common/src/main/java/me/lucko/luckperms/common/model/nodemap/NodeMapBase.java:102-154 (MIT, THIRD-PARTY-NOTICES.md).
void NodeMap::Applicable(const ContextSet& subject, int64_t now, std::vector<const Node*>& out) const {
    out.clear();
    for (const Node& n : nodes_) {
        if (Applies(n, now) && n.contexts.IsSatisfiedBy(subject)) out.push_back(&n);
    }
}

int64_t NodeMap::NextExpiry(int64_t now) const {
    int64_t next = 0;
    for (const Node& n : nodes_) {
        if (n.expiry == 0 || n.expiry < now) continue;
        if (next == 0 || n.expiry < next) next = n.expiry;
    }
    return next;
}

}  // namespace coop::permissions
