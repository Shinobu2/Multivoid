// coop/text/name_filter.h -- the one character filter of a displayed name: a player's nickname,
// a chat speaker's name, a thanks-list entry or title.
#pragma once

#include <cstdint>
#include <string>

namespace coop::text {

// The characters a displayed NAME may not contain: the C0 and C1 controls and DEL, an unpaired
// surrogate, the line and paragraph separators, and every Default_Ignorable codepoint. A name in a
// log line then cannot start a new line as the rig's reader splits it (`splitlines` breaks on
// U+0085, U+2028, U+2029), and a name cannot reorder or hide the text around it. A DENYLIST: an
// allowlist cannot survive a widening alphabet, because the script it forgot fails silently. The
// invisibles are the Default_Ignorable_Code_Point property rather than a hand-written list: one
// enumeration missed the combining grapheme joiner (advance 0 in both default families) and gave
// two identical-looking names distinct fold keys. The chat TEXT lane keeps its own, narrower list
// (`SanitizeUtf8`: a chat line may keep a TAB); the three line terminators are in both.
bool IsDeniedInName(uint32_t cp);

// The one character filter of a displayed name: a player's nickname, a chat speaker's name, a
// thanks-list entry or title. A nickname adds its cap and dash trims (`SanitizeNickname`); the
// thanks list adds its `Trim` and its byte cap; the speaker name takes the result as it is.
// In codepoints: a denied codepoint dropped, runs of spaces collapsed to one, leading spaces
// dropped, a combining mark at position 0 dropped.
std::wstring FilterNameChars(const std::wstring& raw);

// Asserted at boot beside the repertoire selftest: a wrong verdict here is a nickname that splits
// a log line, which no drill sees.
bool RunNameFilterSelftest();

}  // namespace coop::text
