// coop/permissions/resolution.h -- a player's resolved permission answers. Resolve flattens the
// player's own nodes and those of every group it inherits (the first node met for a key wins) into
// an immutable Resolved; Evaluate runs LuckPerms' processor chain over it (the exact key, then the
// wildcards from the most specific cut, then the owner step) and reads nothing else, so any thread
// may call it. Checker caches Resolved per player and contexts on the owner thread and rebuilds at
// a model change or at the first Get past the earliest expiry it holds. Engine-free: no ue_wrap
// include, no logging, no clock (time is a parameter).
#pragma once

#include "coop/permissions/inheritance.h"
#include "coop/permissions/model.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace coop::permissions {

enum class Tristate : uint8_t { Undefined, False, True };

// The answer to one check and what decided it. `node` is the deciding node's key; `holder` labels
// the deciding holder by kind and name (a player id or a group name) or reads `owner`. Both are empty
// when Undefined, and `node` is empty for the owner step. The views point into the Resolved
// evaluated (or static storage) and are valid while it lives.
struct Decision {
    Tristate value = Tristate::Undefined;
    std::string_view node;
    std::string_view holder;
};

// Lets a std::string_view look a key up without building a string.
struct StringHash {
    using is_transparent = void;
    size_t operator()(std::string_view s) const noexcept { return std::hash<std::string_view>{}(s); }
};

// Immutable once built. `flat` holds every key (wildcard keys and `*` included) with the first node
// met for it; `wildcards` holds each `a.b.*` key under `a.b`; `root` is the `*` node. `validUntil`
// is the earliest expiry among the holders consulted (0 = none ahead): the answers hold while
// `now <= validUntil`.
struct Resolved {
    struct Entry {
        bool value;
        std::string node;
        std::string holder;
    };
    std::unordered_map<std::string, Entry, StringHash, std::equal_to<>> flat;
    std::unordered_map<std::string, Entry, StringHash, std::equal_to<>> wildcards;
    std::optional<Entry> root;
    int64_t validUntil = 0;
};

// `start`'s applicable nodes, then those of each holder of InheritanceOrder, each in store order.
void ResolveInheritedNodes(const Model& m, const Holder& start, const ContextSet& subject, int64_t now,
                           std::vector<std::pair<const Holder*, const Node*>>& out);

// The player's resolved answers; a player id with no user is answered as Model::MakeUnknownUser.
// Reads the model: owner thread only.
std::shared_ptr<const Resolved> Resolve(const Model& m, std::string_view playerId, const ContextSet& subject,
                                        int64_t now);

// Const and allocation-free for any query a declared node can be (up to 256 bytes). The declared
// default of a node is applied by the caller after this, not inside the chain. With `owner`, the
// answer a chain left undefined is True from `owner`.
Decision Evaluate(const Resolved& r, std::string_view permission, bool owner);

// True when `r.flat` holds `permission` itself with value true. A node set on the holder or a group
// it inherits, not one a wildcard implies: a wildcard key (`a.b.*`, `*`) is a different key and
// never counts. Essentials' own exempt checks go through isAuthorized, which honours wildcards; ours
// never lets a wildcard exempt.
bool IsSetExplicitly(const Resolved& r, std::string_view permission);

// The owner thread's cache: one Resolved per (player id, contexts), rebuilt when the groups'
// revision or that user's revision moved or `validUntil != 0 && now > validUntil`.
class Checker {
public:
    explicit Checker(const Model& m) : model_(m) {}
    std::shared_ptr<const Resolved> Get(std::string_view playerId, const ContextSet& subject, int64_t now);
    void Clear() { entries_.clear(); }
    size_t BuildCountForTest() const { return builds_; }

private:
    struct Key {
        std::string playerId;
        std::vector<std::pair<std::string, std::string>> subject;
        bool operator<(const Key& o) const {
            return playerId != o.playerId ? playerId < o.playerId : subject < o.subject;
        }
    };
    struct CacheEntry {
        std::shared_ptr<const Resolved> resolved;
        uint64_t groupsRevision = 0;
        uint64_t userRevision = 0;
    };

    const Model& model_;
    std::map<Key, CacheEntry> entries_;
    size_t builds_ = 0;
};

}  // namespace coop::permissions
