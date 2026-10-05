// coop/permissions/permissions_cases_files.cpp -- the checks of coop/permissions/permission_files.h
// and the model's load half: the text of a holder file parsed in memory, then a Model loaded from
// parsed holders and asked as the files say, the writer's text read back, and one disk case in the
// runner's scratch folder.

#include "coop/permissions/permission_files.h"
#include "coop/permissions/permissions_selftest.h"
#include "coop/permissions/resolution.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>
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

bool ReportHas(const LoadReport& r, std::string_view needle) {
    for (const std::string& s : r.problems) {
        if (s == needle) return true;
    }
    return false;
}

bool ReportMentions(const LoadReport& r, std::string_view needle) {
    for (const std::string& s : r.problems) {
        if (s.find(needle) != std::string::npos) return true;
    }
    return false;
}

void WholeStoreCases(CheckSink& sink) {
    // A report with a problem is a broken store (ShouldLoad false); a report without one, and the
    // report of a server with no store, are not. Each store is a HolderText vector loaded through
    // LoadHolders, as the texts read from disk are.
    Model cleanModel;
    const LoadReport clean = LoadHolders({{"a", true, R"({"parents":["b"],"permissions":["ok.node"]})"},
                                          {"b", true, R"({"permissions":["other.node"]})"},
                                          {Id('a'), false, R"({"primaryGroup":"a","parents":["a"]})"}},
                                         cleanModel);
    sink.Check(clean.problems.empty() && clean.groups == 2 && clean.users == 1 && ShouldLoad(clean),
               "whole: a clean store loads, a parent group that sorts later is no miss");

    const Parsed bad = Parse(R"({"permissions":[{"permission":"deny.me","value":"no"},"b"]})");
    Model oneBadModel;
    const LoadReport oneBad =
        LoadHolders({{"a", true, R"({"permissions":["ok.node"]})"},
                     {"b", true, R"({"permissions":[{"permission":"deny.me","value":"no"},"b"]})"},
                     {Id('a'), false, R"({"permissions":["ok.node"]})"}},
                    oneBadModel);
    sink.Check(HasProblem(bad, "permission entry 0 refused (value)") && !oneBad.problems.empty() &&
                   !ShouldLoad(oneBad),
               "whole: one refused entry among good holders makes the store broken");

    sink.Check(ShouldLoad(LoadReport{}), "whole: no store is not a broken store");

    Model keyModel;
    const LoadReport key = LoadHolders({{"g1", true, R"({"permissions":["has space"]})"}}, keyModel);
    sink.Check(ReportHas(key, "g1: key \"has space\" refused") && !ShouldLoad(key) && key.groups == 1,
               "whole: a refused key is a problem and the holder still loads");

    Model holderModel;
    const LoadReport holder =
        LoadHolders({{"g1", true, "{}"}, {"g1", true, "{}"}, {"zz", false, "{}"}}, holderModel);
    sink.Check(ReportHas(holder, "g1: refused") && ReportHas(holder, "zz: refused") && holder.groups == 1 &&
                   holder.users == 0 && !ShouldLoad(holder),
               "whole: a refused holder (a duplicate group, an invalid user id) is a problem");

    Model primaryModel;
    const LoadReport primary = LoadHolders({{Id('a'), false, R"({"primaryGroup":"ghost"})"}}, primaryModel);
    sink.Check(ReportHas(primary, Id('a') + ": primary group \"ghost\" does not exist") && !ShouldLoad(primary),
               "whole: a user whose primary group is missing is a problem");

    // The holder file has no primaryGroup field: its primary is `default`, which exists, so no
    // primary line; the default step then promotes the missing parent to the stored primary, which
    // must not raise one either.
    Model parentModel;
    const LoadReport parent = LoadHolders({{"g1", true, R"({"parents":["phantom"]})"},
                                           {Id('b'), false, R"({"parents":["nowhere"]})"}},
                                          parentModel);
    sink.Check(ReportHas(parent, "g1: parent group \"phantom\" does not exist") &&
                   ReportHas(parent, Id('b') + ": parent group \"nowhere\" does not exist") &&
                   !ReportMentions(parent, ": primary group") && !ShouldLoad(parent),
               "whole: a group or user naming a missing parent group is a problem, and only that");

    Model falseParentModel;
    const LoadReport falseParent =
        LoadHolders({{Id('c'), false, R"({"permissions":[{"permission":"group.ghost","value":false}]})"}},
                    falseParentModel);
    sink.Check(falseParent.problems.empty() && ShouldLoad(falseParent) && falseParent.users == 1,
               "whole: a false group node is no parent, so a missing group it names is no problem");
}

