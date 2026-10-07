// coop/permissions/model.cpp -- see coop/permissions/model.h.

#include "coop/permissions/model.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace coop::permissions {
namespace {

std::string Lowered(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    }
    return out;
}

bool IsPlayerId(std::string_view lowered) {
    if (lowered.size() != 32) return false;
    for (char c : lowered) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

Node DefaultGroupNode() {
    Node n;
    n.key = std::string("group.") + std::string(kDefaultGroup);
    return n;
}

}  // namespace

// Ported from LuckPerms common/src/main/java/me/lucko/luckperms/common/tasks/SyncTask.java:59-62 (MIT, THIRD-PARTY-NOTICES.md).
Model::Model() {
    groups_.emplace(std::string(kDefaultGroup),
                    Holder{HolderKind::Group, std::string(kDefaultGroup), std::string(), NodeMap()});
}

// Ported from LuckPerms common/src/main/java/me/lucko/luckperms/common/model/manager/user/AbstractUserManager.java:82-127 (MIT, THIRD-PARTY-NOTICES.md).
void Model::GiveDefaultIfNeeded(Holder& user) {
    // The user's stored global groups: its true `group.*` nodes with no context, temporary or not,
    // expired or not, the group existing or not, in store order.
    std::vector<std::string_view> globals;
    for (const Node& n : user.nodes.Nodes()) {
        if (n.value && n.contexts.Empty() && KindOf(n.key) == NodeKind::Inheritance) {
            globals.push_back(GroupOf(n.key));
        }
    }
    if (globals.empty()) {
        user.primaryGroup = std::string(kDefaultGroup);
        user.nodes.Set(DefaultGroupNode());
        return;
    }
    for (std::string_view g : globals) {
        if (g == user.primaryGroup) return;
    }
    user.primaryGroup = std::string(globals.front());
}

bool Model::CreateGroup(std::string_view name) {
    const std::string lowered = Lowered(name);
    if (!IsValidGroupName(lowered) || groups_.count(lowered) != 0) return false;
    groups_.emplace(lowered, Holder{HolderKind::Group, lowered, std::string(), NodeMap()});
    ++groupsRevision_;
    return true;
}

// Ported from LuckPerms common/src/main/java/me/lucko/luckperms/common/commands/group/DeleteGroup.java:78 (MIT, THIRD-PARTY-NOTICES.md).
bool Model::DeleteGroup(std::string_view name) {
    const std::string lowered = Lowered(name);
    if (lowered == kDefaultGroup) return false;
    if (groups_.erase(lowered) == 0) return false;
    ++groupsRevision_;
    return true;
}

const Holder* Model::FindGroup(std::string_view name) const {
    auto it = groups_.find(Lowered(name));
    return it == groups_.end() ? nullptr : &it->second;
}

const Holder* Model::FindUser(std::string_view playerId) const {
    auto it = users_.find(Lowered(playerId));
    return it == users_.end() ? nullptr : &it->second;
}

void Model::ForEachGroup(const std::function<void(const Holder&)>& fn) const {
    std::vector<const Holder*> ordered;
    ordered.reserve(groups_.size());
    for (const auto& entry : groups_) ordered.push_back(&entry.second);
    std::sort(ordered.begin(), ordered.end(), [](const Holder* a, const Holder* b) { return a->name < b->name; });
    for (const Holder* g : ordered) fn(*g);
}

bool Model::SetNode(HolderKind kind, std::string_view name, Node n) {
    std::string key;
    if (!NormalizeKey(n.key, &key)) return false;
    n.key = std::move(key);
    const std::string lowered = Lowered(name);
    if (kind == HolderKind::Group) {
        auto it = groups_.find(lowered);
        if (it == groups_.end()) return false;
        if (!it->second.nodes.Set(n)) return false;
        ++groupsRevision_;
        return true;
    }
    if (!IsPlayerId(lowered)) return false;
    auto it = users_.find(lowered);
    const bool created = it == users_.end();
    if (created) {
        Holder user{HolderKind::User, lowered, std::string(), NodeMap()};
        GiveDefaultIfNeeded(user);
        it = users_.emplace(lowered, std::move(user)).first;
    }
    const bool changed = it->second.nodes.Set(n);
    if (!created && !changed) return false;
    ++userRevisions_[lowered];
    return true;
}

bool Model::UnsetNode(HolderKind kind, std::string_view name, Node n) {
    std::string key;
    if (!NormalizeKey(n.key, &key)) return false;
    n.key = std::move(key);
    const std::string lowered = Lowered(name);
    if (kind == HolderKind::Group) {
        auto it = groups_.find(lowered);
        if (it == groups_.end() || !it->second.nodes.Unset(n)) return false;
        ++groupsRevision_;
        return true;
    }
    auto it = users_.find(lowered);
    if (it == users_.end() || !it->second.nodes.Unset(n)) return false;
    GiveDefaultIfNeeded(it->second);
    ++userRevisions_[lowered];
    return true;
}

bool Model::LoadGroup(std::string_view name, const std::vector<Node>& nodes, std::vector<size_t>* refused) {
    const std::string lowered = Lowered(name);
    if (lowered != kDefaultGroup && !CreateGroup(lowered)) return false;
    Holder& group = groups_.find(lowered)->second;
    for (size_t i = 0; i < nodes.size(); ++i) {
        Node n = nodes[i];
        std::string key;
        if (!NormalizeKey(n.key, &key)) {
            if (refused) refused->push_back(i);
            continue;
        }
        n.key = std::move(key);
        group.nodes.Set(n);
    }
    ++groupsRevision_;
    return true;
}

bool Model::LoadUser(std::string_view playerId, std::string_view primaryGroup, const std::vector<Node>& nodes,
                     std::vector<size_t>* refused) {
    const std::string lowered = Lowered(playerId);
    if (!IsPlayerId(lowered) || users_.count(lowered) != 0) return false;
    Holder user{HolderKind::User, lowered, Lowered(primaryGroup), NodeMap()};
    for (size_t i = 0; i < nodes.size(); ++i) {
        Node n = nodes[i];
        std::string key;
        if (!NormalizeKey(n.key, &key)) {
            if (refused) refused->push_back(i);
            continue;
        }
        n.key = std::move(key);
        user.nodes.Set(n);
    }
    GiveDefaultIfNeeded(user);
    users_.emplace(lowered, std::move(user));
    ++userRevisions_[lowered];
    return true;
}

bool Model::SetPrimaryGroup(std::string_view playerId, std::string_view group) {
    const std::string lowered = Lowered(playerId);
    auto it = users_.find(lowered);
    if (it == users_.end()) return false;
    const std::string groupName = Lowered(group);
    if (groups_.count(groupName) == 0 || it->second.primaryGroup == groupName) return false;
    it->second.primaryGroup = groupName;
    ++userRevisions_[lowered];
    return true;
}

uint64_t Model::UserRevision(std::string_view playerId) const {
    auto it = userRevisions_.find(Lowered(playerId));
    return it == userRevisions_.end() ? 0 : it->second;
}

Holder Model::MakeUnknownUser(std::string_view playerId) const {
    Holder user{HolderKind::User, Lowered(playerId), std::string(), NodeMap()};
    GiveDefaultIfNeeded(user);
    return user;
}

}  // namespace coop::permissions
