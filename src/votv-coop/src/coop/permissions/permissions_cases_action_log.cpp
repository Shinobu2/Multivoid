// coop/permissions/permissions_cases_action_log.cpp -- the checks of the writer in
// coop/permissions/action_log.h, in the runner's scratch folder: a missing folder made and one line
// written byte-exact for a given timestamp, a second call appending a second, a folder that cannot
// be made refused.

#include "coop/permissions/action_log.h"
#include "coop/permissions/permissions_selftest.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>

namespace coop::permissions {
namespace {

namespace fs = std::filesystem;

std::string Id(char c) { return std::string(32, c); }

// The whole file as bytes; empty when it cannot be opened.
std::string ReadAll(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void WriterCases(CheckSink& sink, const fs::path& scratch) {
    // The runner has already failed a check when it had no folder to give.
    if (scratch.empty()) return;
    const fs::path root = scratch / "action_log";
    const fs::path folder = root / "a";

    Action act;
    act.sourceId = Id('a');
    act.sourceName = "Host";
    act.targetType = "user";
    act.targetId = Id('b');
    act.targetName = "Bob";
    act.description = "/kick " + Id('b') + " spamming";

    const std::string first = ActionJson(act, 1700000000) + "\n";
    const std::string second = ActionJson(act, 1700000001) + "\n";
    const fs::path file = folder / kActionLogFileName;

    sink.Check(AppendAction(folder, act, 1700000000) && ReadAll(file) == first,
               "action log: creates the folder");
    sink.Check(AppendAction(folder, act, 1700000001) && ReadAll(file) == first + second,
               "action log: appends");

    // A FILE where the folder's parent should be: the folder cannot be made.
    const fs::path blocker = root / "blocker";
    {
        std::ofstream b(blocker, std::ios::binary | std::ios::trunc);
        b << "x";
    }
    sink.Check(!AppendAction(blocker / "sub", act, 1700000000),
               "action log: refuses a blocked folder");
}

}  // namespace

void RunActionLogCases(CheckSink& sink, const std::filesystem::path& scratch) { WriterCases(sink, scratch); }

}  // namespace coop::permissions