Node WithContext(Node n, const char* key, const char* value) {
    n.contexts.Add(key, value);
    return n;
}

bool Has(const std::string& text, std::string_view needle) { return text.find(needle) != std::string::npos; }

// Every kind of node the writer has a form for, written and read back: each holder serialised
// again is byte-equal to the first text.
void RoundTripCases(CheckSink& sink) {
    Model m;
    m.CreateGroup("a");
    m.CreateGroup("b");
    Node temp = N("temp.node");
    temp.expiry = kNow + 60;
    Node two = N("two.ctx");
    two.contexts.Add("world", "x");
    two.contexts.Add("world", "y");
    m.SetNode(HolderKind::Group, "a", N("plain.node"));
    m.SetNode(HolderKind::Group, "a", N("denied.node", false));
    m.SetNode(HolderKind::Group, "a", temp);
    m.SetNode(HolderKind::Group, "a", WithContext(N("ctx.node"), "server", "a"));
    m.SetNode(HolderKind::Group, "a", two);
    m.SetNode(HolderKind::Group, "a", N("weight.10"));
    m.SetNode(HolderKind::Group, "a", N("group.b"));
    m.SetNode(HolderKind::User, Id('a'), N("group.b"));
    m.SetPrimaryGroup(Id('a'), "b");
    m.SetNode(HolderKind::User, Id('a'), N("group.default", false));

    std::vector<HolderText> texts;
    m.ForEachGroup([&](const Holder& g) { texts.push_back({g.name, true, SerializeHolder(g, kNow, true)}); });
    texts.push_back({Id('a'), false, SerializeHolder(*m.FindUser(Id('a')), kNow, true)});
    Model backModel;
    const LoadReport back = LoadHolders(texts, backModel);
    bool equal = back.problems.empty() && texts.size() == 4;
    for (const HolderText& t : texts) {
        const Holder* h = t.group ? backModel.FindGroup(t.stem) : backModel.FindUser(t.stem);
        equal = equal && h != nullptr && SerializeHolder(*h, kNow, true) == t.text;
    }
    sink.Check(equal, "write: every node form of a group and a user reads back to the same text");
    sink.Check(Has(texts[0].text, "\"group.b\"") && !Has(texts[0].text, "\"parents\"") &&
                   Has(texts[3].text, "\"primaryGroup\": \"b\""),
               "write: a parent is a permissions entry, never a parents entry, and a user's primary is written");
}

void ExpiredNodeCases(CheckSink& sink) {
    Model m;
    m.CreateGroup("a");
    Node old = N("old.node");
    old.expiry = kNow - 1;
    Node edge = N("edge.node");
    edge.expiry = kNow;
    m.SetNode(HolderKind::Group, "a", old);
    m.SetNode(HolderKind::Group, "a", edge);
    m.SetNode(HolderKind::Group, "a", N("live.node"));
    const Holder& g = *m.FindGroup("a");
    const std::string kept = SerializeHolder(g, kNow, false);
    const std::string pruned = SerializeHolder(g, kNow, true);
    sink.Check(Has(kept, "old.node") && !Has(pruned, "old.node"),
               "write: an expired node is in the text without pruning and left out with it");
    sink.Check(Has(pruned, "edge.node") && Has(pruned, "live.node"),
               "write: a node whose expiry second is now still applies and is kept");
}

void DefaultUserCases(CheckSink& sink) {
    Model m;
    m.CreateGroup("b");
    m.SetNode(HolderKind::User, Id('a'), N("group.default"));
    const Holder& plain = *m.FindUser(Id('a'));
    sink.Check(IsDefaultUser(plain, kNow, true) && IsDefaultUser(plain, kNow, false),
               "no-file state: group.default alone is the default state");

    Node old = N("old.node");
    old.expiry = kNow - 5;
    m.SetNode(HolderKind::User, Id('b'), N("group.default"));
    m.SetNode(HolderKind::User, Id('b'), old);
    const Holder& expired = *m.FindUser(Id('b'));
    sink.Check(IsDefaultUser(expired, kNow, true) && !IsDefaultUser(expired, kNow, false),
               "no-file state: an expired node beside it counts only without ignoreExpired");

    m.SetNode(HolderKind::User, Id('c'), N("other.node"));
    m.SetNode(HolderKind::User, Id('d'), WithContext(N("group.default"), "server", "a"));
    m.SetNode(HolderKind::User, Id('e'), N("group.default"));
    m.SetPrimaryGroup(Id('e'), "b");
    sink.Check(!IsDefaultUser(*m.FindUser(Id('c')), kNow, true) && !IsDefaultUser(*m.FindUser(Id('c')), kNow, false) &&
                   !IsDefaultUser(*m.FindUser(Id('d')), kNow, true) &&
                   !IsDefaultUser(*m.FindUser(Id('d')), kNow, false) &&
                   !IsDefaultUser(*m.FindUser(Id('e')), kNow, true) && !IsDefaultUser(*m.FindUser(Id('e')), kNow, false),
               "no-file state: another live node, a context on group.default, or another primary is not it");
}

