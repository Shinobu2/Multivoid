// coop/commands/command_targets.cpp -- ResolveTarget and DescribeTargetError.
//
// Shapes: Source's UTIL_PlayerByCommandArg tries its id forms (`#userid`, the Steam id) before a
// name (reference/source-sdk-2013/src/game/shared/util_shared.cpp:1072-1140); LuckPerms resolves
// the `@` selectors of a command argument (bukkit/.../BukkitCommandExecutor.java:123-163). Ours,
// where we differ: an ambiguous name is an error that LISTS its matches, and `@p` is the nearest
// OTHER player (the caller has `@s`).

#include "coop/commands/command_targets.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include "coop/text/case_fold.h"
#include "coop/text/utf8_codec.h"

namespace coop::commands {

namespace {

using Candidates = std::vector<const PlayerView*>;

// The players by ascending slot: every result lists them in that order.
Candidates BySlot(const std::vector<PlayerView>& players) {
    Candidates out;
    out.reserve(players.size());
    for (const PlayerView& p : players) out.push_back(&p);
    std::sort(out.begin(), out.end(),
              [](const PlayerView* a, const PlayerView* b) { return a->slot < b->slot; });
    return out;
}

TargetResult Fail(TargetError e) {
    TargetResult r;
    r.error = e;
    return r;
}

// One candidate resolves; several are Ambiguous; none is NoMatch.
TargetResult FromMatches(const Candidates& m) {
    TargetResult r;
    if (m.empty()) {
        r.error = TargetError::NoMatch;
    } else if (m.size() > 1) {
        r.error = TargetError::Ambiguous;
        for (const PlayerView* p : m) r.matches.push_back(p->slot);
    } else {
        r.slots.push_back(m[0]->slot);
        r.playerIds.push_back(m[0]->playerId);
    }
    return r;
}

const PlayerView* Find(const Candidates& sorted, int slot) {
    for (const PlayerView* p : sorted)
        if (p->slot == slot) return p;
    return nullptr;
}

bool IsDigit(char c) { return c >= '0' && c <= '9'; }

bool IsHex(char c) {
    return IsDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

char LowerAscii(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c; }

// A nick or a word as a sequence of folded codepoints (a surrogate pair is one codepoint).
std::u32string Fold(std::string_view utf8) {
    std::u32string out;
    if (utf8.empty()) return out;
    const std::wstring w = coop::text::FromUtf8Lossy(utf8.data(), utf8.size());
    for (size_t i = 0; i < w.size();) {
        uint32_t cp = 0;
        const size_t units = coop::text::DecodeCodepoint(w, i, &cp);
        out.push_back(coop::text::CaseFold(cp));
        i += units != 0 ? units : 1;
    }
    return out;
}

TargetResult ResolveNearest(const Caller& caller, const Candidates& sorted) {
    if (caller.slot < 0) return Fail(TargetError::NoCaller);
    const PlayerView* self = Find(sorted, caller.slot);
    if (self == nullptr) return Fail(TargetError::NoMatch);
    if (!self->hasPosition) return Fail(TargetError::NoPosition);
    const PlayerView* best = nullptr;
    double bestD = 0;
    for (const PlayerView* p : sorted) {
        if (p->slot == caller.slot || !p->hasPosition) continue;
        const double dx = double(p->x) - double(self->x);
        const double dy = double(p->y) - double(self->y);
        const double dz = double(p->z) - double(self->z);
        const double d = dx * dx + dy * dy + dz * dz;
        if (best == nullptr || d < bestD) {  // ascending slot: a tie keeps the lower slot
            best = p;
            bestD = d;
        }
    }
    if (best == nullptr) return Fail(TargetError::NoMatch);
    return FromMatches({best});
}

// `#<n>`: 1 to 5 decimal digits, 1..65535 (the ledger's number is 16-bit; 0 is "none").
TargetResult ResolveNumber(std::string_view word, const Candidates& sorted) {
    const std::string_view digits = word.substr(1);
    if (digits.empty() || digits.size() > 5) return Fail(TargetError::NoMatch);
    unsigned n = 0;
    for (char c : digits) {
        if (!IsDigit(c)) return Fail(TargetError::NoMatch);
        n = n * 10 + static_cast<unsigned>(c - '0');
    }
    if (n == 0 || n > 65535) return Fail(TargetError::NoMatch);
    Candidates m;
    for (const PlayerView* p : sorted)
        if (p->playerNo == n) m.push_back(p);
    return FromMatches(m);
}

}  // namespace

TargetResult ResolveTarget(std::string_view word, bool one, const Caller& caller,
                           const std::vector<PlayerView>& players, int (*pick)(int count)) {
    if (word.empty()) return Fail(TargetError::NoMatch);
    const Candidates sorted = BySlot(players);

    if (word == "@s") {
        if (caller.slot < 0) return Fail(TargetError::NoCaller);
        const PlayerView* self = Find(sorted, caller.slot);
        if (self == nullptr) return Fail(TargetError::NoMatch);
        return FromMatches({self});
    }
    if (word == "@a") {
        if (sorted.empty()) return Fail(TargetError::NoMatch);
        if (one && sorted.size() > 1) {
            TargetResult r = Fail(TargetError::TooMany);
            r.usedSelector = true;
            return r;
        }
        TargetResult r;
        r.usedSelector = true;
        for (const PlayerView* p : sorted) {
            r.slots.push_back(p->slot);
            r.playerIds.push_back(p->playerId);
        }
        return r;
    }
    if (word == "@p") return ResolveNearest(caller, sorted);
    if (word == "@r") {
        const int n = static_cast<int>(sorted.size());
        if (n == 0 || pick == nullptr) return Fail(TargetError::NoMatch);
        const int at = pick(n);
        if (at < 0 || at >= n) return Fail(TargetError::NoMatch);
        TargetResult r = FromMatches({sorted[static_cast<size_t>(at)]});
        r.usedSelector = true;
        return r;
    }
    if (word[0] == '@') return Fail(TargetError::NoMatch);
    if (word[0] == '#') return ResolveNumber(word, sorted);

    // A player id: 8 to 32 hex digits, lowered first (ids are lower-case).
    if (word.size() >= 8 && word.size() <= 32 && std::all_of(word.begin(), word.end(), IsHex)) {
        std::string lowered(word);
        for (char& c : lowered) c = LowerAscii(c);
        Candidates m;
        for (const PlayerView* p : sorted)
            if (p->playerId.size() >= lowered.size() &&
                p->playerId.compare(0, lowered.size(), lowered) == 0)
                m.push_back(p);
        if (!m.empty()) return FromMatches(m);
    }

    const std::u32string needle = Fold(word);
    if (needle.empty()) return Fail(TargetError::NoMatch);  // a failed fold would match every nick
    std::vector<std::u32string> folded;
    folded.reserve(sorted.size());
    for (const PlayerView* p : sorted) folded.push_back(Fold(p->nick));

    Candidates exact;
    for (size_t i = 0; i < sorted.size(); ++i)
        if (folded[i] == needle) exact.push_back(sorted[i]);
    if (!exact.empty()) return FromMatches(exact);

    Candidates part;
    for (size_t i = 0; i < sorted.size(); ++i)
        if (folded[i].find(needle) != std::u32string::npos) part.push_back(sorted[i]);
    return FromMatches(part);
}

std::string DescribeTargetError(const TargetResult& r, std::string_view word,
                                const std::vector<PlayerView>& players) {
    const std::string w(word);
    switch (r.error) {
        case TargetError::None: return {};
        case TargetError::NoMatch: return "No player matches '" + w + "'.";
        case TargetError::Ambiguous: {
            std::string out = "'" + w + "' matches several players: ";
            bool first = true;
            for (int slot : r.matches) {
                for (const PlayerView& p : players) {
                    if (p.slot != slot) continue;
                    if (!first) out += ", ";
                    first = false;
                    out += p.nick + " (#" + std::to_string(p.playerNo) + ")";
                    break;
                }
            }
            return out + ".";
        }
        case TargetError::TooMany:
            return "'" + w + "' names more than one player; this command takes one.";
        case TargetError::NoCaller: return "'" + w + "' needs a player to run it.";
        case TargetError::NoPosition: return "'" + w + "' needs your position.";
    }
    return {};
}

}  // namespace coop::commands
