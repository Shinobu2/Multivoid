// coop/permissions/node_map.h -- one holder's nodes, ported from LuckPerms' NodeMapMutable: one node
// per identity (same key, equal contexts, both temporary or both permanent), kept sorted so that
// the first node met for a key is the one flattening keeps. Engine-free: no ue_wrap include, no
// logging. Not locked: the Model's owner thread uses it.
#pragma once

#include "coop/permissions/node.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace coop::permissions {

class NodeMap {
public:
    // Replaces the node of the same identity, or inserts. True when anything changed, false when
    // the identical node was already there. The key is expected normalised (the Model does that).
    bool Set(const Node& n);
    // Removes the node of that identity (its value and expiry instant are ignored). True if one went.
    bool Unset(const Node& n);
    void Clear() { nodes_.clear(); }
    size_t Size() const { return nodes_.size(); }

    // Replaces `out` with the nodes that apply under `subject` at `now`, in store order.
    void Applicable(const ContextSet& subject, int64_t now, std::vector<const Node*>& out) const;

    // The smallest expiry among temporary nodes with `expiry >= now`; 0 when there is none.
    int64_t NextExpiry(int64_t now) const;

    // Store order: contexts most specific first; within equal contexts temporary before permanent,
    // temporaries by earlier expiry; then key ascending; then false before true.
    const std::vector<Node>& Nodes() const { return nodes_; }

private:
    std::vector<Node> nodes_;
};

}  // namespace coop::permissions
