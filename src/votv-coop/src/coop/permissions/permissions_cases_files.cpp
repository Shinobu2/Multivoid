// coop/permissions/permissions_cases_files.cpp -- the checks of coop/permissions/permission_files.h
// and the model's load half: the text of a holder file parsed in memory (no disk), then a Model
// loaded from parsed holders and asked as the files say.

#include "coop/permissions/permission_files.h"
#include "coop/permissions/permissions_selftest.h"
#include "coop/permissions/resolution.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace coop::permissions {
namespace {

constexpr int64_t kNow = 1'000'000;

std::string Id(char c) { return std::string(32, c); }

Node N(const char* key, bool value = true) {
    Node n;
    n.key = key;
    n.value = value;
    return n;
}

struct Parsed {
    bool ok = false;
    std::string primary;
    std::vector<Node> nodes;
    std::vector<std::string> problems;
};

Parsed Parse(std::string_view text) {
    Parsed p;
    p.ok = ParseHolderText(text, "t", &p.primary, &p.nodes, &p.problems);
    return p;
}

bool HasProblem(const Parsed& p, std::string_view needle) {
    for (const std::string& s : p.problems) {
        if (s.find(needle) != std::string::npos) return true;
    }
    return false;
}

void PermissionFormCases(CheckSink& sink) {
    const Parsed p = Parse(R"({"uuid":"x","name":"Bob","meta":[1],"prefixes":{"a":1},"primaryGroup":"admin","permissions":[
        "a.b",
        {"permission":"c.d","value":false,"expiry":2000,"server":"srv"},
        {"E.f":{"context":{"mode":["listen","dedicated"]},"world":"w1"}}]})");
    sink.Check(p.ok && p.problems.empty() && p.nodes.size() == 3, "files: the three permission forms parse clean");
    if (p.nodes.size() != 3) return;
    sink.Check(p.primary == "admin", "files: the primaryGroup is read");
    sink.Check(p.nodes[0].key == "a.b" && p.nodes[0].value && p.nodes[0].expiry == 0 && p.nodes[0].contexts.Empty(),
               "files: a string entry is a true global node");
    sink.Check(p.nodes[1].key == "c.d" && !p.nodes[1].value && p.nodes[1].expiry == 2000 &&
                   p.nodes[1].contexts.Contains("server", "srv"),
               "files: an object entry carries value, expiry and the server field");
    sink.Check(p.nodes[2].key == "E.f" && p.nodes[2].contexts.Size() == 3 &&
                   p.nodes[2].contexts.Contains("mode", "listen") && p.nodes[2].contexts.Contains("mode", "dedicated") &&
                   p.nodes[2].contexts.Contains("world", "w1"),
               "files: the one-key entry reads context lists and the world field");

    const Parsed global = Parse(R"({"permissions":[{"permission":"a","server":"global"}]})");
    sink.Check(global.nodes.size() == 1 && global.nodes[0].contexts.Empty(), "files: server=global is no context");
    const Parsed bare = Parse(R"({"foo":1})");
    sink.Check(bare.ok && bare.primary == "default" && bare.nodes.empty() && bare.problems.empty(),
               "files: unknown keys are ignored and the primary group defaults");
}

void ParentFormCases(CheckSink& sink) {
    const Parsed p = Parse(R"({"parents":["g1",
        {"group":"g2","expiry":500,"value":false},
        {"g3":{"context":{"server":"s"}}}]})");
    sink.Check(p.ok && p.problems.empty() && p.nodes.size() == 3, "files: the three parent forms parse clean");
    if (p.nodes.size() != 3) return;
    sink.Check(p.nodes[0].key == "group.g1" && p.nodes[0].value, "files: a string parent is a group node");
    sink.Check(p.nodes[1].key == "group.g2" && p.nodes[1].value && p.nodes[1].expiry == 500,
               "files: an object parent keeps its expiry and ignores its value");
    sink.Check(p.nodes[2].key == "group.g3" && p.nodes[2].contexts.Contains("server", "s"),
               "files: a one-key parent carries its context");
}

void RefusalCases(CheckSink& sink) {
    const Parsed badValue = Parse(R"({"permissions":[{"permission":"a","value":"yes"},"b"]})");
    sink.Check(badValue.ok && badValue.nodes.size() == 1 && badValue.nodes[0].key == "b" &&
                   HasProblem(badValue, "permission entry 0 refused (value)"),
               "files: a wrong-typed value refuses that entry only, with a problem");
    const Parsed badPair = Parse(R"({"permissions":[{"permission":"a","context":{"server":" "}},"b"]})");
    sink.Check(badPair.nodes.size() == 1 && badPair.nodes[0].key == "b" && HasProblem(badPair, "(context)"),
               "files: a refused context pair refuses the entry, never widens it to global");
    const Parsed emptyList = Parse(R"({"permissions":[{"permission":"a","context":{"server":[]}}]})");
    sink.Check(emptyList.nodes.empty() && HasProblem(emptyList, "(context)"),
               "files: an empty context list refuses the entry, never widens it to global");
    const Parsed badExpiry = Parse(R"({"permissions":[{"permission":"a","expiry":1.5}]})");
    sink.Check(badExpiry.nodes.empty() && HasProblem(badExpiry, "(expiry)"),
               "files: a non-integer expiry refuses the entry");
    const Parsed badPrimary = Parse(R"({"primaryGroup":5})");
    sink.Check(badPrimary.ok && badPrimary.primary == "default" && HasProblem(badPrimary, "primaryGroup"),
               "files: a non-string primaryGroup falls back to default with a problem");

    const std::string badUtf8 = std::string("{\"permissions\":[\"a") + "\xff" + "\"]}";
    const Parsed utf8 = Parse(badUtf8);
    sink.Check(!utf8.ok && HasProblem(utf8, "not a JSON object"), "files: an invalid UTF-8 byte refuses the file whole");
    const Parsed array = Parse("[1,2]");
    sink.Check(!array.ok && array.problems.size() == 1, "files: a non-object root is refused");
    const Parsed truncated = Parse("{\"permissions\":");
    sink.Check(!truncated.ok, "files: a truncated text is refused");
}

