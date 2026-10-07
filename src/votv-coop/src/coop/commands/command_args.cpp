// coop/commands/command_args.cpp -- the parsers of one argument word. See command_args.h.

#include "coop/commands/command_args.h"

#include <charconv>
#include <cstddef>
#include <limits>
#include <string>

namespace coop::commands {

namespace {

bool IsDigit(char c) { return c >= '0' && c <= '9'; }
bool IsAlpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
char Lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c; }

bool EqualsLower(std::string_view s, std::string_view lowerWord) {
    if (s.size() != lowerWord.size()) return false;
    for (size_t i = 0; i < s.size(); ++i)
        if (Lower(s[i]) != lowerWord[i]) return false;
    return true;
}

// The units in the order a duration writes them: the words that name each, and its seconds
// (Java's ChronoUnit estimates, LuckPerms' own).
struct UnitDef {
    const char* names[6];
    long long seconds;
};

constexpr UnitDef kUnits[] = {
    {{"y", "ys", "year", "years"}, 31556952},
    {{"mo", "mos", "month", "months"}, 2629746},
    {{"w", "ws", "week", "weeks"}, 604800},
    {{"d", "ds", "day", "days"}, 86400},
    {{"h", "hs", "hr", "hrs", "hour", "hours"}, 3600},
    {{"m", "ms", "min", "mins", "minute", "minutes"}, 60},
    {{"s", "ss", "sec", "secs", "second", "seconds"}, 1},
};
constexpr size_t kUnitCount = sizeof(kUnits) / sizeof(kUnits[0]);

// The index in kUnits that `letters` names, or -1.
int UnitOf(std::string_view letters) {
    for (size_t u = 0; u < kUnitCount; ++u)
        for (const char* name : kUnits[u].names)
            if (name != nullptr && EqualsLower(letters, name)) return static_cast<int>(u);
    return -1;
}

bool AllDigits(std::string_view s) {
    if (s.empty()) return false;
    for (char c : s)
        if (!IsDigit(c)) return false;
    return true;
}

// ASCII digits only, non-empty; false when the number does not fit a long long.
bool DigitsToNumber(std::string_view s, long long* out) {
    long long v = 0;
    for (char c : s) {
        const int d = c - '0';
        if (v > (std::numeric_limits<long long>::max() - d) / 10) return false;
        v = v * 10 + d;
    }
    *out = v;
    return true;
}

struct Group {
    std::string_view digits;
    int unit;
};

}  // namespace

// A leading `+` before a digit is skipped; from_chars reads a `-` itself and must take it all.
bool ParseInteger(std::string_view s, long long* out) {
    if (s.size() >= 2 && s[0] == '+' && s[1] >= '0' && s[1] <= '9') s.remove_prefix(1);
    long long v = 0;
    const char* end = s.data() + s.size();
    const auto r = std::from_chars(s.data(), end, v);
    if (r.ec != std::errc() || r.ptr != end) return false;
    *out = v;
    return true;
}

bool ParseBoolean(std::string_view s, bool* out) {
    if (EqualsLower(s, "true")) {
        *out = true;
        return true;
    }
    if (EqualsLower(s, "false")) {
        *out = false;
        return true;
    }
    return false;
}

DurationError ParseDuration(std::string_view s, long long nowSeconds, long long* expiry) {
    if (s.empty()) return DurationError::NotADuration;

    if (AllDigits(s)) {
        long long moment = 0;
        if (!DigitsToNumber(s, &moment)) return DurationError::TooFar;
        const long long most = nowSeconds > std::numeric_limits<long long>::max() - kMaxDurationSeconds
                                   ? std::numeric_limits<long long>::max()
                                   : nowSeconds + kMaxDurationSeconds;
        if (moment > most) return DurationError::TooFar;
        if (moment <= nowSeconds) return DurationError::Passed;
        *expiry = moment;
        return DurationError::None;
    }

    // The shape of the whole word first: groups of digits, a unit word, commas; the units in
    // order and each once, so there are at most kUnitCount groups.
    Group groups[kUnitCount];
    size_t count = 0;
    int lastUnit = -1;
    size_t i = 0;
    while (i < s.size()) {
        const size_t digitsAt = i;
        while (i < s.size() && IsDigit(s[i])) ++i;
        if (i == digitsAt) return DurationError::NotADuration;
        const size_t lettersAt = i;
        while (i < s.size() && IsAlpha(s[i])) ++i;
        const int unit = UnitOf(s.substr(lettersAt, i - lettersAt));
        if (unit < 0 || unit <= lastUnit) return DurationError::NotADuration;
        lastUnit = unit;
        groups[count++] = {s.substr(digitsAt, lettersAt - digitsAt), unit};
        while (i < s.size() && s[i] == ',') ++i;
    }

    long long total = 0;
    for (size_t g = 0; g < count; ++g) {
        long long n = 0;
        if (!DigitsToNumber(groups[g].digits, &n)) return DurationError::TooFar;
        const long long unitSeconds = kUnits[groups[g].unit].seconds;
        if (n > (kMaxDurationSeconds - total) / unitSeconds) return DurationError::TooFar;
        total += n * unitSeconds;
    }
    if (total == 0) return DurationError::Zero;
    *expiry = nowSeconds + total;
    return DurationError::None;
}

}  // namespace coop::commands
