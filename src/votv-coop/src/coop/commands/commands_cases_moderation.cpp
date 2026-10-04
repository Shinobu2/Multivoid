// coop/commands/commands_cases_moderation.cpp -- the cases of coop/commands/moderation_commands.h:
// each root's replies and the arguments its verb receives, over fake ports that record into
// statics local to this file (the production registry never holds a domain's bindings in one).
// Called from RunSelftest.

#include "coop/commands/command_dispatcher.h"
#include "coop/commands/command_registry.h"
#include "coop/commands/command_targets.h"
#include "coop/commands/commands_selftest.h"
#include "coop/commands/moderation_commands.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace coop::commands {
namespace {

using coop::moderation::ModResult;
using coop::moderation::PlayerToken;
using Ports = moderation::Ports;
using Told = std::pair<int, std::string>;

std::string Repeat(const char* s, int times) {
    std::string out;
    for (int i = 0; i < times; ++i) out += s;
    return out;
}

// What one set of fake ports received and what it answers.
struct Rec {
    bool hosted = true;
    ModResult result = ModResult::Done;
    int calls = 0;
    std::string verb;
    PlayerToken token;
    std::string id;
    std::string reason;
    bool byAddress = false;
    std::vector<std::string> prefixIds;
    std::string prefixAsked;
    std::string nick;  // what recordNick answers
    bool known = false;  // what the policy's known answers (Fakes<1> only)
    std::vector<Told> told;
    std::vector<uint32_t> toldGeneration;
};

// Two sets of ports with separate records: `Id` is only a name for the statics.
template <int Id>
struct Fakes {
    static Rec& R() {
        static Rec rec;
        return rec;
    }
    static ModResult Kick(const PlayerToken& t, const char* reason) {
        R().calls++; R().verb = "kick"; R().token = t; R().reason = reason;
        return R().result;
    }
    static ModResult Ban(const PlayerToken& t, const char* reason, bool byAddress) {
        R().calls++; R().verb = "ban"; R().token = t; R().reason = reason; R().byAddress = byAddress;
        return R().result;
    }
    static ModResult BanOffline(const char* id, const char* reason, bool byAddress) {
        R().calls++; R().verb = "banOffline"; R().id = id; R().reason = reason; R().byAddress = byAddress;
        return R().result;
    }
    static ModResult Unban(const char* id) {
        R().calls++; R().verb = "unban"; R().id = id;
        return R().result;
    }
    static ModResult Teleport(const PlayerToken& t) {
        R().calls++; R().verb = "teleport"; R().token = t;
        return R().result;
    }
    static bool Hosted() { return R().hosted; }
    static std::vector<std::string> IdsWithPrefix(std::string_view prefix) {
        R().prefixAsked = std::string(prefix);
        return R().prefixIds;
    }
    static std::string RecordNick(std::string_view) { return R().nick; }
    static bool Known(std::string_view) { return R().known; }
    static void Notify(const Caller& to, std::string_view line) {
        R().told.emplace_back(to.slot, std::string(line));
        R().toldGeneration.push_back(to.generation);
    }
    static Ports MakePorts() {
        Ports p;
        p.kick = &Kick;
        p.ban = &Ban;
        p.banOffline = &BanOffline;
        p.unban = &Unban;
        p.teleport = &Teleport;
        p.hosted = &Hosted;
        p.idsWithPrefix = &IdsWithPrefix;
        p.recordNick = &RecordNick;
        p.notify = &Notify;
        return p;
    }
};

PlayerView Seat(int slot, unsigned no, const char* nick, const std::string& id, uint32_t generation,
                bool worldReady) {
    PlayerView v;
    v.slot = slot;
    v.playerNo = no;
    v.nick = nick;
    v.playerId = id;
    v.generation = generation;
    v.worldReady = worldReady;
    return v;
}

// The host, Bob (a client), Cy (still joining) and Dee.
std::vector<PlayerView> Seated() {
    return {Seat(0, 1, "Host", Repeat("a", 32), 0, true), Seat(1, 2, "Bob", Repeat("b", 32), 7, true),
            Seat(2, 5, "Cy", Repeat("c", 32), 9, false), Seat(3, 6, "Dee", Repeat("d", 32), 4, true)};
}

bool AllowAll(const Caller&, std::string_view, bool) { return true; }
// Only Dee holds a notify node.
bool HoldsDee(std::string_view id, std::string_view, bool) { return id == Repeat("d", 32); }
bool ExplicitNobody(std::string_view, std::string_view) { return false; }
bool ExplicitCy(std::string_view id, std::string_view) { return id == Repeat("c", 32); }

Policy MakePolicy() {
    Policy p;
    p.check = &AllowAll;
    p.holds = &HoldsDee;
    p.isExplicit = &ExplicitNobody;
    p.known = &Fakes<1>::Known;
    return p;
}

template <int Id>
DispatchResult Run(const Registry& reg, const Caller& who, const char* line) {
    Fakes<Id>::R() = Rec{};
    return Dispatch(reg, who, line, Seated(), MakePolicy());
}

bool Said(const DispatchResult& r, const char* line) {
    return r.ran && r.replies.size() == 1 && r.replies[0] == line;
}

bool Refused(const DispatchResult& r, const char* line) {
    return !r.ran && r.replies.size() == 1 && r.replies[0] == line;
}

bool TokenIs(const PlayerToken& t, int slot, unsigned no, uint32_t generation) {
    return t.slot == slot && t.playerNo == no && t.generation == generation;
}

// Run `line` with the fake answering `result`.
template <int Id>
DispatchResult RunAs(const Registry& reg, const Caller& who, const char* line, ModResult result) {
    Fakes<Id>::R() = Rec{};
    Fakes<Id>::R().result = result;
    return Dispatch(reg, who, line, Seated(), MakePolicy());
}

void KickCases(Checker& check, const Registry& reg, const Caller& console, const Caller& bob) {
    using F = Fakes<1>;
    check(Said(Run<1>(reg, console, "kick Cy lol"), "Kicked Cy.") && F::R().verb == "kick" &&
              TokenIs(F::R().token, 2, 5, 9) && F::R().reason == "lol",
          "moderation: /kick calls the verb with the target's token and the typed reason");
    check(F::R().told == std::vector<Told>{{3, "Host kicked Cy: lol"}} &&
              F::R().toldGeneration == std::vector<uint32_t>{4},
          "moderation: /kick tells the players holding notify, with a reason, at their generation");
    check(Said(Run<1>(reg, console, "kick Cy"), "Kicked Cy.") && F::R().reason.empty() &&
              F::R().told == std::vector<Told>{{3, "Host kicked Cy"}},
          "moderation: /kick without a reason passes no reason and tells the holders without one");
    check(Said(RunAs<1>(reg, console, "kick Cy", ModResult::Gone), "Cy already left.") &&
              F::R().told.empty(),
          "moderation: /kick of a player who left says so and tells nobody");
    check(Said(RunAs<1>(reg, console, "kick Cy", ModResult::NoSession), "There is no hosted session."),
          "moderation: /kick answers NoSession");
    check(Said(RunAs<1>(reg, console, "kick Cy", ModResult::NoId), "Could not kick Cy (NoId)."),
          "moderation: /kick names a result it does not map");
    check(Said(Run<1>(reg, bob, "kick Cy"), "Kicked Cy.") &&
              F::R().told == std::vector<Told>{{3, "Bob kicked Cy"}, {0, "Bob kicked Cy"}},
          "moderation: a client's kick is told to the holders and then to the host's own feed");
    {
        Caller stranger;
        stranger.slot = 9;
        check(Said(Run<1>(reg, stranger, "kick Cy"), "Kicked Cy.") &&
                  F::R().told == std::vector<Told>{{3, "Someone kicked Cy"}, {0, "Someone kicked Cy"}},
              "moderation: a caller absent from the players is named Someone");
    }
    check(Refused(Run<1>(reg, console, "kick Host"), "That is the host -- it cannot be kicked.") &&
              F::R().calls == 0,
          "moderation: /kick of the host is refused before the verb");
    {
        Policy exempt = MakePolicy();
        exempt.isExplicit = &ExplicitCy;
        F::R() = Rec{};
        const DispatchResult r = Dispatch(reg, bob, "kick Cy", Seated(), exempt);
        check(Refused(r, "Cy cannot be kicked.") && F::R().calls == 0,
              "moderation: an exempt target is refused before the verb");
    }
}

void BanCases(Checker& check, const Registry& reg, const Caller& console, const Caller& bob) {
    using F = Fakes<1>;
    check(Said(Run<1>(reg, console, "ban Cy griefing"), "Banned Cy (cccccccc).") &&
              F::R().verb == "ban" && TokenIs(F::R().token, 2, 5, 9) && F::R().reason == "griefing" &&
              F::R().byAddress && F::R().told == std::vector<Told>{{3, "Host banned Cy: griefing"}},
          "moderation: /ban bans by id and address and tells the holders");
    check(Said(Run<1>(reg, console, "banid Cy"), "Banned Cy (cccccccc).") && F::R().verb == "ban" &&
              !F::R().byAddress && F::R().reason.empty(),
          "moderation: /banid bans by id only");
    check(Said(RunAs<1>(reg, console, "ban Cy", ModResult::NoId), "Cy's identity is not proved yet.") &&
              F::R().told.empty(),
          "moderation: /ban of a player with no proved id says so");
    check(Said(RunAs<1>(reg, console, "ban Cy", ModResult::Gone), "Cy already left."),
          "moderation: /ban of a player who left says so");
    check(Said(RunAs<1>(reg, console, "ban Cy", ModResult::NoSession), "There is no hosted session."),
          "moderation: /ban answers NoSession");
    check(Said(RunAs<1>(reg, console, "ban Cy", ModResult::NotBanned), "Could not ban Cy (NotBanned)."),
          "moderation: /ban names a result it does not map");
    check(Refused(Run<1>(reg, console, "ban Host"), "That is the host -- it cannot be banned."),
          "moderation: /ban of the host is refused");

    const std::string eve = Repeat("e", 32);
    {
        Fakes<1>::R() = Rec{};
        F::R().nick = "Eve";
        const DispatchResult r = Dispatch(reg, console, ("ban " + eve + " spam").c_str(), Seated(), MakePolicy());
        check(Said(r, "Banned Eve (offline).") && F::R().verb == "banOffline" && F::R().id == eve &&
                  F::R().reason == "spam" && F::R().byAddress &&
                  F::R().told == std::vector<Told>{{3, "Host banned Eve: spam"}},
              "moderation: /ban of an unseated id bans it offline, by the record's nick");
    }
    check(Said(Run<1>(reg, console, ("banid " + eve).c_str()), "Banned eeeeeeee (offline).") &&
              !F::R().byAddress && F::R().told == std::vector<Told>{{3, "Host banned eeeeeeee"}},
          "moderation: /banid of an unseated id with no record names it by its first eight");
    check(Refused(Run<1>(reg, bob, ("ban " + eve).c_str()),
                  "eeeeeeee has never played here; only the host can act on an unknown id.") &&
              F::R().calls == 0 && F::R().told.empty(),
          "moderation: the dispatcher refuses a client's offline /ban of an id with no record");
    check(Refused(Run<1>(reg, bob, ("banid " + eve).c_str()),
                  "eeeeeeee has never played here; only the host can act on an unknown id.") &&
              F::R().calls == 0,
          "moderation: the dispatcher refuses a client's offline /banid of an id with no record");
    {
        Fakes<1>::R() = Rec{};
        F::R().known = true;
        F::R().nick = "Eve";
        const DispatchResult r = Dispatch(reg, bob, ("ban " + eve).c_str(), Seated(), MakePolicy());
        check(Said(r, "Banned Eve (offline).") && F::R().verb == "banOffline" && F::R().id == eve &&
                  F::R().told == std::vector<Told>{{3, "Bob banned Eve"}, {0, "Bob banned Eve"}},
              "moderation: a client's offline /ban of an id with a record still bans it");
    }
    {
        Fakes<1>::R() = Rec{};
        F::R().known = true;
        const DispatchResult r = Dispatch(reg, bob, ("banid " + eve).c_str(), Seated(), MakePolicy());
        check(Said(r, "Banned eeeeeeee (offline).") && F::R().verb == "banOffline",
              "moderation: a record with an empty nick is a known id, named by its first eight");
    }
    {
        Fakes<1>::R() = Rec{};
        Policy noKnown = MakePolicy();
        noKnown.known = nullptr;
        const DispatchResult r = Dispatch(reg, bob, ("ban " + eve).c_str(), Seated(), noKnown);
        check(Refused(r, "eeeeeeee has never played here; only the host can act on an unknown id.") &&
                  F::R().calls == 0,
              "moderation: a policy with no known treats every offline id as unseen");
        const DispatchResult c = Dispatch(reg, console, ("ban " + eve).c_str(), Seated(), noKnown);
        check(Said(c, "Banned eeeeeeee (offline).") && F::R().verb == "banOffline",
              "moderation: the console bans an unseen id whatever known answers");
    }
    check(Said(RunAs<1>(reg, console, ("ban " + eve).c_str(), ModResult::NoSession),
               "There is no hosted session."),
          "moderation: an offline /ban answers NoSession");
    check(Said(RunAs<1>(reg, console, ("ban " + eve).c_str(), ModResult::NoId),
               "Could not ban eeeeeeee (NoId)."),
          "moderation: an offline /ban names a result it does not map");
}

void UnbanCases(Checker& check, const Registry& reg, const Caller& console) {
    using F = Fakes<1>;
    check(Said(Run<1>(reg, console, "unban abc"), "Give 8 to 32 characters of a player id.") &&
              F::R().prefixAsked.empty(),
          "moderation: /unban refuses a word under 8 characters");
    check(Said(Run<1>(reg, console, "unban zzzzzzzz"), "Give 8 to 32 characters of a player id."),
          "moderation: /unban refuses a word that is not hex");
    check(Said(Run<1>(reg, console, ("unban " + Repeat("1", 33)).c_str()),
               "Give 8 to 32 characters of a player id."),
          "moderation: /unban refuses a word over 32 characters");
    check(Said(Run<1>(reg, console, "unban 12345678"), "No ban matches 12345678.") && F::R().calls == 0,
          "moderation: /unban with no match says so");
    {
        Fakes<1>::R() = Rec{};
        F::R().prefixIds = {Repeat("1", 32), Repeat("2", 32)};
        const DispatchResult r = Dispatch(reg, console, "unban 12345678", Seated(), MakePolicy());
        check(Said(r, "2 bans match 12345678; give more of the id.") && F::R().calls == 0,
              "moderation: /unban with several matches asks for more of the id");
    }
    const std::string full = Repeat("1", 8) + Repeat("a", 24);
    {
        Fakes<1>::R() = Rec{};
        F::R().prefixIds = {full};
        const DispatchResult r = Dispatch(reg, console, "unban 11111111AA", Seated(), MakePolicy());
        check(Said(r, "Unbanned 11111111.") && F::R().verb == "unban" && F::R().id == full &&
                  F::R().prefixAsked == "11111111aa",
              "moderation: /unban lowers the word, matches by prefix and unbans the one id");
        F::R() = Rec{};
        F::R().prefixIds = {full};
        F::R().result = ModResult::NotBanned;
        check(Said(Dispatch(reg, console, "unban 11111111", Seated(), MakePolicy()), "11111111 is not banned."),
              "moderation: /unban of an id that was not banned says so");
        F::R() = Rec{};
        F::R().prefixIds = {full};
        F::R().result = ModResult::NoSession;
        check(Said(Dispatch(reg, console, "unban 11111111", Seated(), MakePolicy()), "There is no hosted session."),
              "moderation: /unban answers NoSession");
        F::R() = Rec{};
        F::R().prefixIds = {full};
        F::R().result = ModResult::Gone;
        check(Said(Dispatch(reg, console, "unban 11111111", Seated(), MakePolicy()),
                   "Could not unban 11111111 (Gone)."),
              "moderation: /unban names a result it does not map");
    }
}

void TeleportCases(Checker& check, const Registry& reg, const Caller& console, const Caller& bob) {
    using F = Fakes<1>;
    check(Said(Run<1>(reg, console, "tphere Cy"), "Cy is still joining.") && F::R().calls == 0,
          "moderation: /tphere of a player still joining does nothing");
    check(Said(Run<1>(reg, console, "tphere Dee"), "Teleported Dee to you.") && F::R().verb == "teleport" &&
              TokenIs(F::R().token, 3, 6, 4),
          "moderation: /tphere brings the target with its token");
    check(Said(RunAs<1>(reg, console, "tphere Dee", ModResult::Gone), "Dee already left."),
          "moderation: /tphere of a player who left says so");
    check(Said(RunAs<1>(reg, console, "tphere Dee", ModResult::Failed), "Could not teleport Dee (Failed)."),
          "moderation: /tphere names a failed teleport through the generic line");
    check(Said(RunAs<1>(reg, console, "tphere Dee", ModResult::NoSession), "There is no hosted session."),
          "moderation: /tphere answers NoSession");
    check(Said(RunAs<1>(reg, console, "tphere Dee", ModResult::NoId), "Could not teleport Dee (NoId)."),
          "moderation: /tphere names a result it does not map");
    check(Refused(Run<1>(reg, bob, "tphere Dee"), "/tphere is the host's until /tp can name a destination.") &&
              F::R().calls == 0,
          "moderation: /tphere is refused to a client before the verb");
    check(Refused(Run<1>(reg, console, "tphere Host"), "That is the host -- it cannot be teleported."),
          "moderation: /tphere of the host is refused");
}

}  // namespace

