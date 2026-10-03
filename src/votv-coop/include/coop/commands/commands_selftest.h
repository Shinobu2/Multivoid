// coop/commands/commands_selftest.h -- the commands' un-gated boot selftest, run at every session
// start on both peers: pinned lines through the tokenizer, pinned players through the resolver.

#pragma once

namespace coop::commands {

// Logs `commands selftest: ALL PASS (N checks)`, or one `commands selftest FAIL: <case>` per
// failure and `commands selftest: M/N checks passed`.
bool RunSelftest();

}  // namespace coop::commands
