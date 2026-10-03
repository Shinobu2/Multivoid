// coop/permissions/model.h -- the permission model: users and groups, each a Holder of nodes, with
// LuckPerms' default-group rule and the revisions a cache invalidates on. A user is named by its
// player id (32 lower-case hex, derived from the proved key), a group by a name IsValidGroupName
// accepts. Engine-free: no ue_wrap include, no logging. Not locked: one thread at a time uses a
// Model -- `permission_host` builds it on the TimelineThread at a host start and hands it over once
// through its hand-off slot; after the game thread adopts it, only the game thread reads it.
#pragma once

#include "coop/permissions/node.h"
#include "coop/permissions/node_map.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace coop::permissions {

enum class HolderKind : uint8_t { User, Group };

// `primaryGroup` (users only) is the STORED primary group, possibly empty.
struct Holder {
    HolderKind kind;
    std::string name;
    std::string primaryGroup;
    NodeMap nodes;
};

inline constexpr std::string_view kDefaultGroup = "default";

class Model {
public:
    // Creates group `default`.
    Model();

    // Names are lower-cased; an invalid name is refused. CreateGroup creates a missing group (false
    // if present). DeleteGroup refuses `default`, else removes the group; nodes naming it stay in
    // their holders and name no parent.
    bool CreateGroup(std::string_view name);
    bool DeleteGroup(std::string_view name);
    const Holder* FindGroup(std::string_view name) const;
    const Holder* FindUser(std::string_view playerId) const;

    // Sets (replacing the node of its identity) or removes a node; the key is normalised first and a
    // refused key changes nothing. True when the store changed. SetNode on a missing group is false
    // (groups are created explicitly); on a missing user it creates the user, holding the permanent
    // global `group.default` and the stored primary `default` before the node goes in. After an
    // UnsetNode that removed a node of a user, LuckPerms' default rule runs over the user's stored
    // global groups: none, and it gains `group.default` and the primary `default`; a stored primary
    // that is not among them becomes the first of them.
    bool SetNode(HolderKind kind, std::string_view name, Node n);
    bool UnsetNode(HolderKind kind, std::string_view name, Node n);

    // The load half of a store read at start. LoadGroup creates the group and LoadUser the user
    // (`default` always exists and takes the nodes); each node goes through SetNode's key
    // normalisation, and the index in `nodes` of every node the key rules refuse is appended to
    // `refused`. False ONLY for the holder: an invalid name or id, or one that exists, and then
    // nothing of it loads. LoadUser then runs the default step, so a user with no stored global
    // group gains `group.default`, and a stored primary that is not among the user's global groups
    // becomes the first of them. The stored primary can so differ from `primaryGroup` passed in: a
    // caller that checks the written primary does so on its own copy, not by reading it back.
    bool LoadGroup(std::string_view name, const std::vector<Node>& nodes, std::vector<size_t>* refused);
    bool LoadUser(std::string_view playerId, std::string_view primaryGroup, const std::vector<Node>& nodes,
                  std::vector<size_t>* refused);

    // False for a missing user or group, or when the primary was already that group.
    bool SetPrimaryGroup(std::string_view playerId, std::string_view group);

    // A change to a group (create, delete, a node that changed the store) bumps the groups
    // revision; a change to a user bumps that user's. 0 for a user the model does not have.
    uint64_t GroupsRevision() const { return groupsRevision_; }
    uint64_t UserRevision(std::string_view playerId) const;

    // The holder a player id with no user is answered as: an empty user run through the same
    // default step as a created user, so it holds `group.default` and the primary `default`.
    Holder MakeUnknownUser(std::string_view playerId) const;

private:
    // The one default step: creation, UnsetNode and MakeUnknownUser all run it.
    static void GiveDefaultIfNeeded(Holder& user);

    std::unordered_map<std::string, Holder> groups_, users_;
    std::unordered_map<std::string, uint64_t> userRevisions_;
    uint64_t groupsRevision_ = 1;
};

}  // namespace coop::permissions
