// coop/commands/command_args.h -- the parsers of one argument word: a whole number, true or false,
// a duration.
//
// Engine-free (no ue_wrap include), pure. The duration grammar is LuckPerms' `DurationParser`
// (reference/LuckPerms/common/src/main/java/me/lucko/luckperms/common/util/DurationParser.java:43-59)
// and its epoch-second form
// (reference/LuckPerms/common/src/main/java/me/lucko/luckperms/common/command/utils/ArgumentList.java:147-165),
// read as one word, so no whitespace in it. Inherited from LuckPerms on purpose: `ms` and a
// capital `M` are MINUTES (not milliseconds, not months), and a bare number is an epoch second, so
// `30` has already passed. LuckPerms parses a group's number as an `int`; ours is a `long long`
// bounded by the cap, so no group wraps.

#pragma once

#include <cstdint>
#include <string_view>

namespace coop::commands {

// A leading `+` before a digit is skipped; a `-` is read by the conversion itself, and the whole
// word must be consumed.
bool ParseInteger(std::string_view s, long long* out);

// `true` or `false`, ASCII case-insensitive; anything else is false and leaves *out alone.
bool ParseBoolean(std::string_view s, bool* out);

enum class DurationError : uint8_t {
    None,
    NotADuration,  // the word is not a duration at all (an optional Duration then leaves it)
    Zero,          // a duration of no time
    Passed,        // an absolute second that is not after now
    TooFar,        // more than kMaxDurationSeconds after now, or a number too large to hold
};

// A hundred years: the most a duration may reach past now.
inline constexpr long long kMaxDurationSeconds = 100LL * 31556952;

// A word of ASCII digits only (leading zeros allowed) is an absolute epoch second. Otherwise groups
// of digits then a unit word, in the order years, months, weeks, days, hours, minutes, seconds,
// each at most once, commas allowed after a group (`1d12h`, `1d,2h`): y ys year years; mo mos
// month months; w ws week weeks; d ds day days; h hs hr hrs hour hours; m ms min mins minute
// minutes; s ss sec secs second seconds, ASCII case-insensitive. A unit is as long as Java's
// ChronoUnit estimate (a year 31556952 s, a month 2629746 s). The SHAPE is judged over the whole
// word first, so `1000y1x` is NotADuration whatever its numbers. On None, *expiry is the absolute
// expiry in epoch seconds.
DurationError ParseDuration(std::string_view s, long long nowSeconds, long long* expiry);

}  // namespace coop::commands