void ModerationCases(Checker& check) {
    const Caller console{0, 0, true};
    Caller bob;
    bob.slot = 1;
    bob.generation = 7;
    bob.playerId = Repeat("b", 32);

    Registry reg;
    check(moderation::Register(reg, Fakes<1>::MakePorts()), "moderation: the five roots register");
    check(reg.FindRoot("kick", nullptr) != nullptr && reg.FindRoot("ban", nullptr) != nullptr &&
              reg.FindRoot("banid", nullptr) != nullptr && reg.FindRoot("unban", nullptr) != nullptr &&
              reg.FindRoot("tphere", nullptr) != nullptr,
          "moderation: kick, ban, banid, unban and tphere are roots");
    {
        const CommandSpec* ban = reg.FindRoot("ban", nullptr);
        const CommandSpec* banid = reg.FindRoot("banid", nullptr);
        check(ban != nullptr && banid != nullptr && reg.NodeOf(*ban) == "multivoid.ban" &&
                  reg.NodeOf(*banid) == "multivoid.ban" &&
                  reg.FindNode("multivoid.ban.offline") != nullptr &&
                  reg.FindNode("multivoid.ban.exempt") != nullptr &&
                  reg.FindNode("multivoid.ban.notify") != nullptr &&
                  reg.FindNode("multivoid.banid.notify") == nullptr,
              "moderation: /ban and /banid share the node multivoid.ban and its qualifier nodes");
    }

    KickCases(check, reg, console, bob);
    BanCases(check, reg, console, bob);
    UnbanCases(check, reg, console);
    TeleportCases(check, reg, console, bob);

    for (const char* line : {"kick Cy", "ban Cy", "banid Cy", "unban 12345678", "tphere Dee"}) {
        Fakes<1>::R() = Rec{};
        Fakes<1>::R().hosted = false;
        const DispatchResult r = Dispatch(reg, console, line, Seated(), MakePolicy());
        check(Said(r, "There is no hosted session.") && Fakes<1>::R().calls == 0 &&
                  Fakes<1>::R().prefixAsked.empty(),
              "moderation: every root answers first that there is no hosted session");
    }

    {
        // Two registrations with different fakes, each calling its own.
        Registry other;
        check(moderation::Register(other, Fakes<2>::MakePorts()), "moderation: a second registry registers");
        Fakes<1>::R() = Rec{};
        Fakes<2>::R() = Rec{};
        const DispatchResult a = Dispatch(reg, console, "kick Cy", Seated(), MakePolicy());
        const DispatchResult b = Dispatch(other, console, "kick Dee", Seated(), MakePolicy());
        check(a.ran && b.ran && Fakes<1>::R().calls == 1 && Fakes<2>::R().calls == 1 &&
                  TokenIs(Fakes<1>::R().token, 2, 5, 9) && TokenIs(Fakes<2>::R().token, 3, 6, 4),
              "moderation: two registries with different ports each call their own");
    }
}

}  // namespace coop::commands
