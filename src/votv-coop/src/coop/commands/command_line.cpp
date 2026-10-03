// coop/commands/command_line.cpp -- SplitLine.
//
// Ported from LuckPerms' QuotedStringTokenizer (common/.../command/utils/QuotedStringTokenizer.java
// :62-102, MIT, THIRD-PARTY-NOTICES.md): a quote opens a word only at its start, runs to the next
// quote, and an unclosed one runs to the end -- never an error. Divergences (ours): a run of spaces
// separates two words once (LuckPerms makes an empty word of each extra space), and a closing quote
// ends its word wherever it is, so `"a b"c` is two words.

#include "coop/commands/command_line.h"

namespace coop::commands {

namespace {

// The byte length of the quote character starting at line[i]; 0 when it is not one.
size_t QuoteLen(std::string_view line, size_t i) {
    const unsigned char c = static_cast<unsigned char>(line[i]);
    if (c == '"') return 1;
    if (c == 0xE2 && i + 2 < line.size() &&
        static_cast<unsigned char>(line[i + 1]) == 0x80) {
        const unsigned char third = static_cast<unsigned char>(line[i + 2]);
        if (third == 0x9C || third == 0x9D) return 3;  // U+201C, U+201D
    }
    return 0;
}

char LowerAscii(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c; }

}  // namespace

bool EqualsAsciiNoCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (LowerAscii(a[i]) != LowerAscii(b[i])) return false;
    return true;
}

ParsedLine SplitLine(std::string_view line) {
    ParsedLine out;
    const size_t n = line.size();
    size_t i = 0;
    while (i < n) {
        while (i < n && line[i] == ' ') ++i;
        if (i >= n) break;
        const size_t begin = i;
        const size_t open = QuoteLen(line, i);
        if (open != 0) {
            const size_t start = i + open;
            size_t j = start;
            while (j < n && QuoteLen(line, j) == 0) ++j;
            out.words.emplace_back(line.substr(start, j - start));
            i = (j < n) ? j + QuoteLen(line, j) : n;
        } else {
            size_t j = i;
            while (j < n && line[j] != ' ') ++j;
            out.words.emplace_back(line.substr(i, j - i));
            i = j;
        }
        out.begins.push_back(begin);
    }
    return out;
}

}  // namespace coop::commands
