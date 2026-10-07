// coop/permissions/node.h -- one permission node and the key rules around it, ported from LuckPerms:
// a key (lower-cased, dotted), a value (false negates), an expiry (epoch seconds, 0 permanent) and
// the contexts it applies in. `group.<name>` is an inheritance node, `weight.<n>` a weight node,
// anything else a plain permission. Engine-free: no ue_wrap include, no logging.
#pragma once

#include "coop/permissions/context_set.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace coop::permissions {

struct Node {
    std::string key;
    bool value = true;
    int64_t expiry = 0;
    ContextSet contexts;
};

enum class NodeKind : uint8_t { Permission, Inheritance, Weight };

// A group name: 1..36 of [a-z0-9_+-], and not a Windows device name (a group becomes a file name).
// The one check every group name passes. Takes an already lowered name.
bool IsValidGroupName(std::string_view lowered);

// Lowers the key's ASCII letters into *out. False for an empty key, one over 200 bytes, one holding
// an ASCII space or a control byte (0x00-0x1F, 0x7F), or a `group.` key whose group name fails
// IsValidGroupName.
bool NormalizeKey(std::string_view in, std::string* out);

// Classifies a NORMALISED key: `group.<valid name>` is Inheritance, `weight.<n>` with n a whole
// signed decimal (a leading `+` before a digit is allowed) is Weight, anything else Permission.
NodeKind KindOf(std::string_view normalizedKey);

// The name after `group.` of an inheritance key.
std::string_view GroupOf(std::string_view inheritanceKey);

// The number of a weight key; false when the key is not a weight node.
bool WeightValue(std::string_view weightKey, int* out);

// `*`, or a key ending in `.*` and longer than two bytes (`.*` alone is a plain permission).
bool IsWildcard(std::string_view key);

// A node applies at `now` until its expiry second inclusive; a permanent node always applies.
bool Applies(const Node& n, int64_t now);

}  // namespace coop::permissions
