// coop/commands/command_line.h -- the text after a chat line's `/`, split into words.
//
// Engine-free: no ue_wrap include, so the arbiter's own executable can link it.

#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace coop::commands {

struct ParsedLine {
    std::vector<std::string> words;
    // begins[i] is the byte offset in the split line where word i starts: its opening quote
    // for a quoted word. An argument that takes the rest of the line reads the raw text from here.
    std::vector<size_t> begins;
};

// A word that starts with a quote character (", or U+201C / U+201D as UTF-8) runs from after it
// to the next quote character (any of the three), the quotes dropped, spaces kept; with no
// closing quote it runs to the end of the line. Any other word runs to the next ASCII space and
// keeps a quote inside it as a byte. A run of spaces separates two words once; leading and
// trailing spaces make no word. Never fails; pure.
ParsedLine SplitLine(std::string_view line);

}  // namespace coop::commands
