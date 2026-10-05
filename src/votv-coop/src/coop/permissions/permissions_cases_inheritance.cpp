// coop/permissions/permissions_cases_inheritance.cpp -- the checks of coop/permissions/model.h and
// inheritance.h: LuckPerms' InheritanceComparatorTest, InheritanceTest and TraversalAlgorithmTest
// rows, then the cases this port adds (the default group, lapsed and deleted groups, cycles).

#include "coop/permissions/inheritance.h"
#include "coop/permissions/model.h"
#include "coop/permissions/permissions_selftest.h"

#include <cstdint>
#include <initializer_list>
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

void Group(Model& m, const char* name, int weight = 0, std::initializer_list<const char*> parents = {}) {
    m.CreateGroup(name);
    if (weight != 0) m.SetNode(HolderKind::Group, name, N("weight." + std::to_string(weight)));
    for (const char* p : parents) m.SetNode(HolderKind::Group, name, N(std::string("group.") + p));
}

std::string Names(const std::vector<const Holder*>& v) {
    std::string s;
    for (const Holder* h : v) {
        if (!s.empty()) s += ",";
        s += h->name == Id('1') ? std::string("root") : h->name;
    }
    return s;
}

bool Has(const Holder* h, const char* key) {
    if (h == nullptr) return false;
    for (const Node& n : h->nodes.Nodes()) {
        if (n.key == key) return true;
    }
    return false;
}

std::string ParentsOf(const Model& m, const Holder* h, const ContextSet& subject = Ctx()) {
    std::vector<const Holder*> out;
    Parents(m, *h, subject, kNow, out);
    return Names(out);
}

std::string OrderOf(const Model& m, const Holder* h) {
    std::vector<const Holder*> out;
    InheritanceOrder(m, *h, Ctx(), kNow, out);
    return Names(out);
}

// V3: the user `root` holds the three groups; the input list is sorted with the user as comparing.
void ComparatorCases(CheckSink& sink) {
    struct Row {
        const char* primary;
        int vipW, adminW, modW;
        std::vector<const char*> input;
        const char* expected;
        const char* what;
    };
    const Row rows[] = {
        {"admin", 0, 0, 0, {"admin", "moderator", "root"}, "root,admin,moderator", "inheritance: a user is always first"},
        {"admin", 0, 0, 0, {"vip", "admin", "moderator"}, "admin,vip,moderator", "inheritance: the primary group comes first at equal weight"},
        {"vip", 3, 10, 5, {"vip", "admin", "moderator"}, "admin,moderator,vip", "inheritance: weight beats the primary group"},
    };
    for (const Row& r : rows) {
        Model m;
        Group(m, "vip", r.vipW);
        Group(m, "admin", r.adminW);
        Group(m, "moderator", r.modW);
        for (const char* g : {"group.admin", "group.moderator", "group.vip"}) m.SetNode(HolderKind::User, Id('1'), N(g));
        m.SetPrimaryGroup(Id('1'), r.primary);
        const Holder* root = m.FindUser(Id('1'));
        std::vector<const Holder*> v;
        for (const char* name : r.input) {
            v.push_back(std::string(name) == "root" ? root : m.FindGroup(name));
        }
        SortByInheritance(m, v, *root, kNow);
        sink.Check(Names(v) == r.expected, r.what);
    }
}

