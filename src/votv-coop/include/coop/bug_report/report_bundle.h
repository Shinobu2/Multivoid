// coop/bug_report/report_bundle.h -- the bug-report bundle: a zip of the logs, the ini's report
// form and the player's own words, redacted on the way in, written next to the game as
// <game folder>\multivoid_reports\report-<UTC yyyymmdd-hhmmss>.zip. Nothing is sent.
//
// Request is called from any thread; the part that reads game state runs on the game thread, the
// part that reads, redacts and zips files on ONE detached worker that touches no UObject. Each
// entry streams in kChunkBytes chunks, twice: a first pass teaches the Redactor every player id,
// a second applies it line by line into a temporary file that miniz deflates. Memory is a chunk,
// the longest line (a line is carried whole, so a file with no '\n' is read whole), the
// Redactor's tables (one entry per distinct id, key and address) and miniz's buffers, whatever
// the log's size.

#pragma once

#include "coop/bug_report/report_core.h"

#include <cstdint>
#include <string>
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

}  // namespace coop::bug_report
