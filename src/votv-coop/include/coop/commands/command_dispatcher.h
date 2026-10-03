// coop/commands/command_dispatcher.h -- one line to one verb.
//
// Engine-free (no ue_wrap include). The dispatcher is the ONE place a command's permission is
// asked: it splits the line, walks to the verb (an alias through its expansion), asks the
// caller's Policy, parses and resolves the arguments, calls the handler and collects its reply
// lines. A handler checks nothing itself (the one exception is /help, which lists what the caller
// may use). MTA's shape: one command table, checked once at dispatch, the command's default
// passed into the check (CConsole.cpp:68-69).

#pragma once

#include "coop/commands/command_registry.h"
#include "coop/commands/command_targets.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace coop::commands {

// `check` answers whether `caller` may use `node`; `defaultGranted` is the node's declared
// default, for a holder with nothing set. `pick` is the one source of chance (`@r`).
using CheckFn = bool (*)(const Caller& caller, std::string_view node, bool defaultGranted);

struct Policy {
    CheckFn check = nullptr;  // null refuses everything: a policy that was never installed
    int (*pick)(int count) = nullptr;
};

// What a handler sees, for the handler call only. The caller, the spec, the players, the registry
// and the policy outlive no further than the call; a handler that defers work copies what it
// needs (slots, player ids, texts, the caller's slot and generation). One entry per `spec.args`
// index in each vector: `given[i]` is false for an absent optional argument; `targets[i]` is
// filled for Player / Players, `integers[i]` for Integer, `texts[i]` is the word (the raw
// remainder for Rest) for every kind.
struct Context {
    const Caller& caller;
    const CommandSpec& spec;
    const std::vector<PlayerView>& players;
    const Registry& registry;
    const Policy& policy;
    std::vector<TargetResult> targets;
    std::vector<long long> integers;
    std::vector<std::string> texts;
    std::vector<bool> given;
    std::vector<std::string> replies;

    void Reply(std::string line) { replies.push_back(std::move(line)); }
};

// A refusal or a usage error sets no `ran`; its one reply line says why.
struct DispatchResult {
    bool ran = false;
    std::vector<std::string> replies;
};

// `line` is the text after the `/`. Game thread on the host (its callers); pure here.
DispatchResult Dispatch(const Registry& reg, const Caller& caller, std::string_view line,
                        const std::vector<PlayerView>& players, const Policy& policy);

// Declares `multivoid.command.selector` and registers `/help`, the first command.
bool RegisterBuiltins(Registry& reg);

}  // namespace coop::commands
