// coop/commands/commands_selftest.h -- the commands' un-gated boot selftest, run at every session
// start on both peers: pinned lines through the tokenizer, pinned players through the resolver, a
// test command tree through the registry and the dispatcher, the moderation commands over fake
// ports.

#pragma once

namespace coop::commands {

// Logs `commands selftest: ALL PASS (N checks)`, or one `commands selftest FAIL: <case>` per
// failure and `commands selftest: M/N checks passed`.
bool RunSelftest();

// The run's tally; the case files of this folder take it by reference.
struct Checker {
    int pass = 0, total = 0;
    void operator()(bool ok, const char* what);
};

// The moderation commands over fake ports (commands_cases_moderation.cpp), called by RunSelftest.
void ModerationCases(Checker& check);

}  // namespace coop::commands
