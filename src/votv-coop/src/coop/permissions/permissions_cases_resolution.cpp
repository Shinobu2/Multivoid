// coop/permissions/permissions_cases_resolution.cpp -- the checks of coop/permissions/resolution.h:
// LuckPerms' PermissionProcessorTest, PermissionCalculatorTest and InheritanceTest rows over the
// resolved map, then the cases this port adds (flattening, the owner step, exact expiry, the cache).
// Each check holds the shared_ptr first: a Decision views into the Resolved it was made from.

#include "coop/permissions/permissions_selftest.h"
#include "coop/permissions/resolution.h"

#include <cstdint>
#include <initializer_list>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace coop::permissions {
namespace {

using Pair = std::pair<const char*, const char*>;

constexpr int64_t kNow = 1'000'000;

std::string Id(char c) { return std::string(32, c); }

ContextSet Ctx(std::initializer_list<Pair> pairs = {}) {
    ContextSet s;
    for (const Pair& p : pairs) s.Add(p.first, p.second);
    return s;
}

Node N(const std::string& key, bool value = true, int64_t expiry = 0, ContextSet contexts = {}) {
    Node n;
    n.key = key;
    n.value = value;
    n.expiry = expiry;
    n.contexts = std::move(contexts);
    return n;
}

void Grant(Model& m, HolderKind kind, const std::string& who, std::initializer_list<std::pair<const char*, bool>> nodes) {
    for (const auto& [key, value] : nodes) m.SetNode(kind, who, N(key, value));
}

bool Is(const Resolved& r, const char* query, Tristate want, const char* node = nullptr, const char* holder = nullptr,
        bool owner = false) {
    const Decision d = Evaluate(r, query, owner);
    return d.value == want && (node == nullptr || d.node == node) && (holder == nullptr || d.holder == holder);
}

void DirectCases(CheckSink& sink) {
    // V2a testDirect
    Model m;
    Grant(m, HolderKind::User, Id('1'), {{"test.node1", true}, {"test.node2", false}});
    Checker c(m);
    const auto r = c.Get(Id('1'), Ctx(), kNow);
    sink.Check(Is(*r, "test", Tristate::Undefined), "direct: test is undefined");
    sink.Check(Is(*r, "test.node1", Tristate::True), "direct: test.node1 is true");
    sink.Check(Is(*r, "test.node2", Tristate::False), "direct: test.node2 is false");
    sink.Check(Evaluate(*r, "test", false).node.empty(), "direct: an undefined answer names no node");
    sink.Check(Is(*r, "test.node1", Tristate::True, "test.node1"), "direct: test.node1 is decided by its key");
    sink.Check(Is(*r, "test.node2", Tristate::False, "test.node2"), "direct: test.node2 is decided by its key");
}

void WildcardCases(CheckSink& sink) {
    // V2b testWildcard over EXAMPLE_PERMISSIONS without its regex keys and overridetest.*.
    Model m;
    Grant(m, HolderKind::User, Id('1'),
          {{"test.node1", true}, {"test.node2", false}, {"one.two.three.four", true}, {"one.two.three.*", false},
           {"one.two.three", true}, {"one.two.*", false}, {"one.two", true}, {"one.*", false}, {"one", true},
           {"*", false}});
    Checker c(m);
    const auto r = c.Get(Id('1'), Ctx(), kNow);
    struct Row {
        const char* query;
        bool value;
        const char* decidedBy;  // the query itself when direct, else the wildcard key
    };
    const Row rows[] = {
        {"one.two.three.four", true, "one.two.three.four"},
        {"one.two.three.test", false, "one.two.three.*"},
        {"one.two.three.*", false, "one.two.three.*"},
        {"one.two.three", true, "one.two.three"},
        {"one.two.test", false, "one.two.*"},
        {"one.two.*", false, "one.two.*"},
        {"one.two", true, "one.two"},
        {"one.test", false, "one.*"},
        {"one.*", false, "one.*"},
        {"one", true, "one"},
        {"test", false, "*"},
        {"*", false, "*"},
    };
    for (const Row& row : rows) {
        const Decision d = Evaluate(*r, row.query, false);
        sink.Check(d.value == (row.value ? Tristate::True : Tristate::False), "wildcard: the value of a query");
        sink.Check(d.node == row.decidedBy, "wildcard: the deciding key of a query");
    }
}

void InheritedNodesCases(CheckSink& sink) {
    // V4b testResolveInheritedNodes: start `test` -> vip+ (w10) -> vip (w5) -> member.
    Model m;
    m.CreateGroup("member");
    m.SetNode(HolderKind::Group, "member", N("test", false));
    m.CreateGroup("vip");
    m.SetNode(HolderKind::Group, "vip", N("weight.5"));
    m.SetNode(HolderKind::Group, "vip", N("group.member"));
    m.SetNode(HolderKind::Group, "vip", N("test", true, 0, Ctx({{"server", "test"}})));
    m.CreateGroup("vip+");
    m.SetNode(HolderKind::Group, "vip+", N("weight.10"));
    m.SetNode(HolderKind::Group, "vip+", N("group.vip"));
    m.SetNode(HolderKind::Group, "vip+", N("test"));
    m.CreateGroup("test");
    m.SetNode(HolderKind::Group, "test", N("group.vip+"));

    auto filtered = [&](const ContextSet& subject) {
        std::vector<std::pair<const Holder*, const Node*>> all;
        ResolveInheritedNodes(m, *m.FindGroup("test"), subject, kNow, all);
        std::string s;
        for (const auto& [h, n] : all) {
            if (n->key != "test") continue;
            s += h->name + (n->value ? "=true;" : "=false;");
        }
        return s;
    };
    sink.Check(filtered(Ctx()) == "vip+=true;member=false;", "inherited: without a context, vip+ then member");
    sink.Check(filtered(Ctx({{"server", "test"}})) == "vip+=true;vip=true;member=false;",
               "inherited: under server=test, vip's contextual node joins");
}

void FlatteningCases(CheckSink& sink) {
    {
        Model m;
        Grant(m, HolderKind::Group, "default", {{"test", false}});
        Grant(m, HolderKind::User, Id('1'), {{"test", true}});
        Checker c(m);
        const auto r = c.Get(Id('1'), Ctx(), kNow);
        sink.Check(Is(*r, "test", Tristate::True, "test", ("user:" + Id('1')).c_str()),
                   "flatten: the user's own node beats its group's");
    }
    {
        Model m;
        m.CreateGroup("a");
        m.CreateGroup("b");
        Grant(m, HolderKind::Group, "a", {{"test", true}});
        Grant(m, HolderKind::Group, "b", {{"test", false}});
        m.SetNode(HolderKind::Group, "a", N("group.b"));
        m.SetNode(HolderKind::User, Id('1'), N("group.a"));
        Checker c(m);
        const auto r = c.Get(Id('1'), Ctx(), kNow);
        sink.Check(Is(*r, "test", Tristate::True, "test", "group:a"), "flatten: a nearer group's node beats a farther one's");
    }
}

void OwnerCases(CheckSink& sink) {
    Model m;
    m.SetNode(HolderKind::User, Id('1'), N("other"));
    Checker c(m);
    {
        const auto r = c.Get(Id('1'), Ctx(), kNow);
        sink.Check(Is(*r, "test", Tristate::True, "", "owner", true), "owner: an undefined query is true from the owner step");
        sink.Check(Is(*r, "test", Tristate::Undefined, nullptr, nullptr, false), "owner: without the owner step it stays undefined");
    }
    Grant(m, HolderKind::Group, "default", {{"test", false}});
    {
        const auto r = c.Get(Id('1'), Ctx(), kNow);
        sink.Check(Is(*r, "test", Tristate::False, "test", "group:default", true), "owner: a false in default denies the owner");
    }
    Grant(m, HolderKind::User, Id('1'), {{"test", true}});
    {
        const auto r = c.Get(Id('1'), Ctx(), kNow);
        sink.Check(Is(*r, "test", Tristate::True, "test", ("user:" + Id('1')).c_str(), true),
                   "owner: the owner's own true beats default's false");
    }
    Model m2;
    m2.SetNode(HolderKind::User, Id('2'), N("*", false));
    Checker c2(m2);
    const auto r2 = c2.Get(Id('2'), Ctx(), kNow);
    sink.Check(Is(*r2, "anything", Tristate::False, "*", nullptr, true), "owner: a false * still denies the owner");
}

void ExpiryCases(CheckSink& sink) {
    constexpr int64_t kE = kNow + 100;
    {
        Model m;
        m.SetNode(HolderKind::User, Id('1'), N("test", true, kE));
        Checker c(m);
        const auto at = c.Get(Id('1'), Ctx(), kE);
        sink.Check(Is(*at, "test", Tristate::True), "expiry: a temporary node answers at its second");
        const size_t builds = c.BuildCountForTest();
        const auto after = c.Get(Id('1'), Ctx(), kE + 1);
        sink.Check(Is(*after, "test", Tristate::Undefined) && c.BuildCountForTest() == builds + 1,
                   "expiry: it stops answering at the next second, the entry rebuilt, the model untouched");
    }
    {
        Model m;
        m.CreateGroup("vip");
        m.SetNode(HolderKind::Group, "vip", N("vp.node"));
        m.SetNode(HolderKind::User, Id('1'), N("group.vip", true, kE));
        Checker c(m);
        const auto at = c.Get(Id('1'), Ctx(), kE);
        sink.Check(Is(*at, "vp.node", Tristate::True), "expiry: a temporary group answers at its second");
        const size_t builds = c.BuildCountForTest();
        const auto after = c.Get(Id('1'), Ctx(), kE + 1);
        sink.Check(Is(*after, "vp.node", Tristate::Undefined) && c.BuildCountForTest() == builds + 1,
                   "expiry: and stops at the next, the entry rebuilt");
    }
    {
        // The user's own nodes are all permanent; a group it inherits holds the temporary node.
        Model m;
        m.CreateGroup("g");
        m.SetNode(HolderKind::Group, "g", N("x", true, kE));
        m.SetNode(HolderKind::User, Id('1'), N("group.g"));
        m.SetNode(HolderKind::User, Id('1'), N("own"));
        const auto r = Resolve(m, Id('1'), Ctx(), kNow);
        sink.Check(r->validUntil == kE, "expiry: validUntil is the earliest expiry among the holders consulted");
    }
}

void CacheCases(CheckSink& sink) {
    {
        Model m;
        m.CreateGroup("g");
        m.SetNode(HolderKind::User, Id('1'), N("group.g"));
        Checker c(m);
        const auto before = c.Get(Id('1'), Ctx(), kNow);
        m.SetNode(HolderKind::Group, "g", N("granted"));
        const auto after = c.Get(Id('1'), Ctx(), kNow);
        sink.Check(Is(*before, "granted", Tristate::Undefined) && Is(*after, "granted", Tristate::True),
                   "cache: a change to an inherited group is seen by the next Get");
        m.SetNode(HolderKind::User, Id('1'), N("own"));
        const auto afterUser = c.Get(Id('1'), Ctx(), kNow);
        sink.Check(Is(*afterUser, "own", Tristate::True), "cache: a change to the user is seen by the next Get");
        m.SetNode(HolderKind::User, Id('2'), N("someone.else"));
        const size_t builds = c.BuildCountForTest();
        const auto same = c.Get(Id('1'), Ctx(), kNow);
        sink.Check(same == afterUser && c.BuildCountForTest() == builds, "cache: a change to another user leaves the entry");
    }
    {
        // An unknown player id is answered as default's nodes and creates no user.
        Model m;
        Grant(m, HolderKind::Group, "default", {{"d.node", true}});
        Checker c(m);
        const auto r = c.Get(Id('9'), Ctx(), kNow);
        sink.Check(Is(*r, "d.node", Tristate::True), "unknown: an unknown id answers default's nodes");
        sink.Check(m.FindUser(Id('9')) == nullptr, "unknown: asking creates no user");
        m.SetNode(HolderKind::User, Id('9'), N("mine"));
        const auto after = c.Get(Id('9'), Ctx(), kNow);
        sink.Check(Is(*after, "mine", Tristate::True), "unknown: the same Get sees a grant that creates the user");
    }
}

void QueryCases(CheckSink& sink) {
    {
        Model m;
        m.SetNode(HolderKind::User, Id('1'), N("one.two"));
        Checker c(m);
        const auto r = c.Get(Id('1'), Ctx(), kNow);
        sink.Check(Is(*r, "ONE.TWO", Tristate::True, "one.two"), "query: upper case answers as lower case");
    }
    {
        Model m;
        m.SetNode(HolderKind::User, Id('1'), N(".*"));
        Checker c(m);
        const auto r = c.Get(Id('1'), Ctx(), kNow);
        sink.Check(Is(*r, "x.y", Tristate::Undefined) && Is(*r, ".*", Tristate::True),
                   "query: a .* node is a plain key");
    }
    {
        Model m;
        m.SetNode(HolderKind::User, Id('1'), N("a.*"));
        Checker c(m);
        const auto r = c.Get(Id('1'), Ctx(), kNow);
        const std::string longQuery = "a." + std::string(298, 'x');
        sink.Check(Is(*r, longQuery.c_str(), Tristate::True, "a.*"), "query: a 300-byte query reaches the wildcard");
    }
}

void ImmutabilityCases(CheckSink& sink) {
    Model m;
    m.SetNode(HolderKind::User, Id('1'), N("before"));
    Checker c(m);
    const auto r = c.Get(Id('1'), Ctx(), kNow);
    m.SetNode(HolderKind::User, Id('1'), N("after"));
    m.SetNode(HolderKind::User, Id('1'), N("before", false));
    sink.Check(Is(*r, "before", Tristate::True) && Is(*r, "after", Tristate::Undefined),
               "immutable: a Resolved answers as built after the model changed");
}

}  // namespace

void RunResolutionCases(CheckSink& sink) {
    DirectCases(sink);
    WildcardCases(sink);
    InheritedNodesCases(sink);
    FlatteningCases(sink);
    OwnerCases(sink);
    ExpiryCases(sink);
    CacheCases(sink);
    QueryCases(sink);
    ImmutabilityCases(sink);
}

}  // namespace coop::permissions