bool WriteByHand(const std::filesystem::path& p, const std::string& text) {
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << text;
    return static_cast<bool>(f);
}

void DiskCases(CheckSink& sink, const std::filesystem::path& scratch) {
    // The runner has already failed a check when it had no folder to give.
    if (scratch.empty()) return;
    namespace fs = std::filesystem;
    const std::string text = R"({"permissions":["staff.use"]})";
    const fs::path file = scratch / "groups" / "staff.json";
    sink.Check(WriteHolderFile(scratch, true, "staff", &text).ok() && fs::is_regular_file(file),
               "disk: a write creates the folder and the file");
    std::vector<HolderText> texts;
    LoadReport read;
    sink.Check(ReadStoreTexts(scratch, &texts, &read) && read.problems.empty() && texts.size() == 1 &&
                   texts[0].stem == "staff" && texts[0].group && texts[0].text == text,
               "disk: ReadStoreTexts returns the file written");
    Model m;
    const LoadReport loaded = LoadStore(scratch, m);
    sink.Check(loaded.groups == 1 && loaded.users == 0 && loaded.problems.empty() && m.FindGroup("staff") != nullptr,
               "disk: LoadStore loads the one group with no problem");

    const std::string second = R"({"permissions":["staff.use","staff.more"]})";
    std::vector<HolderText> again;
    LoadReport readAgain;
    sink.Check(WriteHolderFile(scratch, true, "staff", &second).ok() && ReadStoreTexts(scratch, &again, &readAgain) &&
                   again.size() == 1 && again[0].text == second,
               "disk: a second write replaces the file whole");

    sink.Check(WriteHolderFile(scratch, true, "staff", nullptr).ok() && !fs::exists(file) &&
                   WriteHolderFile(scratch, true, "staff", nullptr).ok() &&
                   WriteHolderFile(scratch, false, Id('a'), nullptr).ok(),
               "disk: a delete removes the file, and a delete of a missing file or folder is ok");

    std::vector<HolderText> none;
    LoadReport missing;
    sink.Check(ReadStoreTexts(scratch / "nowhere", &none, &missing) && none.empty() && missing.problems.empty(),
               "disk: a missing folder is read as an empty store");

    sink.Check(WriteByHand(scratch / "groups" / "Bad Name.json", "{}") && WriteByHand(scratch / "users" / "abc.json", "{}"),
               "disk: the hand-written fixtures are written");
    std::vector<HolderText> skipped;
    LoadReport skipReport;
    sink.Check(ReadStoreTexts(scratch, &skipped, &skipReport) && skipped.empty() &&
                   ReportHas(skipReport, "bad name: not a valid group name, file skipped") &&
                   ReportHas(skipReport, "abc: not a 32 hex player id, file skipped"),
               "disk: a bad group name and a bad player id are skipped with their problems");
}

void ForEachGroupCases(CheckSink& sink) {
    Model m;
    m.CreateGroup("b");
    m.CreateGroup("a");
    std::string order;
    m.ForEachGroup([&](const Holder& g) { order += g.name + ";"; });
    sink.Check(order == "a;b;default;", "groups: ForEachGroup visits in ascending name order");
}

void NameCases(CheckSink& sink) {
    std::string out;
    sink.Check(NarrowAscii(L"Mods.JSON", &out) && out == "Mods.JSON" && NarrowAscii(L"", &out) && out.empty(),
               "name: an ASCII name narrows unchanged");
    sink.Check(!NarrowAscii(L"мод", &out) && out.empty() && !NarrowAscii(L"a\u0080", &out) && out.empty(),
               "name: a name holding a unit above 0x7F is refused, never narrowed through the code page");
}

}  // namespace

void RunFilesCases(CheckSink& sink, const std::filesystem::path& scratch) {
    PermissionFormCases(sink);
    ParentFormCases(sink);
    RefusalCases(sink);
    ExplicitCases(sink);
    LoadCases(sink);
    WholeStoreCases(sink);
    RoundTripCases(sink);
    ExpiredNodeCases(sink);
    DefaultUserCases(sink);
    DiskCases(sink, scratch);
    ForEachGroupCases(sink);
    NameCases(sink);
}

}  // namespace coop::permissions
