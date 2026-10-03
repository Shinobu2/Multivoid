// coop/permissions/context_set.cpp -- see coop/permissions/context_set.h.

#include "coop/permissions/context_set.h"

#include <algorithm>

namespace coop::permissions {
namespace {

char LowerAscii(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c; }

std::string Lowered(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = LowerAscii(c);
    return out;
}

// A key or value LuckPerms accepts, narrowed by the control-byte refusal: not empty, not only
// spaces, no control byte.
bool ValidPart(std::string_view s) {
    bool anyNonSpace = false;
    for (char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (c <= 0x1F || c == 0x7F) return false;
        if (c != ' ') anyNonSpace = true;
    }
    return anyNonSpace;
}

}  // namespace

// Ported from LuckPerms common/src/main/java/me/lucko/luckperms/common/context/ImmutableContextSetImpl.java:57,281 (MIT, THIRD-PARTY-NOTICES.md).
bool ContextSet::Add(std::string_view key, std::string_view value) {
    if (!ValidPart(key) || !ValidPart(value)) return false;
    std::pair<std::string, std::string> p{Lowered(key), Lowered(value)};
    if ((p.first == "server" || p.first == "world") && p.second == "global") return true;
    auto it = std::lower_bound(pairs_.begin(), pairs_.end(), p);
    if (it != pairs_.end() && *it == p) return true;
    pairs_.insert(it, std::move(p));
    return true;
}

bool ContextSet::Contains(std::string_view key, std::string_view value) const {
    for (const auto& p : pairs_) {
        if (p.first == key && p.second == value) return true;
    }
    return false;
}

bool ContextSet::ContainsKey(std::string_view key) const {
    for (const auto& p : pairs_) {
        if (p.first == key) return true;
    }
    return false;
}

// Ported from LuckPerms common/src/main/java/me/lucko/luckperms/common/context/ImmutableContextSetImpl.java:141-163 (MIT, THIRD-PARTY-NOTICES.md).
bool ContextSet::IsSatisfiedBy(const ContextSet& subject) const {
    size_t i = 0;
    while (i < pairs_.size()) {
        const std::string& key = pairs_[i].first;
        bool matched = false;
        for (; i < pairs_.size() && pairs_[i].first == key; ++i) {
            if (!matched && subject.Contains(key, pairs_[i].second)) matched = true;
        }
        if (!matched) return false;
    }
    return true;
}

// Ported from LuckPerms common/src/main/java/me/lucko/luckperms/common/context/comparator/ContextSetComparator.java:49-98 (MIT, THIRD-PARTY-NOTICES.md).
int CompareSpecificity(const ContextSet& a, const ContextSet& b) {
    if (a == b) return 0;
    auto cmp = [](bool x, bool y) { return static_cast<int>(x) - static_cast<int>(y); };
    int r = cmp(!a.Empty(), !b.Empty());
    if (r != 0) return r;
    r = cmp(a.ContainsKey("server"), b.ContainsKey("server"));
    if (r != 0) return r;
    r = cmp(a.ContainsKey("world"), b.ContainsKey("world"));
    if (r != 0) return r;
    if (a.Size() != b.Size()) return a.Size() < b.Size() ? -1 : 1;
    // Equal sizes and unequal sets: the sorted pairs differ at some index.
    const auto& pa = a.Pairs();
    const auto& pb = b.Pairs();
    for (size_t i = 0; i < pa.size(); ++i) {
        if (pa[i] == pb[i]) continue;
        return pa[i] < pb[i] ? -1 : 1;
    }
    return 0;
}

}  // namespace coop::permissions
