// coop/commands/command_dispatcher.h -- one line to one verb.
//
// Engine-free (no ue_wrap include). The dispatcher is the ONE place a command's permission is
// asked: it splits the line, walks to the verb (an alias through its expansion), asks the
// caller's Policy, parses and resolves the arguments, calls the handler and collects its reply
// lines. A handler checks nothing itself (the one exception is /help, which lists what the caller
// may use). A command's qualifiers -- who may act on an offline player (and on an id the host has
// no record of, only the console), who cannot be acted on, who is told -- are checked here, before
// the handler; a handler receives their answers and never asks the permission system. MTA's shape: one command table, checked once at dispatch, the command's default
// passed into the check (reference/mtasa-blue/Server/mods/deathmatch/logic/CConsole.cpp:68-69).

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
    // A third party's answer, for a Notify qualifier. Null: nobody is notified.
    bool (*holds)(std::string_view playerId, std::string_view node, bool defaultGranted) = nullptr;
    // Whether `node` is set on the player or a group it inherits, a wildcard not counting, for an
    // Exempt qualifier. Null: an Exempt target is refused as "identity is not proved yet".
    bool (*isExplicit)(std::string_view playerId, std::string_view node) = nullptr;
    // Whether the host has any record of the player id, for a GateOffline qualifier: an offline
    // target the host never saw is refused to a caller that is not the console. Null: every id is
    // unseen.
    bool (*known)(std::string_view playerId) = nullptr;
};

// What a handler sees, for the handler call only. The caller, the spec, the players, the registry
// and the policy outlive no further than the call; a handler that defers work copies what it
// needs (slots, player ids, texts, the caller's slot and generation). One entry per `spec.args`
// index in each vector: `given[i]` is false for an absent optional argument; `targets[i]` is
// filled for Player / PlayerOrId / Players (an offline PlayerOrId target has `offline` set,
// `offlineId`, and no slots), `integers[i]` for Integer, `texts[i]` is the word (the raw
// remainder for Rest) for every kind. `notifySlots` is filled for a spec with a Notify qualifier:
// the seated slots, other than the caller's and the host's, that hold its node.
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
    std::vector<int> notifySlots;

    void Reply(std::string line) { replies.push_back(std::move(line)); }
};

// A refusal or a usage error sets no `ran`; its one reply line says why.
struct DispatchResult {
    bool ran = false;
    std::vector<std::string> replies;
};

// What an empty line (a bare `/`) is answered with: by Dispatch, and by a client, which never
// dispatches and shows it itself.
inline constexpr std::string_view kHelpHint = "Type /help for the commands.";

// `line` is the text after the `/`. Game thread on the host (its callers); pure here.
DispatchResult Dispatch(const Registry& reg, const Caller& caller, std::string_view line,
                        const std::vector<PlayerView>& players, const Policy& policy);

// Declares `multivoid.command.selector` and registers `/help`, the first command.
bool RegisterBuiltins(Registry& reg);

}  // namespace coop::commands
