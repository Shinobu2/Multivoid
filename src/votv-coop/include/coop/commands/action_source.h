// coop/commands/action_source.h -- who an admin action is recorded as.
//
// The one caller-to-source rule for an action-log record: the caller's proved player id and the
// nick the players list holds for its slot. A chat line that names the actor in a sentence keeps its
// own lookup; this is for the record.

#pragma once

#include "coop/commands/command_dispatcher.h"
#include "coop/permissions/action_log.h"

namespace coop::commands {

// Sets `a.sourceId` to the caller's proved id (empty when none is proved, as for the console) and
// `a.sourceName` to the nick of the player seated in the caller's slot, else "Someone".
void FillSource(const Context& ctx, coop::permissions::Action& a);

}  // namespace coop::commands
