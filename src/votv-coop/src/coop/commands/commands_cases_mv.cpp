// coop/commands/commands_cases_mv.cpp -- the cases of coop/commands/mv_commands.h: each leaf's
// reply, the edit it hands the host and the notice it sends, the lines of the reading leaves and of
// reload, over fake ports and an in-memory store.
// The fake `apply` runs PlanEdit over the store's texts as the host's Apply does and composes the
// host's lines for the outcomes these cases reach (the host's own composition is the rig's). Called
// from RunSelftest.

#include "coop/commands/command_dispatcher.h"
#include "coop/commands/command_registry.h"
#include "coop/commands/command_targets.h"
#include "coop/commands/commands_selftest.h"
#include "coop/commands/mv_commands.h"
#include "coop/permissions/permission_edit.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace coop::commands {
namespace {

namespace perm = coop::permissions;

constexpr int64_t kNow = 1'700'000'000;

std::string Id(char c) { return std::string(32, c); }

// What the fake ports hold, receive and answer.
struct Fake {
    bool hosted = true;
    bool applyNoSession = false;
    bool hostHoldsLog = true;
    std::vector<perm::HolderText> texts;
    int applyCalls = 0;
    perm::HolderKey key{perm::HolderKind::User, std::string()};
    bool callerIsOwner = false;
    std::vector<std::string> nodes;
    perm::Action action;
    std::vector<std::pair<int, std::string>> told;
    std::vector<uint32_t> toldGeneration;
    // What `info` and `listgroups` read, and what `reload` answers and was asked by.
    perm::Model live;
    bool broken = false;
    std::vector<std::string> reloadLines;
    int reloadCalls = 0;
    std::string reloadActor;
};

Fake& F() {
    static Fake fake;
    return fake;
}

perm::ContextSet ListenSubject() {
    perm::ContextSet s;
    s.Add("mode", "listen");
    return s;
}

// The store's text for a holder, or null.
const std::string* TextOf(bool group, const std::string& stem) {
    for (const perm::HolderText& t : F().texts)
        if (t.group == group && t.stem == stem) return &t.text;
    return nullptr;
}

// Apply as the host composes it, for the outcomes the cases reach: a plan over the texts, the file
// text recorded on a change.
perm::host::ApplyResult FakeApply(const perm::HolderKey& key,
                                  const std::function<bool(perm::Model&, std::string*)>& change,
                                  bool callerIsOwner, const std::vector<std::string>& nodes,
                                  const perm::Action& action) {
    Fake& f = F();
    ++f.applyCalls;
    f.key = key;
    f.callerIsOwner = callerIsOwner;
    f.nodes = nodes;
    f.action = action;
    perm::host::ApplyResult out;
    if (f.applyNoSession) {
        out.outcome = perm::host::ApplyOutcome::NoSession;
        return out;
    }
    const perm::EditPlan plan = perm::PlanEdit(f.texts, key, change, callerIsOwner, Id('a'), ListenSubject(), kNow, nodes);
    switch (plan.result) {
        case perm::EditResult::NoChange:
            out.outcome = perm::host::ApplyOutcome::NoChange;
            out.replies = {"No change."};
            break;
        case perm::EditResult::Refused:
            out.outcome = perm::host::ApplyOutcome::Refused;
            out.replies = {plan.why};
            break;
        case perm::EditResult::OwnerLoses:
            out.outcome = perm::host::ApplyOutcome::Refused;
            out.replies = {"That would take " + plan.why + " away from the host."};
            break;
        case perm::EditResult::Changed: {
            const bool group = key.kind == perm::HolderKind::Group;
            size_t at = f.texts.size();
            for (size_t i = 0; i < f.texts.size(); ++i)
                if (f.texts[i].group == group && f.texts[i].stem == key.name) at = i;
            if (plan.deleteFile) {
                if (at != f.texts.size()) f.texts.erase(f.texts.begin() + static_cast<std::ptrdiff_t>(at));
            } else if (at != f.texts.size()) {
                f.texts[at].text = plan.text;
            } else {
                f.texts.push_back({key.name, group, plan.text});
            }
            out.outcome = perm::host::ApplyOutcome::Changed;
            out.replies = {"Done: " + action.description};
            break;
        }
    }
    return out;
}

bool Hosted() { return F().hosted; }
bool HostHoldsLog() { return F().hostHoldsLog; }
std::string NoNick(std::string_view) { return std::string(); }
int64_t Now() { return kNow; }
const perm::Model& Live() { return F().live; }
bool StoreBroken() { return F().broken; }
std::vector<std::string> Reload(const std::string& actorId) {
    ++F().reloadCalls;
    F().reloadActor = actorId;
    return F().reloadLines;
}
void Notify(const Caller& to, std::string_view line) {
    F().told.emplace_back(to.slot, std::string(line));
    F().toldGeneration.push_back(to.generation);
}

mv::Ports MakePorts() {
    mv::Ports p;
    p.hosted = &Hosted;
    p.apply = &FakeApply;
    p.reload = &Reload;
    p.live = &Live;
    p.storeBroken = &StoreBroken;
    p.recordNick = &NoNick;
    p.hostHoldsLog = &HostHoldsLog;
    p.now = &Now;
    p.notify = &Notify;
    return p;
}

PlayerView Seat(int slot, unsigned no, const char* nick, const std::string& id, uint32_t generation) {
    PlayerView v;
    v.slot = slot;
    v.playerNo = no;
    v.nick = nick;
    v.playerId = id;
    v.generation = generation;
    v.worldReady = true;
    return v;
}

// The host, Bob (a client), Cy and Dee; only Dee holds the notify node.
std::vector<PlayerView> Seated() {
    return {Seat(0, 1, "Host", Id('a'), 0), Seat(1, 2, "Bob", Id('b'), 7), Seat(2, 5, "Cy", Id('c'), 9),
            Seat(3, 6, "Dee", Id('d'), 4)};
}

bool AllowAll(const Caller&, std::string_view, bool) { return true; }
bool OperatorOnly(const Caller& who, std::string_view, bool) { return who.isOperator; }
bool HoldsDee(std::string_view id, std::string_view, bool) { return id == Id('d'); }
bool KnowsNobody(std::string_view) { return false; }
long long ClockNow() { return kNow; }

Policy MakePolicy(CheckFn check) {
    Policy p;
    p.check = check;
    p.holds = &HoldsDee;
    p.known = &KnowsNobody;
    p.nowSeconds = &ClockNow;
    return p;
}

void ResetFake() { F() = Fake{}; }

DispatchResult Run(const Registry& reg, const Caller& who, const std::string& line, CheckFn check = &AllowAll) {
    F().told.clear();
    F().toldGeneration.clear();
    F().applyCalls = 0;
    return Dispatch(reg, who, line, Seated(), MakePolicy(check));
}

// The handler ran and answered this one line.
bool Replied(const DispatchResult& r, const std::string& line) {
    return r.ran && r.replies.size() == 1 && r.replies[0] == line;
}

bool Has(const std::string* text, std::string_view needle) {
    return text != nullptr && text->find(needle) != std::string::npos;
}

Caller Console() {
    Caller c{0, 0, true};
    c.playerId = Id('a');
    return c;
}

Caller Bob() {
    Caller c{1, 7, false};
    c.playerId = Id('b');
    return c;
}

void RegisterCases(Checker& check, Registry& reg) {
    check(RegisterBuiltins(reg) && mv::Register(reg, MakePorts()), "mv: the tree registers");
    const NodeDecl* set = reg.FindNode("multivoid.mv.user.permission.set");
    check(set != nullptr && !set->defaultGranted && reg.FindNode("multivoid.mv.user") == nullptr &&
              reg.FindNode("multivoid.mv.group.permission") == nullptr &&
              reg.FindNode("multivoid.mv.user.permission.set.offline") != nullptr &&
              reg.FindNode("multivoid.mv.user.permission.set.notify") == nullptr &&
              reg.FindNode("multivoid.mv.group.permission.set.offline") == nullptr &&
              reg.FindNode("multivoid.mv.creategroup") != nullptr,
          "mv: only a leaf has a node, default false; the user leaves gate offline, none has its own notify node");

    const CommandSpec* mvRoot = reg.FindRoot("mv", nullptr);
    const auto leaf = [&](std::initializer_list<size_t> path) {
        const CommandSpec* c = mvRoot;
        for (size_t i : path) c = c != nullptr && i < c->subVerbs.size() ? &c->subVerbs[i] : nullptr;
        return c;
    };
    const CommandSpec* settemp = leaf({0, 0, 2});
    const CommandSpec* weight = leaf({1, 2});
    check(settemp != nullptr && weight != nullptr &&
              reg.Usage(*settemp) == "/mv user <who> permission settemp <node> [value] <for> [contexts...]" &&
              reg.Usage(*weight) == "/mv group <name> setweight <weight>",
          "mv: the usage lines carry the targets and the arguments");
}

// Each changing row of the table, and the store it leaves.
void UserRows(Checker& check, const Registry& reg) {
    ResetFake();
    const std::string cy = Id('c');
    const Caller console = Console();
    const std::string head = "Done: /mv user " + cy + " permission ";

    const DispatchResult set = Run(reg, console, "mv user Cy permission set a.b");
    check(Replied(set, head + "set a.b true") && F().applyCalls == 1 && F().callerIsOwner &&
              F().key.kind == perm::HolderKind::User && F().key.name == cy,
          "mv: permission set is Done with the canonical line, for the owner");
    const perm::Action& a = F().action;
    check(a.sourceId == Id('a') && a.sourceName == "Host" && a.targetType == "user" && a.targetId == cy &&
              a.targetName == "Cy" && a.description == "/mv user " + cy + " permission set a.b true",
          "mv: the action names the source, the target and the description");
    bool listed = false;
    for (const std::string& n : F().nodes) listed = listed || n == "multivoid.mv.user.permission.set";
    check(listed, "mv: the node list is the registry's");

    check(Replied(Run(reg, console, "mv user Cy permission set a.b false server=x"),
                  head + "set a.b false server=x"),
          "mv: set false with a context");
    check(Replied(Run(reg, console, "mv user Cy permission settemp a.b 1h"),
                  head + "settemp a.b true until 1700003600"),
          "mv: settemp 1h is an absolute expiry");
    check(Replied(Run(reg, console, "mv user Cy permission unset a.b"), head + "unset a.b"),
          "mv: permission unset");
    check(Replied(Run(reg, console, "mv user Cy permission unset a.b"), "No change."),
          "mv: unsetting what is not set is no change");
    check(Replied(Run(reg, console, "mv user Cy permission unsettemp a.b"), head + "unsettemp a.b"),
          "mv: permission unsettemp");
    check(Replied(Run(reg, console, "mv user Cy permission set group.bad!name"),
                  "'group.bad!name' is not a valid permission."),
          "mv: a node word the model refuses");
}

void GroupRows(Checker& check, const Registry& reg) {
    ResetFake();
    const std::string cy = Id('c');
    const Caller console = Console();

    check(Replied(Run(reg, console, "mv creategroup staff"), "Done: /mv creategroup staff") &&
              F().action.targetType == "group" && F().action.targetId == "staff",
          "mv: creategroup is Done and names the group");
    check(Replied(Run(reg, console, "mv creategroup staff"), "A group named staff already exists."),
          "mv: creategroup twice refuses the second");
    check(Replied(Run(reg, console, "mv creategroup bad!name"), "'bad!name' is not a valid group name."),
          "mv: creategroup refuses a word that is no group name");
    check(Replied(Run(reg, console, "mv user Cy parent add staff"), "Done: /mv user " + cy + " parent add staff"),
          "mv: parent add");
    check(Replied(Run(reg, console, "mv user Cy parent add nosuch"), "No group named nosuch."),
          "mv: parent add of a group that does not exist");
    check(Replied(Run(reg, console, "mv group staff setweight 5"), "Done: /mv group staff setweight 5") &&
              Replied(Run(reg, console, "mv group staff setweight 10"), "Done: /mv group staff setweight 10") &&
              Has(TextOf(true, "staff"), "weight.10") && !Has(TextOf(true, "staff"), "weight.5"),
          "mv: setweight replaces the group's weight");
    check(Replied(Run(reg, console, "mv group staff setweight 2147483648"),
                  "'2147483648' is not a weight: a whole number from -2147483648 to 2147483647."),
          "mv: a weight past int is refused");
    check(Replied(Run(reg, console, "mv group staff permission set s.use"),
                  "Done: /mv group staff permission set s.use true") &&
              F().key.kind == perm::HolderKind::Group && F().key.name == "staff",
          "mv: a group leaf names its holder by the lower-cased word");
    check(Replied(Run(reg, console, "mv group Staff permission unsettemp s.use"), "No change."),
          "mv: unsettemp of a node that is not timed is no change");
    check(Replied(Run(reg, console, "mv group nosuch permission set a.b"), "No group named nosuch.") &&
              Replied(Run(reg, console, "mv group nosuch parent add staff"), "No group named nosuch.") &&
              Replied(Run(reg, console, "mv group nosuch setweight 3"), "No group named nosuch."),
          "mv: a group leaf on a group that does not exist");
    check(Replied(Run(reg, console, "mv group bad!name permission set a.b"), "'bad!name' is not a valid group name.") &&
              F().applyCalls == 0,
          "mv: a group word that is no group name is refused before the host");
    check(Replied(Run(reg, console, "mv group staff parent add staff2"), "No group named staff2.") &&
              Replied(Run(reg, console, "mv creategroup staff2"), "Done: /mv creategroup staff2") &&
              Replied(Run(reg, console, "mv group staff parent add staff2"),
                      "Done: /mv group staff parent add staff2") &&
              Replied(Run(reg, console, "mv group staff parent remove staff2"),
                      "Done: /mv group staff parent remove staff2"),
          "mv: a group inherits another and stops");
    check(Replied(Run(reg, console, "mv deletegroup default"), "You cannot delete the default group.") &&
              Replied(Run(reg, console, "mv deletegroup ghost"), "No group named ghost.") &&
              Replied(Run(reg, console, "mv deletegroup staff2"), "Done: /mv deletegroup staff2"),
          "mv: deletegroup refuses default and a missing group, and deletes one");
}

void DefaultParentRows(Checker& check, const Registry& reg) {
    ResetFake();
    const Caller console = Console();
    const std::string dee = Id('d');
    check(Replied(Run(reg, console, "mv user Dee parent remove default"),
                  "A player is always in a group: add another before removing default."),
          "mv: removing default from a player with no other group is refused, with no user at all too");
    check(Replied(Run(reg, console, "mv creategroup staff"), "Done: /mv creategroup staff") &&
              Replied(Run(reg, console, "mv user Dee parent add staff"), "Done: /mv user " + dee + " parent add staff") &&
              Replied(Run(reg, console, "mv user Dee parent remove default"),
                      "Done: /mv user " + dee + " parent remove default"),
          "mv: default may go once the player holds another group");
}

void NoChangeAndOwnerRows(Checker& check, const Registry& reg) {
    ResetFake();
    const Caller console = Console();
    check(Replied(Run(reg, console, "mv user Cy permission set a.b"), "Done: /mv user " + Id('c') + " permission set a.b true") &&
              Replied(Run(reg, console, "mv user Cy permission set a.b"), "No change."),
          "mv: the same set twice is no change the second time");

    const Caller bob = Bob();
    const DispatchResult loses = Run(reg, bob, "mv group default permission set multivoid.mv.creategroup false");
    check(Replied(loses, "That would take multivoid.mv.creategroup away from the host.") && !F().callerIsOwner,
          "mv: an edit that takes a declared node from the host is refused for a delegate");
    check(Replied(Run(reg, console, "mv group default permission set multivoid.mv.creategroup false"),
                  "Done: /mv group default permission set multivoid.mv.creategroup false"),
          "mv: the owner's own edit of it is done");

    ResetFake();
    const DispatchResult refused = Run(reg, bob, "mv user Cy permission set a.b", &OperatorOnly);
    check(!refused.ran && refused.replies.size() == 1 &&
              refused.replies[0] ==
                  "You do not have permission for /mv user permission set (multivoid.mv.user.permission.set)." &&
              F().applyCalls == 0,
          "mv: a client without the leaf's node is refused by the dispatcher");
}

void NoticeRows(Checker& check, const Registry& reg) {
    ResetFake();
    const std::string cy = Id('c');
    const Caller bob = Bob();
    const std::string line = "[mv] Bob: user " + cy + " permission set n.a true";
    check(Replied(Run(reg, bob, "mv user Cy permission set n.a"), "Done: /mv user " + cy + " permission set n.a true") &&
              F().told.size() == 2 && F().told[0] == std::make_pair(3, line) && F().told[1] == std::make_pair(0, line) &&
              F().toldGeneration[0] == 4,
          "mv: a client's change is told to the notify slot and to the host that holds the log node");

    F().hostHoldsLog = false;
    Run(reg, bob, "mv user Cy permission set n.b");
    check(F().told.size() == 1 && F().told[0].first == 3,
          "mv: the host is not told when it does not hold the log node");

    F().hostHoldsLog = true;
    Run(reg, Console(), "mv user Cy permission set n.c");
    check(F().told.size() == 1 && F().told[0] ==
              std::make_pair(3, "[mv] Host: user " + cy + " permission set n.c true"),
          "mv: the host's own change is told to the notify slot, never to the host");

    check(Replied(Run(reg, bob, "mv user Cy permission set n.c"), "No change.") && F().told.empty(),
          "mv: a change that changes nothing tells no one");
}

void NoSessionRows(Checker& check, const Registry& reg) {
    ResetFake();
    F().hosted = false;
    const char* lines[] = {"mv user Cy permission set a.b",       "mv user Cy permission unset a.b",
                           "mv user Cy permission settemp a.b 1h", "mv user Cy permission unsettemp a.b",
                           "mv user Cy parent add staff",        "mv user Cy parent remove staff",
                           "mv group staff permission set a.b",  "mv group staff permission unset a.b",
                           "mv group staff permission settemp a.b 1h", "mv group staff permission unsettemp a.b",
                           "mv group staff parent add staff",    "mv group staff parent remove staff",
                           "mv group staff setweight 5",         "mv creategroup g",
                           "mv deletegroup g"};
    bool all = true;
    for (const char* line : lines) {
        const DispatchResult r = Run(reg, Console(), line);
        all = all && Replied(r, kNoSession) && F().applyCalls == 0;
    }
    check(all, "mv: with no hosted session every changing leaf answers so and asks nothing");

    ResetFake();
    F().applyNoSession = true;
    check(Replied(Run(reg, Console(), "mv user Cy permission set a.b"), kNoSession) && F().told.empty(),
          "mv: the host's NoSession is answered the same way, and tells no one");
}

perm::Node N(const std::string& key, bool value = true, int64_t expiry = 0, perm::ContextSet contexts = {}) {
    perm::Node n;
    n.key = key;
    n.value = value;
    n.expiry = expiry;
    n.contexts = std::move(contexts);
    return n;
}

perm::ContextSet Ctx(std::initializer_list<std::pair<const char*, const char*>> pairs) {
    perm::ContextSet s;
    for (const auto& p : pairs) s.Add(p.first, p.second);
    return s;
}

bool Contains(const std::vector<std::string>& lines, const std::string& line) {
    return std::find(lines.begin(), lines.end(), line) != lines.end();
}

// What `info` shows of a stored holder: its header, its parents, its other nodes with their marks.
void InfoFormatCases(Checker& check) {
    const perm::HolderKey una{perm::HolderKind::User, Id('1')};
    perm::Model m;
    m.CreateGroup("b");
    m.CreateGroup("staff");
    m.LoadUser(Id('1'), "b", {N("group.b"), N("group.staff", false, 1700003600)}, nullptr);
    const std::vector<std::string> lines = mv::InfoLines(m, una, "Una", kNow);
    check(lines == std::vector<std::string>{"Una: primary group b", "Parents: b, staff = false (until 1700003600)",
                                            "Nothing else is set."},
          "mv info: a user's primary group, its parents sorted by name with their marks, nothing else set");

    const perm::HolderKey marked{perm::HolderKind::User, Id('2')};
    m.SetNode(perm::HolderKind::User, Id('2'), N("a.b", true, 0, Ctx({{"server", "x"}, {"world", "y"}})));
    m.SetNode(perm::HolderKind::User, Id('2'), N("c.d", false, 1000, Ctx({{"server", "x"}})));
    m.SetNode(perm::HolderKind::User, Id('2'), N("e.f", true, 1000));
    m.SetNode(perm::HolderKind::User, Id('2'), N("g.h", true, 1700003600));
    const std::vector<std::string> other = mv::InfoLines(m, marked, "Mia", kNow);
    check(other.size() == 6 && other[0] == "Mia: primary group default" && other[1] == "Parents: default" &&
              Contains(other, "a.b [server=x, world=y]") && Contains(other, "c.d = false (expired) [server=x]") &&
              Contains(other, "e.f (expired)") && Contains(other, "g.h (until 1700003600)"),
          "mv info: a node's marks, in order: false, until or expired, contexts");

    const perm::HolderKey many{perm::HolderKind::User, Id('3')};
    for (int i = 1; i <= 25; ++i)
        m.SetNode(perm::HolderKind::User, Id('3'), N("n." + std::string(i < 10 ? "0" : "") + std::to_string(i)));
    const std::vector<std::string> capped = mv::InfoLines(m, many, "Max", kNow);
    check(capped.size() == 2 + 20 + 1 && capped[2] == "n.01" && capped[21] == "n.20" && capped[22] == "... and 5 more",
          "mv info: twenty node lines, then the count of the rest");

    m.CreateGroup("w");
    m.SetNode(perm::HolderKind::Group, "w", N("weight.5"));
    m.SetNode(perm::HolderKind::Group, "w", N("weight.10", true, 1000));
    m.CreateGroup("plain");
    check(mv::InfoLines(m, {perm::HolderKind::Group, "w"}, "w", kNow) ==
                  std::vector<std::string>{"w: weight 5", "Parents: none", "Nothing else is set."} &&
              mv::InfoLines(m, {perm::HolderKind::Group, "plain"}, "plain", kNow)[0] == "plain: no weight",
          "mv info: a group's current weight, an expired one left out, or no weight");

    check(mv::InfoLines(m, {perm::HolderKind::User, Id('9')}, "Zed", kNow) ==
                  std::vector<std::string>{"Zed has nothing set: the default group applies."} &&
              mv::InfoLines(m, {perm::HolderKind::Group, "ghost"}, "ghost", kNow) ==
                  std::vector<std::string>{"No group named ghost."},
          "mv info: a missing user and a missing group");
}

void ListGroupCases(Checker& check) {
    perm::Model m;
    for (int i = 1; i <= 21; ++i) m.CreateGroup(std::string("g") + (i < 10 ? "0" : "") + std::to_string(i));
    m.SetNode(perm::HolderKind::Group, "g01", N("weight.5"));
    const std::vector<std::string> lines = mv::ListGroupLines(m, kNow);
    check(lines.size() == 21 && lines[0] == "default" && lines[1] == "g01 (weight 5)" && lines[2] == "g02" &&
              lines[19] == "g19" && lines[20] == "... and 2 more",
          "mv listgroups: twenty of twenty-two groups in name order, then the count of the rest");
    perm::Model few;
    few.CreateGroup("b");
    few.CreateGroup("a");
    check(mv::ListGroupLines(few, kNow) == std::vector<std::string>{"a", "b", "default"},
          "mv listgroups: every group of a short list, by name");
}

// The reading leaves and reload through the dispatcher.
void ReadingRows(Checker& check, const Registry& reg) {
    ResetFake();
    const Caller console = Console();
    const std::string cy = Id('c');
    F().live.SetNode(perm::HolderKind::User, cy, N("a.b"));
    check(Run(reg, console, "mv user Cy info").replies ==
              std::vector<std::string>{"Cy: primary group default", "Parents: default", "a.b"},
          "mv: user info shows the stored user of the live model, named by its nick");
    check(Run(reg, console, "mv group default info").replies ==
              std::vector<std::string>{"default: no weight", "Parents: none", "Nothing else is set."},
          "mv: group info shows the stored group");
    check(Replied(Run(reg, console, "mv group bad!name info"), "'bad!name' is not a valid group name."),
          "mv: info of a word that is no group name is refused");
    check(Replied(Run(reg, console, "mv listgroups"), "default"), "mv: listgroups lists the live groups");

    F().broken = true;
    const std::string broken = "The permission files did not load at the host start: fix them, then /mv reload.";
    check(Replied(Run(reg, console, "mv user Cy info"), broken) && Replied(Run(reg, console, "mv group default info"), broken) &&
              Replied(Run(reg, console, "mv listgroups"), broken),
          "mv: with a broken store info and listgroups say so");
    F().broken = false;

    F().reloadLines = {"Reloaded: 1 groups, 2 users."};
    check(Replied(Run(reg, console, "mv reload"), "Reloaded: 1 groups, 2 users.") && F().reloadCalls == 1 &&
              F().reloadActor == Id('a'),
          "mv: reload sends the host's lines and names the caller's id");
    F().reloadLines = {};
    check(Replied(Run(reg, console, "mv reload"), kNoSession), "mv: an empty reload answer is no hosted session");
    const int asked = F().reloadCalls;
    const DispatchResult refused = Run(reg, Bob(), "mv reload", &OperatorOnly);
    check(!refused.ran && refused.replies.size() == 1 &&
              refused.replies[0] == "You do not have permission for /mv reload (multivoid.mv.reload)." &&
              F().reloadCalls == asked,
          "mv: reload is refused to a client without its node");

    F().hosted = false;
    const int before = F().reloadCalls;
    check(Replied(Run(reg, console, "mv user Cy info"), kNoSession) && Replied(Run(reg, console, "mv group staff info"), kNoSession) &&
              Replied(Run(reg, console, "mv listgroups"), kNoSession) && Replied(Run(reg, console, "mv reload"), kNoSession) &&
              F().reloadCalls == before,
          "mv: with no hosted session the reading leaves and reload answer so");
}

// The whole tree as /help lists it to the host: the order and the texts the command drill pins.
void HelpListCases(Checker& check, const Registry& reg) {
    ResetFake();
    const std::vector<std::string> help = Run(reg, Console(), "help").replies;
    check(help.size() == 21 && help[2] == "/mv user <who> permission set <node> [value] [contexts...] -- Sets a permission on a player." &&
              help[8] == "/mv user <who> info -- Shows a player's groups and permissions." &&
              help[15] == "/mv group <name> setweight <weight> -- Sets a group's weight (the highest wins)." &&
              help[16] == "/mv group <name> info -- Shows a group's parents, weight and permissions." &&
              help[19] == "/mv listgroups -- Lists the groups." &&
              help[20] == "/mv reload -- Reloads the permission files from disk.",
          "mv: /help lists the nineteen leaves in the tree's depth-first order");
    check(reg.FindNode("multivoid.mv.reload") != nullptr && !reg.FindNode("multivoid.mv.reload")->defaultGranted &&
              reg.FindNode("multivoid.mv.user.info.offline") != nullptr &&
              reg.FindNode("multivoid.mv.group.info.offline") == nullptr &&
              reg.FindNode("multivoid.mv.listgroups") != nullptr,
          "mv: the reading leaves declare their nodes, reload default false, no notify node among them");
}

}  // namespace

void MvCases(Checker& check) {
    Registry reg;
    RegisterCases(check, reg);
    UserRows(check, reg);
    GroupRows(check, reg);
    DefaultParentRows(check, reg);
    NoChangeAndOwnerRows(check, reg);
    NoticeRows(check, reg);
    NoSessionRows(check, reg);
    InfoFormatCases(check);
    ListGroupCases(check);
    ReadingRows(check, reg);
    HelpListCases(check, reg);
}

}  // namespace coop::commands
