// coop/bug_report/report_stream.h -- the bundle's chunked line reader, which report_files.cpp
// defines and report_bundle.cpp uses. Internal to the bug_report folder: nothing else includes it.

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace coop::bug_report::detail {

struct ReadPlan {
    bool tailOnly = false;           // the last kUe4ssTailBytes, from the first '\n' inside them
    bool completeLinesOnly = false;  // a last line with no '\n' is not part of the entry (a live log)
};

// The bytes of a file one reading covered: pass 1 finds them, pass 2 reads exactly them, so a
// file that grew between the passes is the same text in both.
struct Span {
    uint64_t start = 0, bytes = 0;
};

using LineFn = std::function<void(std::string_view)>;

// Reads `path` through its own handle (shared with a writer) in kChunkBytes chunks and calls `fn`
// with each line, its '\n' removed (a CR stays), a line crossing a chunk carried whole. Pass 1
// (replay false) decides `span`; pass 2 (replay true) reads `span` as it stands. False when the
// file cannot be opened or read.
bool StreamLines(const std::wstring& path, const ReadPlan& plan, bool replay, Span& span,
                 const LineFn& fn);

}  // namespace coop::bug_report::detail
