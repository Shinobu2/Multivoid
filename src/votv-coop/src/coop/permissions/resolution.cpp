// coop/permissions/resolution.cpp -- see coop/permissions/resolution.h.

#include "coop/permissions/resolution.h"


namespace coop::permissions {
namespace {

constexpr size_t kStackQueryBytes = 256;

char LowerAscii(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c; }

std::string HolderLabel(const Holder& h) {
    return (h.kind == HolderKind::User ? "user:" : "group:") + h.name;
}

// `start`'s applicable nodes, then those of each holder in `order`.
void Collect(const Holder& start, const std::vector<const Holder*>& order, const ContextSet& subject, int64_t now,
             std::vector<std::pair<const Holder*, const Node*>>& out) {
    out.clear();
    std::vector<const Node*> applicable;
    auto add = [&](const Holder& h) {
        h.nodes.Applicable(subject, now, applicable);
        for (const Node* n : applicable) out.emplace_back(&h, n);
    };
    add(start);
    for (const Holder* h : order) add(*h);
}

Decision FromEntry(const Resolved::Entry& e) {
    return Decision{e.value ? Tristate::True : Tristate::False, e.node, e.holder};
}

// DIRECT, then WILDCARD; the owner step is the caller's.
Decision RunChain(const Resolved& r, std::string_view query) {
    // Ported from LuckPerms common/src/main/java/me/lucko/luckperms/common/calculator/processor/DirectProcessor.java:43-45 (MIT, THIRD-PARTY-NOTICES.md).
    auto direct = r.flat.find(query);
    if (direct != r.flat.end()) return FromEntry(direct->second);

    // Ported from LuckPerms common/src/main/java/me/lucko/luckperms/common/calculator/processor/WildcardProcessor.java:52-71 (MIT, THIRD-PARTY-NOTICES.md).
    std::string_view node = query;
    while (true) {
        const size_t dot = node.rfind('.');
        if (dot == std::string_view::npos) break;
        node = node.substr(0, dot);
        if (node.empty()) continue;
        auto hit = r.wildcards.find(node);
        if (hit != r.wildcards.end()) return FromEntry(hit->second);
    }
    if (r.root) return FromEntry(*r.root);
    return Decision{};
}

}  // namespace

// Ported from LuckPerms common/src/main/java/me/lucko/luckperms/common/model/PermissionHolder.java:284-297 (MIT, THIRD-PARTY-NOTICES.md).
void ResolveInheritedNodes(const Model& m, const Holder& start, const ContextSet& subject, int64_t now,
                           std::vector<std::pair<const Holder*, const Node*>>& out) {
    std::vector<const Holder*> order;
    InheritanceOrder(m, start, subject, now, order);
    Collect(start, order, subject, now, out);
}

std::shared_ptr<const Resolved> Resolve(const Model& m, std::string_view playerId, const ContextSet& subject,
                                        int64_t now) {
    std::optional<Holder> unknown;
    const Holder* user = m.FindUser(playerId);
    if (user == nullptr) {
        unknown = m.MakeUnknownUser(playerId);
        user = &*unknown;
    }

    std::vector<const Holder*> order;
    InheritanceOrder(m, *user, subject, now, order);
    std::vector<std::pair<const Holder*, const Node*>> nodes;
    Collect(*user, order, subject, now, nodes);

    auto r = std::make_shared<Resolved>();
    const Holder* labelled = nullptr;
    std::string label;
    // Ported from LuckPerms common/src/main/java/me/lucko/luckperms/common/model/PermissionHolder.java:355-369 (MIT, THIRD-PARTY-NOTICES.md).
    for (const auto& [holder, node] : nodes) {
        if (holder != labelled) {
            labelled = holder;
            label = HolderLabel(*holder);
        }
        const Resolved::Entry entry{node->value, node->key, label};
        r->flat.try_emplace(node->key, entry);
        if (node->key == "*") {
            if (!r->root) r->root = entry;
        } else if (IsWildcard(node->key)) {
            r->wildcards.try_emplace(node->key.substr(0, node->key.size() - 2), entry);
        }
    }

    auto note = [&](const Holder& h) {
        const int64_t next = h.nodes.NextExpiry(now);
        if (next != 0 && (r->validUntil == 0 || next < r->validUntil)) r->validUntil = next;
    };
    note(*user);
    for (const Holder* h : order) note(*h);
    return r;
}

Decision Evaluate(const Resolved& r, std::string_view permission, bool owner) {
    // Ported from LuckPerms common/src/main/java/me/lucko/luckperms/common/calculator/PermissionCalculatorBase.java:60 (MIT, THIRD-PARTY-NOTICES.md).
    char stackBuf[kStackQueryBytes];
    std::string heapBuf;
    std::string_view query;
    if (permission.size() <= sizeof(stackBuf)) {
        for (size_t i = 0; i < permission.size(); ++i) stackBuf[i] = LowerAscii(permission[i]);
        query = std::string_view(stackBuf, permission.size());
    } else {
        heapBuf.assign(permission);
        for (char& c : heapBuf) c = LowerAscii(c);
        query = heapBuf;
    }

    Decision d = RunChain(r, query);
    // Ported from LuckPerms common/minecraft/src/main/java/me/lucko/luckperms/common/minecraft/calculator/ServerOwnerProcessor.java:36-48 (MIT, THIRD-PARTY-NOTICES.md).
    if (d.value == Tristate::Undefined && owner) {
        d.value = Tristate::True;
        d.holder = "owner";
    }
    return d;
}

bool IsSetExplicitly(const Resolved& r, std::string_view permission) {
    std::string lowered(permission);
    for (char& c : lowered) c = LowerAscii(c);
    auto it = r.flat.find(lowered);
    return it != r.flat.end() && it->second.value;
}

std::shared_ptr<const Resolved> Checker::Get(std::string_view playerId, const ContextSet& subject, int64_t now) {
    Key key{std::string(playerId), subject.Pairs()};
    for (char& c : key.playerId) c = LowerAscii(c);
    const uint64_t groups = model_.GroupsRevision();
    const uint64_t user = model_.UserRevision(key.playerId);
    auto it = entries_.find(key);
    if (it != entries_.end()) {
        const CacheEntry& e = it->second;
        const bool expired = e.resolved->validUntil != 0 && now > e.resolved->validUntil;
        if (e.groupsRevision == groups && e.userRevision == user && !expired) return e.resolved;
    }
    CacheEntry fresh{Resolve(model_, key.playerId, subject, now), groups, user};
    ++builds_;
    entries_[std::move(key)] = fresh;
    return fresh.resolved;
}

}  // namespace coop::permissions
