// coop/commands/action_source.cpp -- see coop/commands/action_source.h.

#include "coop/commands/action_source.h"

namespace coop::commands {

void FillSource(const Context& ctx, coop::permissions::Action& a) {
    a.sourceId = ctx.caller.playerId;
    a.sourceName = "Someone";
    for (const PlayerView& v : ctx.players) {
        if (v.slot == ctx.caller.slot) {
            a.sourceName = v.nick;
            break;
        }
    }
}

}  // namespace coop::commands