void TraversalCases(CheckSink& sink) {
    // V4a: owner(13) > admin(12) > mod(11) > helper(10) > member(0); vip+(6) > vip(5) > member.
    {
        Model m;
        Group(m, "member");
        Group(m, "helper", 10, {"member"});
        Group(m, "mod", 11, {"helper"});
        Group(m, "admin", 12, {"mod"});
        Group(m, "owner", 13, {"admin"});
        Group(m, "vip", 5, {"member"});
        Group(m, "vip+", 6, {"vip"});
        Group(m, "test", 0, {"owner", "vip+"});
        sink.Check(OrderOf(m, m.FindGroup("test")) == "owner,admin,mod,helper,member,vip+,vip",
                   "traversal: depth-first pre-order of the seven-group graph");
    }
    // V5: root -> [a, b], a -> [a1, a2], b -> [b1, b2].
    {
        Model m;
        Group(m, "a1");
        Group(m, "a2");
        Group(m, "b1");
        Group(m, "b2");
        Group(m, "a", 0, {"a1", "a2"});
        Group(m, "b", 0, {"b1", "b2"});
        Group(m, "root", 0, {"a", "b"});
        sink.Check(OrderOf(m, m.FindGroup("root")) == "a,a1,a2,b,b1,b2", "traversal: pre-order of a two-level tree");
    }
}

void DefaultGroupCases(CheckSink& sink) {
    // One fresh model with group vip, one fresh user.
    {
        Model m;
        m.CreateGroup("vip");
        const std::string u = Id('5');
        m.SetNode(HolderKind::User, u, N("group.vip"));
        sink.Check(ParentsOf(m, m.FindUser(u)) == "default,vip", "default: a first grant leaves the user in default and vip");
        m.UnsetNode(HolderKind::User, u, N("group.default"));
        sink.Check(ParentsOf(m, m.FindUser(u)) == "vip", "default: the stored rule sees vip once default is removed");
        m.UnsetNode(HolderKind::User, u, N("group.vip"));
        sink.Check(ParentsOf(m, m.FindUser(u)) == "default", "default: the last group removed gives default back");
    }
    {
        Model m;
        m.SetNode(HolderKind::User, Id('6'), N("test"));
        const Holder* u = m.FindUser(Id('6'));
        sink.Check(Has(u, "group.default"), "default: a created user holds group.default");
        sink.Check(u != nullptr && u->primaryGroup == "default", "default: a created user's primary is default");
    }
    // A temporary global group that lapsed.
    {
        Model m;
        m.CreateGroup("vip");
        const std::string u = Id('7');
        m.SetNode(HolderKind::User, u, N("group.vip", true, kNow - 1));
        m.UnsetNode(HolderKind::User, u, N("group.default"));
        sink.Check(ParentsOf(m, m.FindUser(u)) == "default", "default: a lapsed temporary group leaves the user in default");
    }
    // A global group that was deleted.
    {
        Model m;
        m.CreateGroup("gone");
        const std::string u = Id('8');
        m.SetNode(HolderKind::User, u, N("group.gone"));
        m.UnsetNode(HolderKind::User, u, N("group.default"));
        m.DeleteGroup("gone");
        sink.Check(ParentsOf(m, m.FindUser(u)) == "default", "default: a deleted group leaves the user in default");
    }
}

void GraphShapeCases(CheckSink& sink) {
    // A cycle x -> y -> x, a user inheriting x.
    {
        Model m;
        Group(m, "x", 0, {"y"});
        Group(m, "y", 0, {"x"});
        m.SetNode(HolderKind::User, Id('9'), N("group.x"));
        m.UnsetNode(HolderKind::User, Id('9'), N("group.default"));
        sink.Check(OrderOf(m, m.FindUser(Id('9'))) == "x,y", "graph: a cycle ends at the visited holder");
    }
    // A self-edge z -> z.
    {
        Model m;
        Group(m, "z", 0, {"z"});
        m.SetNode(HolderKind::User, Id('9'), N("group.z"));
        m.UnsetNode(HolderKind::User, Id('9'), N("group.default"));
        sink.Check(OrderOf(m, m.FindUser(Id('9'))) == "z", "graph: a self-edge is walked once");
    }
    // A group that does not exist, a false inheritance, a context parent.
    {
        Model m;
        m.CreateGroup("vip");
        m.SetNode(HolderKind::User, Id('a'), N("group.ghost"));
        sink.Check(ParentsOf(m, m.FindUser(Id('a'))) == "default", "graph: a group the model lacks is skipped");
        m.SetNode(HolderKind::User, Id('b'), N("group.vip", false));
        sink.Check(ParentsOf(m, m.FindUser(Id('b'))) == "default", "graph: a false group node names no parent");
        m.SetNode(HolderKind::User, Id('c'), N("group.vip", true, 0, Ctx({{"server", "a"}})));
        sink.Check(ParentsOf(m, m.FindUser(Id('c'))) == "default", "graph: a context parent is absent without its context");
        sink.Check(ParentsOf(m, m.FindUser(Id('c')), Ctx({{"server", "a"}})) == "default,vip",
                   "graph: a context parent is present under its context");
    }
}

