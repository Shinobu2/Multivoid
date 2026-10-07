// coop/permissions/context_set.h -- the contexts a permission node or a subject carries: a set of
// lower-cased key/value pairs (`server=a`, `world=b`). A node applies when every key of its set is
// present in the subject's set with at least one equal value; the empty set applies everywhere.
// Engine-free: no ue_wrap include, no logging. Not locked: the owner thread of the Model uses it.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace coop::permissions {

class ContextSet {
public:
    // Adds key=value, both lowered (ASCII letters only). `server=global` and `world=global` are
    // dropped, and true is returned for them. False, nothing added, for an empty or all-space key or
    // value, or one holding a control byte (0x00-0x1F, 0x7F): a caller that gets false refuses the
    // whole node, since dropping the pair would widen the node's set to global.
    bool Add(std::string_view key, std::string_view value);

    bool Empty() const { return pairs_.empty(); }
    size_t Size() const { return pairs_.size(); }
    bool Contains(std::string_view key, std::string_view value) const;
    bool ContainsKey(std::string_view key) const;

    // The receiver is the NODE's set, the argument the subject's: an empty receiver is satisfied by
    // anything; else every key of the receiver has at least one of its values in `subject`. Extra
    // keys in `subject` are ignored.
    bool IsSatisfiedBy(const ContextSet& subject) const;

    // Sorted by key, then value; unique.
    const std::vector<std::pair<std::string, std::string>>& Pairs() const { return pairs_; }

    bool operator==(const ContextSet& other) const { return pairs_ == other.pairs_; }

private:
    std::vector<std::pair<std::string, std::string>> pairs_;
};

// LuckPerms' ascending ContextSetComparator: 0 when equal; else a non-empty set above the empty
// one, one with a `server` key above one without, one with a `world` key above one without, more
// pairs above fewer, then the sorted pairs compared one by one. Negative when `a` is LESS specific.
int CompareSpecificity(const ContextSet& a, const ContextSet& b);

}  // namespace coop::permissions
