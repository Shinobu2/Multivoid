// coop/bug_report/report_bundle.h -- the bug-report bundle: a zip of the logs, the ini's report
// form and the player's own words, redacted on the way in, written next to the game as
// <game folder>\multivoid_reports\report-<UTC yyyymmdd-hhmmss>.zip. Nothing is sent.
//
// Request is called from any thread; the part that reads game state runs on the game thread, the
// part that reads, redacts and zips files on ONE detached worker that touches no UObject. Each
// entry streams in kChunkBytes chunks, twice: a first pass teaches the Redactor every player id,
// a second applies it line by line into a temporary file that miniz deflates, so memory stays at
// a chunk and miniz's buffers whatever the log's size.

#pragma once

#include "coop/bug_report/report_core.h"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace coop::bug_report {

enum class Phase { Idle, Building, Done, Failed };

struct Status {
    Phase phase = Phase::Idle;
    std::wstring zipPath;  // Done: the finished zip
    std::string error;     // Failed: the sentence the pane shows
    uint64_t zipBytes = 0;
    RedactCounts counts;
};

// One file the bundle may hold.
struct Entry {
    const char* name;           // the zip entry name
    std::wstring path;
    bool tailOnly;              // UE4SS.log: its last kUe4ssTailBytes (the whole file when smaller)
    std::string leftOut;        // empty = included; else why not
    uint64_t bytesOnDisk = 0;   // for the pane
};

// Entries the bundle makes, not files it copies.
inline constexpr const char* kMadeEntries[] = {"report.txt", "multivoid.ini", "meta.json"};
inline constexpr size_t kUe4ssTailBytes = 256u << 10, kChunkBytes = 1u << 20;

// The four file entries in zip order, each included or left out with its reason. Any thread.
std::vector<Entry> ListEntries();

// Starts a bundle. False for an invalid form or while one is being built. Any thread.
bool Request(Form form);

// A copy of the latest status. Any thread.
Status GetStatus();

// This machine's folders and the reporter's own ids, as the Redactor takes them. Any thread.
RedactContext ReadThisMachine(std::string selfPlayerId, std::string selfKey);

// What report_files.cpp gives report_bundle.cpp.
namespace detail {

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

}  // namespace detail

}  // namespace coop::bug_report
