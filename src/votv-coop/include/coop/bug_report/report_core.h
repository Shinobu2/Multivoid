// coop/bug_report/report_core.h -- the pure core of a bug report: the redactor that makes a log
// safe to leave the machine, the form's check, and the selftest over them. Engine-free; any
// thread. The bundle that streams files through the redactor is report_bundle.h.

#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace coop::bug_report {

// What the redactor knows about this machine and this reporter. A folder form is a byte string as
// a log may spell it (UTF-8, the ANSI code page, the 8.3 short name, each also with doubled
// backslashes); an empty list switches its rule off.
struct RedactContext {
    std::vector<std::string> localAppDataForms, profileForms;
    std::string selfPlayerId;  // 32 lowercase hex, or empty
    std::string selfKey;       // "gen:" + 64 lowercase hex, or empty
};

// One count per replacement, by class.
struct RedactCounts {
    unsigned profile = 0, players = 0, keys = 0, addresses = 0;
};

// Deterministic per bundle: a token means the same thing everywhere in one zip. Learn is pass 1
// over every line of every entry; Apply is pass 2, one line (without its '\n') at a time. The
// rules and their order are the file comment of report_redact.cpp. Not thread-safe: one worker
// owns one Redactor.
class Redactor {
public:
    explicit Redactor(RedactContext ctx);
    void Learn(std::string_view line);
    std::string Apply(std::string_view line);
    const RedactCounts& Counts() const { return counts_; }

private:
    struct Folder {
        std::string bytes;
        bool local;
    };
    bool MatchFolder(std::string_view line, size_t at, const Folder& f) const;
    std::string AddressToken(std::string_view value);
    std::string PlayerToken(std::string_view first8);
    std::string KeyToken(std::string_view first8);

    std::vector<Folder> folders_;               // local app data first, each list longest first
    std::array<bool, 256> startsRule_{};        // a byte some rule can begin with
    std::string selfPlayer8_, selfKey8_;        // the first 8 of the reporter's own id and key
    std::unordered_set<std::string> learned8_;  // the first 8 of every 32-hex id Learn saw
    std::unordered_map<std::string, int> players_, keys_, addresses_;
    RedactCounts counts_;
};

// The form the player fills in.
struct Form {
    std::string happened, expected, contact;
};
inline constexpr size_t kHappenedMinChars = 20, kFieldMaxBytes = 4000, kContactMaxBytes = 128;

// nullptr = the form is fine; else the sentence the pane shows.
const char* ValidateForm(const Form& f);

// The report.txt body.
std::string ReportText(const Form& f);

// line 2 of a log, a trailing '\r' removed, is what this build writes there
// (ue_wrap::log::kLogFormatLine): the marks in an older log cannot be trusted.
bool IsReadableLogFormat(std::string_view line2);

// The un-gated boot selftest, run once per session start on both peers (shape:
// commands::RunSelftest). Red arm: the dev row selftest_break_bug_report makes one check compare
// against a wrong expectation, so the run must fail.
bool RunSelftest();

}  // namespace coop::bug_report
