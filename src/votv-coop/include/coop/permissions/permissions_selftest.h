// coop/permissions/permissions_selftest.h -- the permission model's selftest. RunSelftest is the
// boot runner (it logs and reads the red-arm row); each Run<Name>Cases function holds one concern's
// checks and needs nothing but a CheckSink (the files cases take a scratch folder for their disk
// case), so a process without the engine layer (the arbiter's own executable) compiles the cases
// and brings its own runner.
#pragma once

#include <filesystem>

namespace coop::permissions {

// Counts a check, and reports a failure through onFail. With breakFirst set, the first check made
// is inverted, so the run must report a failure (the red arm).
struct CheckSink {
    int total = 0;
    int passed = 0;
    bool breakFirst = false;
    void (*onFail)(const char* what) = nullptr;

    bool Check(bool ok, const char* what) {
        ++total;
        if (breakFirst && total == 1) ok = !ok;
        if (ok) {
            ++passed;
            return true;
        }
        if (onFail) onFail(what);
        return false;
    }
};

void RunContextCases(CheckSink& sink);
void RunStoreCases(CheckSink& sink);
void RunInheritanceCases(CheckSink& sink);
void RunResolutionCases(CheckSink& sink);
// `scratch` is an empty folder the cases may write under and delete from; the caller removes it.
void RunFilesCases(CheckSink& sink, const std::filesystem::path& scratch);
void RunEditCases(CheckSink& sink);
void RunGrantsCases(CheckSink& sink);

// Runs every cases function and logs the result line. True when every check passed.
bool RunSelftest();

}  // namespace coop::permissions
