// coop/permissions/node.cpp -- see coop/permissions/node.h.

#include "coop/permissions/node.h"

#include <charconv>
#include <utility>

namespace coop::permissions {
namespace {

constexpr size_t kMaxKeyBytes = 200;
constexpr size_t kMaxGroupNameBytes = 36;
constexpr std::string_view kGroupPrefix = "group.";
constexpr std::string_view kWeightPrefix = "weight.";

bool IsDigit(char c) { return c >= '0' && c <= '9'; }

// Windows reserves these as file names whatever the extension; a group name becomes a file name.
bool IsDeviceName(std::string_view n) {
    if (n == "con" || n == "prn" || n == "aux" || n == "nul") return true;
    if (n.size() == 4 && (n.substr(0, 3) == "com" || n.substr(0, 3) == "lpt")) return n[3] >= '1' && n[3] <= '9';
    return false;
}

}  // namespace

// Ported from LuckPerms common/src/main/java/me/lucko/luckperms/common/storage/misc/DataConstraints.java:37,51 (MIT, THIRD-PARTY-NOTICES.md).
bool IsValidGroupName(std::string_view lowered) {
    if (lowered.empty() || lowered.size() > kMaxGroupNameBytes) return false;
    for (char c : lowered) {
        const bool ok = (c >= 'a' && c <= 'z') || IsDigit(c) || c == '_' || c == '+' || c == '-';
        if (!ok) return false;
    }
    return !IsDeviceName(lowered);
}

// Ported from LuckPerms common/src/main/java/me/lucko/luckperms/common/storage/misc/DataConstraints.java:34,45 (MIT, THIRD-PARTY-NOTICES.md).
bool NormalizeKey(std::string_view in, std::string* out) {
    if (in.empty() || in.size() > kMaxKeyBytes) return false;
    std::string lowered(in);
    for (char& ch : lowered) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (c <= 0x20 || c == 0x7F) return false;
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch + ('a' - 'A'));
    }
    if (std::string_view(lowered).substr(0, kGroupPrefix.size()) == kGroupPrefix &&
        !IsValidGroupName(std::string_view(lowered).substr(kGroupPrefix.size()))) {
        return false;
    }
    *out = std::move(lowered);
    return true;
}

// Ported from LuckPerms common/src/main/java/me/lucko/luckperms/common/node/types/Weight.java:72-83 (MIT, THIRD-PARTY-NOTICES.md).
bool WeightValue(std::string_view weightKey, int* out) {
    if (weightKey.substr(0, kWeightPrefix.size()) != kWeightPrefix) return false;
    std::string_view num = weightKey.substr(kWeightPrefix.size());
    // Java's Integer.parseInt reads a leading `+`; std::from_chars does not.
    if (num.size() > 1 && num[0] == '+' && IsDigit(num[1])) num.remove_prefix(1);
    int v = 0;
    const char* end = num.data() + num.size();
    const auto r = std::from_chars(num.data(), end, v);
    if (num.empty() || r.ec != std::errc() || r.ptr != end) return false;
    if (out) *out = v;
    return true;
}

// Ported from LuckPerms common/src/main/java/me/lucko/luckperms/common/node/factory/NodeBuilders.java:52-71 (MIT, THIRD-PARTY-NOTICES.md).
NodeKind KindOf(std::string_view normalizedKey) {
    if (normalizedKey.substr(0, kGroupPrefix.size()) == kGroupPrefix &&
        IsValidGroupName(normalizedKey.substr(kGroupPrefix.size()))) {
        return NodeKind::Inheritance;
    }
    if (WeightValue(normalizedKey, nullptr)) return NodeKind::Weight;
    return NodeKind::Permission;
}

std::string_view GroupOf(std::string_view inheritanceKey) {
    return inheritanceKey.substr(kGroupPrefix.size());
}

// Ported from LuckPerms common/src/main/java/me/lucko/luckperms/common/calculator/processor/WildcardProcessor.java:85 (MIT, THIRD-PARTY-NOTICES.md).
bool IsWildcard(std::string_view key) {
    if (key == "*") return true;
    return key.size() > 2 && key.substr(key.size() - 2) == ".*";
}

// Ported from LuckPerms common/src/main/java/me/lucko/luckperms/common/node/AbstractNode.java:116-118 (MIT, THIRD-PARTY-NOTICES.md).
bool Applies(const Node& n, int64_t now) { return n.expiry == 0 || now <= n.expiry; }

}  // namespace coop::permissions
