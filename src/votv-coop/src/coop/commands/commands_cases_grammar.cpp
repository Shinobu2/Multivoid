// coop/commands/commands_cases_grammar.cpp -- the cases of the command grammar: a targeted parent
// (an argument taken between two verbs) over test trees, each case a Dispatch of one line. Called
// from RunSelftest.

#include "coop/commands/command_dispatcher.h"
#include "coop/commands/command_registry.h"
#include "coop/commands/command_targets.h"
#include "coop/commands/commands_selftest.h"

#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace coop::commands {
namespace {

// What the last handler call saw.
struct GrammarCapture {
    bool called = false;
    std::string path;
    std::vector<TargetResult> targets;
    std::vector<long long> integers;
    std::vector<std::string> texts;
    std::vector<bool> given;
};
GrammarCapture g_grammar;

void CaptureGrammar(Context& ctx) {
    g_grammar.called = true;
    g_grammar.path = ctx.registry.PathOf(ctx.spec);
    g_grammar.targets = ctx.targets;
    g_grammar.integers = ctx.integers;
    g_grammar.texts = ctx.texts;
    g_grammar.given = ctx.given;
    ctx.Reply("ok");
}

PlayerView GrammarPlayer(int slot, unsigned no, const char* nick, const std::string& id) {
    PlayerView p;
    p.slot = slot;
    p.playerNo = no;
    p.nick = nick;
    p.playerId = id;
    return p;
}

// Nemo's id is empty: seated, not proved yet.
std::vector<PlayerView> GrammarPlayers() {
    return {GrammarPlayer(0, 1, "Host", std::string(32, 'a')),
            GrammarPlayer(1, 2, "Alice", std::string(32, 'd')), GrammarPlayer(2, 3, "Nemo", "")};
}

bool AllowAll(const Caller&, std::string_view, bool) { return true; }
int PickFirst(int) { return 0; }
bool KnowsNobody(std::string_view) { return false; }

Policy GrammarPolicy() {
    Policy p;
    p.check = &AllowAll;
    p.pick = &PickFirst;
    p.known = &KnowsNobody;
    return p;
}

bool RefusedFor(Registry& reg, const CommandSpec& c, const char* reasonPart) {
    std::string why;
    return !reg.Register(c, &why) && why.find(reasonPart) != std::string::npos;
}

bool SaidOnly(const DispatchResult& r, const char* line) {
    return !r.ran && r.replies.size() == 1 && r.replies[0] == line;
}

DispatchResult Run(const Registry& reg, const Caller& who, const char* line, const Policy& policy) {
    g_grammar = GrammarCapture{};
    return Dispatch(reg, who, line, GrammarPlayers(), policy);
}

// A spec without a handler, with sub-verbs and the arguments it takes; a leaf with a handler.
CommandSpec Parent(const char* name, std::vector<ArgSpec> args, std::vector<CommandSpec> subs) {
    CommandSpec c;
    c.name = name;
    c.description = std::string("The ") + name + " verbs.";
    c.args = std::move(args);
    c.subVerbs = std::move(subs);
    return c;
}

CommandSpec Leaf(const char* name, std::vector<ArgSpec> args, std::vector<Qualifier> qualifiers) {
    CommandSpec c;
    c.name = name;
    c.description = std::string("Runs ") + name + ".";
    c.args = std::move(args);
    c.qualifiers = std::move(qualifiers);
    c.handler = &CaptureGrammar;
    return c;
}

const Qualifier kOffline{"offline", QualKind::GateOffline};

// mvt user <who> permission set <node>; mvt group <name> info; mvt group <name> member <who> drop.
CommandSpec TargetedTree() {
    CommandSpec user = Parent("user", {{"who", ArgKind::PlayerOrId, false}},
                              {Parent("permission", {}, {Leaf("set", {{"node", ArgKind::Word, false}},
                                                              {kOffline})})});
    CommandSpec member = Parent("member", {{"who", ArgKind::PlayerOrId, false}},
                                {Leaf("drop", {}, {kOffline})});
    CommandSpec group = Parent("group", {{"name", ArgKind::Word, false}},
                               {Leaf("info", {}, {}), member});
    return Parent("mvt", {}, {user, group});
}

// A one-defect tree: root `bad` -> `user` (a targeted Word parent) -> `leaf`; each case edits one
// part before it is registered.
struct BadTree {
    CommandSpec leaf = Leaf("leaf", {}, {});
    CommandSpec user = Parent("user", {{"who", ArgKind::Word, false}}, {});
    CommandSpec root = Parent("bad", {}, {});
    CommandSpec Build() {
        user.subVerbs = {leaf};
        root.subVerbs = {user};
        return root;
    }
};

void TargetedParentCases(Checker& check) {
    Registry reg;
    check(RegisterBuiltins(reg) && reg.Register(TargetedTree(), nullptr),
          "grammar: the targeted tree registers");
    const CommandSpec* mvt = reg.FindRoot("mvt", nullptr);
    const CommandSpec* user = mvt != nullptr ? &mvt->subVerbs[0] : nullptr;
    const CommandSpec* set = user != nullptr ? &user->subVerbs[0].subVerbs[0] : nullptr;
    const CommandSpec* group = mvt != nullptr ? &mvt->subVerbs[1] : nullptr;
    const CommandSpec* drop = group != nullptr ? &group->subVerbs[1].subVerbs[0] : nullptr;
    if (set == nullptr || drop == nullptr) {
        check(false, "grammar: the targeted tree has its specs");
        return;
    }
    const auto argNames = [&](const CommandSpec& c, std::initializer_list<const char*> names) {
        const std::vector<const ArgSpec*> args = reg.ArgsOf(c);
        if (args.size() != names.size()) return false;
        size_t i = 0;
        for (const char* n : names)
            if (args[i++]->name != n) return false;
        return true;
    };
    check(reg.Usage(*set) == "/mvt user <who> permission set <node>" &&
              reg.Usage(*user) == "/mvt user <who> <permission>" &&
              argNames(*set, {"who", "node"}) && argNames(*drop, {"name", "who"}),
          "grammar: usage and effective arguments name the targets");

    const Policy policy = GrammarPolicy();
    Caller who;
    who.slot = 1;
    const Caller console{0, 0, true};

    {
        const DispatchResult r = Run(reg, who, "mvt user Alice permission set a.b", policy);
        check(r.ran && g_grammar.path == "mvt user permission set" &&
                  g_grammar.targets.size() == 2 && g_grammar.targets[0].slots == std::vector<int>{1} &&
                  g_grammar.texts[0] == "Alice" && g_grammar.texts[1] == "a.b" &&
                  g_grammar.given[0] && g_grammar.given[1],
              "grammar: a targeted parent's target reaches the leaf in the first slot");
    }
    {
        const std::string unseen(32, 'e');
        const std::string line = "mvt user " + unseen + " permission set a.b";
        check(SaidOnly(Run(reg, who, line.c_str(), policy),
                       "eeeeeeee has never played here; only the host can act on an unknown id."),
              "grammar: a caller that is not the console is refused an id the host never saw");
        const DispatchResult r = Run(reg, console, line.c_str(), policy);
        check(r.ran && g_grammar.targets[0].offline && g_grammar.targets[0].offlineId == unseen,
              "grammar: the console reaches the leaf with an offline target");
    }
    check(SaidOnly(Run(reg, who, "mvt user Alice", policy), "Usage: /mvt user <who> <permission>") &&
              SaidOnly(Run(reg, who, "mvt user Alice nothing", policy),
                       "Usage: /mvt user <who> <permission>") &&
              SaidOnly(Run(reg, who, "mvt user", policy), "Usage: /mvt user <who> <permission>"),
          "grammar: a targeted parent without its verb answers its usage");
    {
        const DispatchResult info = Run(reg, who, "mvt group staff info", policy);
        check(info.ran && g_grammar.texts.size() == 1 && g_grammar.texts[0] == "staff",
              "grammar: a Word target reaches a leaf with no argument of its own");
        const DispatchResult dropped = Run(reg, who, "mvt group staff member Alice drop", policy);
        check(dropped.ran && g_grammar.texts[0] == "staff" &&
                  g_grammar.targets[1].slots == std::vector<int>{1},
              "grammar: two targeted parents in one path each add their word");
    }
    check(SaidOnly(Run(reg, who, "mvt user @a permission set a.b", policy),
                   "Name one player, not @a, @p, @r or @s.") &&
              SaidOnly(Run(reg, who, "mvt user @s permission set a.b", policy),
                       "Name one player, not @a, @p, @r or @s.") &&
              SaidOnly(Run(reg, who, "mvt user Nemo permission set a.b", policy),
                       "Nemo's identity is not proved yet."),
          "grammar: a target names one player, and a proved one");

    {
        Registry bad;
        BadTree t;
        t.user.args = {{"who", ArgKind::PlayerOrId, false}, {"x", ArgKind::Word, false}};
        check(RefusedFor(bad, t.Build(), "one required Word, Player or PlayerOrId"),
              "grammar: a targeted spec with two arguments is refused");
        BadTree o;
        o.user.args = {{"who", ArgKind::Word, true}};
        check(RefusedFor(bad, o.Build(), "one required Word, Player or PlayerOrId"),
              "grammar: a targeted spec with an optional argument is refused");
        BadTree r;
        r.user.args = {{"who", ArgKind::Rest, false}};
        check(RefusedFor(bad, r.Build(), "one required Word, Player or PlayerOrId"),
              "grammar: a targeted spec with a Rest argument is refused");
        BadTree q;
        q.root.qualifiers = {kOffline};
        check(RefusedFor(bad, q.Build(), "a qualifier on a spec without a handler"),
              "grammar: a spec without a handler with a qualifier is refused");
        BadTree a;
        a.root.args = {{"who", ArgKind::Word, false}};
        a.root.aliases = {{"b", "user"}};
        check(RefusedFor(bad, a.Build(), "an alias on a spec that takes a target"),
              "grammar: a targeted root with an alias is refused");
        BadTree g;
        g.leaf.qualifiers = {kOffline};
        check(RefusedFor(bad, g.Build(), "needs exactly one Player or PlayerOrId"),
              "grammar: a gate under a Word target is refused");
        BadTree n;
        n.user.args = {{"who", ArgKind::PlayerOrId, false}};
        check(RefusedFor(bad, n.Build(), "needs a GateOffline qualifier"),
              "grammar: a PlayerOrId target with no gate below is refused");
        BadTree h;
        h.user.args = {{"who", ArgKind::PlayerOrId, false, true}};
        h.leaf.qualifiers = {kOffline};
        check(RefusedFor(bad, h.Build(), "needs a pastTense"),
              "grammar: a notHost target with no pastTense is refused");
    }

    {
        const DispatchResult r = Run(reg, who, "help mvt user x permission set", policy);
        check(r.ran && r.replies.size() == 1 &&
                  r.replies[0] == "/mvt user <who> permission set <node> -- Runs set.",
              "grammar: help walks through a targeted parent");
    }
}

}  // namespace

void GrammarCases(Checker& check) { TargetedParentCases(check); }

}  // namespace coop::commands