void PrimaryCases(CheckSink& sink) {
    Model m;
    m.CreateGroup("admin");
    m.CreateGroup("vip");
    const std::string u = Id('d');
    m.SetNode(HolderKind::User, u, N("group.vip"));
    m.UnsetNode(HolderKind::User, u, N("group.default"));
    m.SetPrimaryGroup(u, "admin");
    sink.Check(EffectivePrimary(m, *m.FindUser(u), kNow) == "vip", "primary: a stored primary outside the groups gives the first group");
    // A user whose only group node is a false group.default has no global group; its stored
    // primary is another group, so `default` can only come from the no-global-group branch.
    const std::string v = Id('3');
    m.SetNode(HolderKind::User, v, N("test"));
    m.SetPrimaryGroup(v, "admin");
    m.SetNode(HolderKind::User, v, N("group.default", false));
    sink.Check(EffectivePrimary(m, *m.FindUser(v), kNow) == "default", "primary: no global group gives default");
}

void ModelCases(CheckSink& sink) {
    Model m;
    sink.Check(m.CreateGroup("vip") && !m.CreateGroup("vip"), "model: CreateGroup creates once");
    sink.Check(!m.SetNode(HolderKind::Group, "nothere", N("test")), "model: SetNode on a missing group is refused");
    const std::string u = Id('e');
    sink.Check(m.SetNode(HolderKind::User, u, N("test")) && m.UserRevision(u) == 1 && Has(m.FindUser(u), "test") &&
                   Has(m.FindUser(u), "group.default"),
               "model: SetNode on a missing user creates it, revision 1, with the node and default");
    const std::string missing = Id('f');
    sink.Check(!m.UnsetNode(HolderKind::User, missing, N("test")) && m.FindUser(missing) == nullptr,
               "model: UnsetNode on a missing user creates nothing");
    sink.Check(!m.SetPrimaryGroup(missing, "vip") && m.FindUser(missing) == nullptr,
               "model: SetPrimaryGroup on a missing user creates nothing");
    sink.Check(!m.SetPrimaryGroup(u, "nothere"), "model: SetPrimaryGroup to a missing group is refused");
    sink.Check(!m.DeleteGroup("default"), "model: the default group cannot be deleted");

    // A refused key is refused before the missing user is made; a name that is not 32 hex makes none.
    const std::string fresh = Id('9');
    sink.Check(!m.SetNode(HolderKind::User, fresh, N("group.hello world")) && m.FindUser(fresh) == nullptr &&
                   !m.SetNode(HolderKind::User, "not-hex", N("test")) && m.FindUser("not-hex") == nullptr,
               "model: a refused key or user name creates no user");
    const uint64_t refusedRev = m.GroupsRevision();
    const size_t vipNodes = m.FindGroup("vip")->nodes.Size();
    sink.Check(!m.SetNode(HolderKind::Group, "vip", N("group.hello world")) && m.GroupsRevision() == refusedRev &&
                   m.FindGroup("vip")->nodes.Size() == vipNodes,
               "model: a refused key on a group changes nothing");

    // Revisions.
    const uint64_t g0 = m.GroupsRevision();
    const uint64_t u0 = m.UserRevision(u);
    const std::string other = Id('1');
    m.SetNode(HolderKind::User, other, N("x"));
    const uint64_t other0 = m.UserRevision(other);
    m.SetNode(HolderKind::Group, "vip", N("test"));
    sink.Check(m.GroupsRevision() == g0 + 1 && m.UserRevision(u) == u0, "model: a group change bumps the groups revision only");
    m.SetNode(HolderKind::User, u, N("another"));
    sink.Check(m.GroupsRevision() == g0 + 1 && m.UserRevision(u) == u0 + 1 && m.UserRevision(other) == other0,
               "model: a user change bumps that user's revision only");
    m.SetNode(HolderKind::Group, "vip", N("test"));
    m.SetNode(HolderKind::User, u, N("another"));
    sink.Check(m.GroupsRevision() == g0 + 1 && m.UserRevision(u) == u0 + 1, "model: a no-op Set bumps nothing");
}

