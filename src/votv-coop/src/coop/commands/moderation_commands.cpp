// coop/commands/moderation_commands.cpp -- see coop/commands/moderation_commands.h.
//
// Every handler answers "There is no hosted session." first when the ports say none runs. A
// result a root does not map to a line of its own (a result the guarded path does not expect, or
// /tphere's Failed) is answered "Could not <verb> <who> (<result>)." so a report can be read.

#include "coop/commands/moderation_commands.h"

#include "coop/commands/command_dispatcher.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>

namespace coop::commands::moderation {
namespace {

using coop::moderation::ModResult;
using coop::moderation::PlayerToken;
using coop::moderation::TokenFor;

std::string ShortId(const std::string& id) { return id.substr(0, 8); }

const char* ResultName(ModResult r) {
    switch (r) {
        case ModResult::Done: return "Done";
        case ModResult::NoSession: return "NoSession";
        case ModResult::Gone: return "Gone";
        case ModResult::NoId: return "NoId";
        case ModResult::NotBanned: return "NotBanned";
        case ModResult::Failed: return "Failed";
    }
    return "?";
}

std::string Unmapped(const char* verb, const std::string& who, ModResult r) {
    return std::string("Could not ") + verb + " " + who + " (" + ResultName(r) + ").";
}

const PlayerView* ViewOf(const std::vector<PlayerView>& players, int slot) {
    for (const PlayerView& v : players)
        if (v.slot == slot) return &v;
    return nullptr;
}

PlayerToken TokenOf(const PlayerView& v) {
    return TokenFor(v.slot, static_cast<uint16_t>(v.playerNo), v.generation);
}

// The player the argument at `arg` resolved to (the dispatcher resolved it against the same
// list), or null.
const PlayerView* TargetView(const Context& ctx, size_t arg) {
    const TargetResult& t = ctx.targets[arg];
    return t.slots.empty() ? nullptr : ViewOf(ctx.players, t.slots[0]);
}

// The typed reason, empty when none was given.
std::string ReasonText(const Context& ctx, size_t arg) {
    return ctx.given[arg] ? ctx.texts[arg] : std::string();
}

// "<caller> kicked|banned <target>[: <reason>]" to every player the dispatcher listed as holding
// the notify node, and to the host's own feed when a client acted (the host's reply is its own).
void NotifyOthers(const Ports& p, const Context& ctx, const char* verb, const std::string& target,
                  const std::string& reason) {
    const PlayerView* caller = ViewOf(ctx.players, ctx.caller.slot);
    std::string line = (caller != nullptr ? caller->nick : std::string("Someone")) + " " + verb + " " + target;
    if (!reason.empty()) line += ": " + reason;
    for (const PlayerView& v : ctx.players) {
        if (std::find(ctx.notifySlots.begin(), ctx.notifySlots.end(), v.slot) == ctx.notifySlots.end())
            continue;
        Caller to;
        to.slot = v.slot;
        to.generation = v.generation;
        p.notify(to, line);
    }
    if (ctx.caller.slot > 0) {
        Caller host;
        host.slot = 0;
        host.isOperator = true;
        p.notify(host, line);
    }
}

Handler KickHandler(const Ports& p) {
    return [p](Context& ctx) {
        if (!p.hosted()) { ctx.Reply(kNoSession); return; }
        const PlayerView* v = TargetView(ctx, 0);
        if (v == nullptr) { ctx.Reply("Could not find that player."); return; }
        const std::string reason = ReasonText(ctx, 1);
        const ModResult r = p.kick(TokenOf(*v), reason.c_str());
        switch (r) {
            case ModResult::Done:
                ctx.Reply("Kicked " + v->nick + ".");
                NotifyOthers(p, ctx, "kicked", v->nick, reason);
                break;
            case ModResult::Gone: ctx.Reply(v->nick + " already left."); break;
            case ModResult::NoSession: ctx.Reply(kNoSession); break;
            default: ctx.Reply(Unmapped("kick", v->nick, r)); break;
        }
    };
}

// /ban bans the id and the real address; /banid the id only.
Handler BanHandler(const Ports& p, bool byAddress) {
    return [p, byAddress](Context& ctx) {
        if (!p.hosted()) { ctx.Reply(kNoSession); return; }
        const TargetResult& t = ctx.targets[0];
        const std::string reason = ReasonText(ctx, 1);
        if (t.offline) {
            const std::string recorded = p.recordNick(t.offlineId);
            const std::string who = recorded.empty() ? ShortId(t.offlineId) : recorded;
            const ModResult r = p.banOffline(t.offlineId.c_str(), reason.c_str(), byAddress);
            switch (r) {
                case ModResult::Done:
                    ctx.Reply("Banned " + who + " (offline).");
                    NotifyOthers(p, ctx, "banned", who, reason);
                    break;
                case ModResult::NoSession: ctx.Reply(kNoSession); break;
                default: ctx.Reply(Unmapped("ban", who, r)); break;
            }
            return;
        }
        const PlayerView* v = TargetView(ctx, 0);
        if (v == nullptr) { ctx.Reply("Could not find that player."); return; }
        const ModResult r = p.ban(TokenOf(*v), reason.c_str(), byAddress);
        switch (r) {
            case ModResult::Done:
                ctx.Reply("Banned " + v->nick + " (" + ShortId(v->playerId) + ").");
                NotifyOthers(p, ctx, "banned", v->nick, reason);
                break;
            case ModResult::NoId: ctx.Reply(v->nick + "'s identity is not proved yet."); break;
            case ModResult::Gone: ctx.Reply(v->nick + " already left."); break;
            case ModResult::NoSession: ctx.Reply(kNoSession); break;
            default: ctx.Reply(Unmapped("ban", v->nick, r)); break;
        }
    };
}

bool IsHexPrefix(const std::string& s) {
    if (s.size() < 8 || s.size() > 32) return false;
    return std::all_of(s.begin(), s.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

Handler UnbanHandler(const Ports& p) {
    return [p](Context& ctx) {
        if (!p.hosted()) { ctx.Reply(kNoSession); return; }
        std::string word = ctx.texts[0];
        for (char& c : word)
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
        if (!IsHexPrefix(word)) { ctx.Reply("Give 8 to 32 characters of a player id."); return; }
        const std::vector<std::string> ids = p.idsWithPrefix(word);
        if (ids.empty()) { ctx.Reply("No ban matches " + word + "."); return; }
        if (ids.size() > 1) {
            ctx.Reply(std::to_string(ids.size()) + " bans match " + word + "; give more of the id.");
            return;
        }
        const std::string id8 = ShortId(ids[0]);
        const ModResult r = p.unban(ids[0].c_str());
        switch (r) {
            case ModResult::Done: ctx.Reply("Unbanned " + id8 + "."); break;
            case ModResult::NotBanned: ctx.Reply(id8 + " is not banned."); break;
            case ModResult::NoSession: ctx.Reply(kNoSession); break;
            default: ctx.Reply(Unmapped("unban", id8, r)); break;
        }
    };
}

Handler TeleportHandler(const Ports& p) {
    return [p](Context& ctx) {
        if (!p.hosted()) { ctx.Reply(kNoSession); return; }
        const PlayerView* v = TargetView(ctx, 0);
        if (v == nullptr) { ctx.Reply("Could not find that player."); return; }
        if (!v->worldReady) { ctx.Reply(v->nick + " is still joining."); return; }
        const ModResult r = p.teleport(TokenOf(*v));
        switch (r) {
            case ModResult::Done: ctx.Reply("Teleported " + v->nick + " to you."); break;
            case ModResult::Gone: ctx.Reply(v->nick + " already left."); break;
            case ModResult::NoSession: ctx.Reply(kNoSession); break;
            default: ctx.Reply(Unmapped("teleport", v->nick, r)); break;
        }
    };
}

CommandSpec KickSpec(const Ports& p) {
    CommandSpec c;
    c.name = "kick";
    c.description = "Disconnects a player.";
    c.pastTense = "kicked";
    c.args = {{"who", ArgKind::Player, false, true}, {"reason", ArgKind::Rest, true}};
    c.qualifiers = {{"exempt", QualKind::Exempt}, {"notify", QualKind::Notify}};
    c.handler = KickHandler(p);
    return c;
}

// /ban and /banid share the node `multivoid.ban` and its three qualifiers; /ban registers first.
CommandSpec BanSpec(const Ports& p, bool byAddress) {
    CommandSpec c;
    c.name = byAddress ? "ban" : "banid";
    c.description = byAddress ? "Bans a player by id and, on a direct link, by address."
                              : "Bans a player by id only.";
    if (!byAddress) c.nodeOf = "multivoid.ban";
    c.pastTense = "banned";
    c.args = {{"who", ArgKind::PlayerOrId, false, true}, {"reason", ArgKind::Rest, true}};
    c.qualifiers = {{"offline", QualKind::GateOffline},
                    {"exempt", QualKind::Exempt},
                    {"notify", QualKind::Notify}};
    c.handler = BanHandler(p, byAddress);
    return c;
}

CommandSpec UnbanSpec(const Ports& p) {
    CommandSpec c;
    c.name = "unban";
    c.description = "Lifts a ban.";
    c.args = {{"id", ArgKind::Word, false}};
    c.handler = UnbanHandler(p);
    return c;
}

// The teleport carries no destination and always brings the target to the host, so for a client
// caller "here" would be a lie: the dispatcher refuses any caller but the console.
CommandSpec TeleportSpec(const Ports& p) {
    CommandSpec c;
    c.name = "tphere";
    c.description = "Brings a player to you (the host).";
    c.pastTense = "teleported";
    c.consoleOnly = true;
    c.args = {{"who", ArgKind::Player, false, true}};
    c.handler = TeleportHandler(p);
    return c;
}

}  // namespace

bool Register(Registry& reg, const Ports& p) {
    return reg.Register(KickSpec(p), nullptr) && reg.Register(BanSpec(p, true), nullptr) &&
           reg.Register(BanSpec(p, false), nullptr) && reg.Register(UnbanSpec(p), nullptr) &&
           reg.Register(TeleportSpec(p), nullptr);
}

}  // namespace coop::commands::moderation
