// coop/permissions/inheritance.h -- LuckPerms' inheritance order: a group's weight, a user's
// effective primary group, the parents of a holder (sorted by weight, the primary group first) and
// the depth-first pre-order walk over them with a visited set. Engine-free: no ue_wrap include, no
// logging. Reads a Model on its owner thread.
#pragma once

#include "coop/permissions/model.h"

#include <cstdint>
#include <string_view>
#include <vector>

namespace coop::permissions {

// The largest value of the holder's `weight.<n>` nodes, whatever their contexts; an expired one does
// not count; 0 when there is none.
int WeightOf(const Holder& h, int64_t now);

// The same rule with "there is none" told apart: false when the holder has no `weight.<n>` node that
// applies at `now` (expiry 0 or not below `now`), else true with the largest `<n>` in `*out`.
bool CurrentWeight(const Holder& h, int64_t now, int* out);

// The user's primary group over its LIVE global groups at `now` (true `group.*` nodes with no
// context that apply at `now` and name an existing group, in store order): the stored primary if it
// is one of them, else the first of them, else `default`. Views into `user` or static storage.
std::string_view EffectivePrimary(const Model& m, const Holder& user, int64_t now);

// A stable sort: users before groups; higher weight first; then, only when `comparing` is a user,
// the group named EffectivePrimary(m, comparing, now) first.
void SortByInheritance(const Model& m, std::vector<const Holder*>& v, const Holder& comparing, int64_t now);

// Replaces `out` with the groups `h` inherits directly under `subject` at `now`: its applicable true
// inheritance nodes in store order whose group exists, each once; then, for a user that has stored
// global groups none of which is live (all lapsed or naming deleted groups), `default` unless
// already listed; then sorted by SortByInheritance with `h` as the comparing holder.
void Parents(const Model& m, const Holder& h, const ContextSet& subject, int64_t now,
             std::vector<const Holder*>& out);

// Replaces `out` with every holder `start` inherits, `start` excluded: depth-first pre-order, each
// holder once (a diamond is visited at its first reach, a cycle ends), children from Parents.
void InheritanceOrder(const Model& m, const Holder& start, const ContextSet& subject, int64_t now,
                      std::vector<const Holder*>& out);

}  // namespace coop::permissions