void ExplicitCases(CheckSink& sink) {
    Model m;
    m.CreateGroup("mods");
    m.SetNode(HolderKind::Group, "mods", N("multivoid.kick.exempt"));
    m.SetNode(HolderKind::User, Id('a'), N("group.mods"));
    m.SetNode(HolderKind::User, Id('b'), N("multivoid.*"));
    m.SetNode(HolderKind::User, Id('c'), N("multivoid.kick.exempt", false));
    Checker c(m);
    const auto a = c.Get(Id('a'), ContextSet(), kNow);
    const auto b = c.Get(Id('b'), ContextSet(), kNow);
    const auto d = c.Get(Id('c'), ContextSet(), kNow);
    sink.Check(IsSetExplicitly(*a, "multivoid.kick.exempt"), "explicit: a node set on a parent group counts");
    sink.Check(Evaluate(*b, "multivoid.kick.exempt", false).value == Tristate::True &&
                   !IsSetExplicitly(*b, "multivoid.kick.exempt"),
               "explicit: a wildcard grants the node but never counts as setting it");
    sink.Check(!IsSetExplicitly(*d, "multivoid.kick.exempt"), "explicit: a false node does not count");
}

void LoadCases(CheckSink& sink) {
    const Parsed staff = Parse(R"({"permissions":["staff.use"]})");
    const Parsed userA = Parse(R"({"primaryGroup":"staff","parents":["staff"],"permissions":["own.node"]})");
    const Parsed userB = Parse("{}");
    Model m;
    std::vector<size_t> refused;
    sink.Check(m.LoadGroup("Staff", staff.nodes, &refused) && refused.empty(), "load: a group loads from parsed nodes");
    sink.Check(!m.LoadGroup("staff", staff.nodes, &refused) && !m.LoadGroup("bad name", staff.nodes, &refused),
               "load: an existing or invalid group is refused as the holder");
    sink.Check(m.LoadUser(Id('a'), userA.primary, userA.nodes, &refused) && refused.empty() &&
                   m.LoadUser(Id('b'), userB.primary, userB.nodes, &refused),
               "load: users load from parsed nodes");
    sink.Check(!m.LoadUser(Id('a'), userA.primary, userA.nodes, &refused) &&
                   !m.LoadUser("zz", "default", std::vector<Node>(), &refused),
               "load: an existing user and an invalid id are refused as the holder");

    Checker c(m);
    const auto a = c.Get(Id('a'), ContextSet(), kNow);
    const auto b = c.Get(Id('b'), ContextSet(), kNow);
    sink.Check(Evaluate(*a, "staff.use", false).value == Tristate::True &&
                   Evaluate(*a, "own.node", false).value == Tristate::True,
               "load: a user resolves its own node and a group's it inherits");
    sink.Check(Evaluate(*b, "staff.use", false).value == Tristate::Undefined,
               "load: a user outside the group does not");
    const Holder* userBHolder = m.FindUser(Id('b'));
    bool hasDefault = false;
    if (userBHolder) {
        for (const Node& n : userBHolder->nodes.Nodes()) hasDefault = hasDefault || n.key == "group.default";
    }
    sink.Check(userBHolder && userBHolder->primaryGroup == "default" && hasDefault,
               "load: a user with no stored global group gains group.default");

    const Parsed badKey = Parse(R"({"permissions":["ok.node","has space","also.ok"]})");
    Model k;
    std::vector<size_t> bad;
    sink.Check(k.LoadGroup("keys", badKey.nodes, &bad) && bad.size() == 1 && bad[0] == 1,
               "load: a bad key is refused by index and the holder still loads");
    k.SetNode(HolderKind::User, Id('d'), N("group.keys"));
    Checker kc(k);
    const auto d = kc.Get(Id('d'), ContextSet(), kNow);
    sink.Check(Evaluate(*d, "ok.node", false).value == Tristate::True &&
                   Evaluate(*d, "also.ok", false).value == Tristate::True,
               "load: the nodes around a refused one load");
}

}  // namespace

void RunFilesCases(CheckSink& sink) {
    PermissionFormCases(sink);
    ParentFormCases(sink);
    RefusalCases(sink);
    ExplicitCases(sink);
    LoadCases(sink);
}

}  // namespace coop::permissions
