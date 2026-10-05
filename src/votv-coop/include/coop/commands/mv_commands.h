// coop/commands/mv_commands.h -- `/mv`: the permission store edited in game, LuckPerms' `/lp` shape.
//
// A domain's command bindings, in moderation_commands' shape: this file registers the `mv` root into
// a Registry and the handlers call the permission host through the PORTS handed in, captured by
// value, so no file static holds them. The production registry is given the real ports
// (coop/permissions/permission_host, command_sync); the selftest gives its own Registry fakes that
// record what they receive. The dispatcher has already decided who may run a leaf and has checked its
// qualifiers (an offline target, who is told); a handler here never asks the permission system. A
// changing leaf hands the host ONE edit (a closure over a copy of the model, the holder it names, the
// action it logs) and sends every reply line the host composed; the host writes, publishes, logs and
// appends the action log, the handlers are engine-free and cannot. The reading leaves (`info`,
// `listgroups`) format the live model within one call; `reload` asks the host.
//
// Engine-free like the model files: it reaches coop/permissions only for the model, its types and its
// pure helpers (key and group-name checks, node kinds), and the host only through the ports.

#pragma once

#include "coop/commands/command_dispatcher.h"
#include "coop/commands/command_registry.h"
#include "coop/commands/command_targets.h"
#include "coop/permissions/permission_host.h"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace coop::commands::mv {

struct Ports {
    // A running hosted session: every leaf acts only inside one.
    bool (*hosted)() = nullptr;
    // One edit of one holder: the host's Apply.
    coop::permissions::host::ApplyResult (*apply)(const coop::permissions::HolderKey&,
        const std::function<bool(coop::permissions::Model&, std::string*)>&, bool callerIsOwner,
        const std::vector<std::string>& nodes, const coop::permissions::Action&) = nullptr;
    // The host's Reload; an empty answer means no hosted session.
    std::vector<std::string> (*reload)(const std::string& actorId) = nullptr;
    // The live model and whether it is the empty one of a store that did not load; read within one
    // handler call, never kept.
    const coop::permissions::Model& (*live)() = nullptr;
    bool (*storeBroken)() = nullptr;
    // The nick a seen-players record holds for an id, to name an offline player; empty when none.
    std::string (*recordNick)(std::string_view id) = nullptr;
    // The host holds kAdminLogNode: production `host::Allows(LocalGuid(), kAdminLogNode, false,
    // /*owner=*/true)` (the owner unless it denied itself), in command_sync.cpp's RealMvPorts.
    bool (*hostHoldsLog)() = nullptr;
    // `info`'s clock: the expired marks and the weight rule.
    int64_t (*now)() = nullptr;
    // One reply line to a caller, now or later (a seated player, or the host's own feed).
    void (*notify)(const Caller& to, std::string_view line) = nullptr;
};

// Registers the `mv` root into `reg`. False when the registry refuses it.
bool Register(Registry& reg, const Ports& p);

// The lines of `/mv user|group ... info`: the STORED holder `key` of `m` (what its file holds, never
// a resolved answer), named `displayName`, with `now` for the expired marks and the weight rule
// (mv_info.cpp). Pure; each line is one reply.
std::vector<std::string> InfoLines(const coop::permissions::Model& m, const coop::permissions::HolderKey& key,
                                   const std::string& displayName, int64_t now);

// The lines of `/mv listgroups`: one per group in name order, with its weight when it has one, at
// most twenty and then `... and N more` (mv_info.cpp). Pure.
std::vector<std::string> ListGroupLines(const coop::permissions::Model& m, int64_t now);

}  // namespace coop::commands::mv