void OrderCases(CheckSink& sink) {
    // Equal-weight parents of a group keep store order; of a user the effective primary first.
    Model m;
    Group(m, "a");
    Group(m, "b");
    m.CreateGroup("g");
    m.SetNode(HolderKind::Group, "g", N("group.b"));
    m.SetNode(HolderKind::Group, "g", N("group.a"));
    sink.Check(ParentsOf(m, m.FindGroup("g")) == "a,b", "order: a group's equal-weight parents keep store order");
    const std::string u = Id('2');
    m.SetNode(HolderKind::User, u, N("group.a"));
    m.SetNode(HolderKind::User, u, N("group.b"));
    m.UnsetNode(HolderKind::User, u, N("group.default"));
    m.SetPrimaryGroup(u, "b");
    sink.Check(ParentsOf(m, m.FindUser(u)) == "b,a", "order: a user's effective primary comes first");
}

void WeightCases(CheckSink& sink) {
    Model m;
    m.CreateGroup("w1");
    m.SetNode(HolderKind::Group, "w1", N("weight.5", true, 0, Ctx({{"server", "a"}})));
    sink.Check(WeightOf(*m.FindGroup("w1"), kNow) == 5, "weight: a contextual weight counts under any subject");
    m.CreateGroup("w2");
    m.SetNode(HolderKind::Group, "w2", N("weight.9", true, kNow));
    sink.Check(WeightOf(*m.FindGroup("w2"), kNow) == 9 && WeightOf(*m.FindGroup("w2"), kNow + 1) == 0,
               "weight: an expired weight does not count");

    // CurrentWeight tells "none" apart from zero: the highest applying weight, contexts ignored.
    int weight = -1;
    m.CreateGroup("w3");
    sink.Check(!CurrentWeight(*m.FindGroup("w3"), kNow, &weight) && weight == -1,
               "weight: a group with no weight node has none");
    m.SetNode(HolderKind::Group, "w3", N("weight.5"));
    m.SetNode(HolderKind::Group, "w3", N("weight.10", true, kNow));
    m.SetNode(HolderKind::Group, "w3", N("weight.7", true, 0, Ctx({{"server", "a"}})));
    sink.Check(CurrentWeight(*m.FindGroup("w3"), kNow, &weight) && weight == 10 &&
                   CurrentWeight(*m.FindGroup("w3"), kNow + 1, &weight) && weight == 7,
               "weight: the highest weight that applies, a context ignored and an expired one left out");
    m.CreateGroup("w4");
    m.SetNode(HolderKind::Group, "w4", N("weight.0"));
    sink.Check(CurrentWeight(*m.FindGroup("w4"), kNow, &weight) && weight == 0,
               "weight: a weight of zero is a weight");
}

}  // namespace

void RunInheritanceCases(CheckSink& sink) {
    ComparatorCases(sink);
    TraversalCases(sink);
    DefaultGroupCases(sink);
    GraphShapeCases(sink);
    PrimaryCases(sink);
    ModelCases(sink);
    OrderCases(sink);
    WeightCases(sink);
}

}  // namespace coop::permissions
