// coop/permissions/inheritance.cpp -- see coop/permissions/inheritance.h.

#include "coop/permissions/inheritance.h"

#include <algorithm>
#include <unordered_set>

namespace coop::permissions {
namespace {

// A true inheritance node of `h` with no context: one of its global groups.
bool IsGlobalGroupNode(const Node& n) {
    return n.value && n.contexts.Empty() && KindOf(n.key) == NodeKind::Inheritance;
}

}  // namespace

// Ported from LuckPerms common/src/main/java/me/lucko/luckperms/common/model/WeightCache.java:49-69 (MIT, THIRD-PARTY-NOTICES.md).
int WeightOf(const Holder& h, int64_t now) {
    bool found = false;
    int best = 0;
    for (const Node& n : h.nodes.Nodes()) {
        int w = 0;
        if (!Applies(n, now) || !WeightValue(n.key, &w)) continue;
        if (!found || w > best) best = w;
        found = true;
    }
    return best;
}

// Ported from LuckPerms common/src/main/java/me/lucko/luckperms/common/model/manager/user/AbstractUserManager.java:81-111 (MIT, THIRD-PARTY-NOTICES.md).
std::string_view EffectivePrimary(const Model& m, const Holder& user, int64_t now) {
    std::string_view first;
    for (const Node& n : user.nodes.Nodes()) {
        if (!IsGlobalGroupNode(n) || !Applies(n, now)) continue;
        const std::string_view group = GroupOf(n.key);
        if (m.FindGroup(group) == nullptr) continue;
        if (group == user.primaryGroup) return group;
        if (first.empty()) first = group;
    }
    return first.empty() ? kDefaultGroup : first;
}

// Ported from LuckPerms common/src/main/java/me/lucko/luckperms/common/inheritance/InheritanceComparator.java:56-89 (MIT, THIRD-PARTY-NOTICES.md).
void SortByInheritance(const Model& m, std::vector<const Holder*>& v, const Holder& comparing, int64_t now) {
    struct Item {
        const Holder* holder;
        bool user;
        int weight;
        bool primary;
    };
    const bool byPrimary = comparing.kind == HolderKind::User;
    const std::string_view primary = byPrimary ? EffectivePrimary(m, comparing, now) : std::string_view();
    std::vector<Item> items;
    items.reserve(v.size());
    for (const Holder* h : v) {
        const bool user = h->kind == HolderKind::User;
        items.push_back({h, user, user ? 0 : WeightOf(*h, now), !user && byPrimary && h->name == primary});
    }
    std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
        if (a.user != b.user) return a.user;
        if (a.user) return false;
        if (a.weight != b.weight) return a.weight > b.weight;
        return a.primary && !b.primary;
    });
    for (size_t i = 0; i < items.size(); ++i) v[i] = items[i].holder;
}

// Ported from LuckPerms common/src/main/java/me/lucko/luckperms/common/inheritance/InheritanceGraph.java:59-71 (MIT, THIRD-PARTY-NOTICES.md).
void Parents(const Model& m, const Holder& h, const ContextSet& subject, int64_t now,
             std::vector<const Holder*>& out) {
    out.clear();
    std::vector<const Node*> applicable;
    h.nodes.Applicable(subject, now, applicable);
    for (const Node* n : applicable) {
        if (!n->value || KindOf(n->key) != NodeKind::Inheritance) continue;
        const Holder* g = m.FindGroup(GroupOf(n->key));
        if (g != nullptr && std::find(out.begin(), out.end(), g) == out.end()) out.push_back(g);
    }

    if (h.kind == HolderKind::User) {
        bool stored = false, live = false;
        for (const Node& n : h.nodes.Nodes()) {
            if (!IsGlobalGroupNode(n)) continue;
            stored = true;
            if (Applies(n, now) && m.FindGroup(GroupOf(n.key)) != nullptr) live = true;
        }
        if (stored && !live) {
            const Holder* def = m.FindGroup(kDefaultGroup);
            if (def != nullptr && std::find(out.begin(), out.end(), def) == out.end()) out.push_back(def);
        }
    }
    SortByInheritance(m, out, h, now);
}

// Written here, not ported: LuckPerms' depth-first pre-order is the textbook walk (its default,
// common/src/main/java/me/lucko/luckperms/common/config/ConfigKeys.java:350-358); its own
// traversal file carries Guava's Apache-2.0 code and is not used.
void InheritanceOrder(const Model& m, const Holder& start, const ContextSet& subject, int64_t now,
                      std::vector<const Holder*>& out) {
    out.clear();
    std::unordered_set<const Holder*> visited{&start};
    std::vector<const Holder*> stack, children;
    Parents(m, start, subject, now, children);
    stack.assign(children.rbegin(), children.rend());
    while (!stack.empty()) {
        const Holder* h = stack.back();
        stack.pop_back();
        if (!visited.insert(h).second) continue;
        out.push_back(h);
        Parents(m, *h, subject, now, children);
        stack.insert(stack.end(), children.rbegin(), children.rend());
    }
}

}  // namespace coop::permissions
