// coop/permissions/permissions_cases_store.cpp -- the checks of coop/permissions/node.h and
// node_map.h: LuckPerms' node parsing, NodeMapTest and NodeComparatorTest rows, then the cases this
// port adds (key limits, group names, wildcards, expiry).

#include "coop/permissions/node.h"
#include "coop/permissions/node_map.h"
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
constexpr int64_t kDay = 86400;

ContextSet Ctx(std::initializer_list<Pair> pairs = {}) {
    ContextSet s;
    for (const Pair& p : pairs) s.Add(p.first, p.second);
    return s;
}

Node N(const char* key, bool value = true, int64_t expiry = 0, ContextSet contexts = {}) {
    Node n;
    n.key = key;
    n.value = value;
    n.expiry = expiry;
    n.contexts = std::move(contexts);
    return n;
}

std::string Norm(const char* in) {
    std::string out;
    return NormalizeKey(in, &out) ? out : std::string("<refused>");
}

bool KeysAre(const std::vector<const Node*>& v, std::initializer_list<const char*> keys) {
    if (v.size() != keys.size()) return false;
    size_t i = 0;
    for (const char* k : keys) {
        if (v[i++]->key != k) return false;
    }
    return true;
}

void ParseCases(CheckSink& sink) {
    // NodeParseTest / NodeBuildersTest
    sink.Check(KindOf(Norm("group.test")) == NodeKind::Inheritance && GroupOf(Norm("group.test")) == "test",
               "node: group.test is the inheritance of `test`");
    sink.Check(Norm("group.TEST") == "group.test" && GroupOf(Norm("group.TEST")) == "test",
               "node: group.TEST names the group `test`");
    sink.Check(Norm("group.hello world") == "<refused>", "node: a group name with a space is refused");
    int w = -1;
    sink.Check(WeightValue("weight.100", &w) && w == 100 && KindOf("weight.100") == NodeKind::Weight,
               "node: weight.100 is 100");
    sink.Check(WeightValue("weight.-100", &w) && w == -100, "node: weight.-100 is -100");
    sink.Check(WeightValue("weight.0", &w) && w == 0, "node: weight.0 is 0");
    sink.Check(WeightValue("weight.+100", &w) && w == 100 && KindOf("weight.+100") == NodeKind::Weight,
               "node: weight.+100 is 100");
    sink.Check(KindOf("weight.+-5") == NodeKind::Permission && !WeightValue("weight.+-5", &w),
               "node: weight.+-5 is a permission");
    sink.Check(KindOf("weight.--5") == NodeKind::Permission && !WeightValue("weight.--5", &w),
               "node: weight.--5 is a permission");
    sink.Check(KindOf("weight.") == NodeKind::Permission, "node: weight. is a permission");
    sink.Check(KindOf("weight.hello") == NodeKind::Permission, "node: weight.hello is a permission");
    sink.Check(KindOf("aaaa") == NodeKind::Permission, "node: aaaa is a permission");
    sink.Check(KindOf("luckperms.user.info") == NodeKind::Permission, "node: luckperms.user.info is a permission");
    sink.Check(KindOf("group.default") == NodeKind::Inheritance, "node: group.default is an inheritance");
    sink.Check(KindOf("weight.10") == NodeKind::Weight, "node: weight.10 is a weight");
}

void KeyCases(CheckSink& sink) {
    std::string out;
    sink.Check(!NormalizeKey("group.con", &out), "node: group.con (a device name) is refused");
    sink.Check(!NormalizeKey("group.LPT9", &out), "node: group.LPT9 (a device name) is refused");
    sink.Check(!NormalizeKey(std::string(201, 'a'), &out), "node: a 201-byte key is refused");
    sink.Check(!NormalizeKey("a b", &out), "node: a key with a space is refused");
    sink.Check(!NormalizeKey("a\x7f" "b", &out), "node: a key with DEL is refused");

    sink.Check(IsValidGroupName("vip+"), "node: vip+ is a valid group name");
    sink.Check(!IsValidGroupName("nul"), "node: nul is not a valid group name");
    sink.Check(IsValidGroupName(std::string(36, 'a')), "node: a 36-character group name is valid");
    sink.Check(!IsValidGroupName(std::string(37, 'a')), "node: a 37-character group name is not");

    sink.Check(NormalizeKey(std::string(200, 'a'), &out) && out.size() == 200, "node: a 200-byte key is accepted");

    sink.Check(IsWildcard("*"), "node: * is a wildcard");
    sink.Check(IsWildcard("a.*"), "node: a.* is a wildcard");
    sink.Check(IsWildcard("a.b.*"), "node: a.b.* is a wildcard");
    sink.Check(!IsWildcard(".*"), "node: .* alone is a plain permission");
    sink.Check(!IsWildcard("a"), "node: a is not a wildcard");
    sink.Check(!IsWildcard("a*"), "node: a* is not a wildcard");

    const Node temp = N("t", true, 500);
    sink.Check(Applies(temp, 499), "node: a temporary node applies before its second");
    sink.Check(Applies(temp, 500), "node: a temporary node applies at its second");
    sink.Check(!Applies(temp, 501), "node: a temporary node stops applying after its second");
    sink.Check(Applies(N("p"), 999'999'999), "node: a permanent node always applies");
}

void SimpleStoreCases(CheckSink& sink) {
    // NodeMapTest 6a
    {
        NodeMap m;
        const Node n = N("test");
        sink.Check(m.Set(n), "store: Set of a new node changes the store");
        sink.Check(m.Size() == 1, "store: size 1 after Set");
        sink.Check(m.Unset(n), "store: Unset of the node removes it");
        sink.Check(m.Size() == 0, "store: size 0 after Unset");
    }
    // 6c: a different value replaces
    {
        struct Row { const char* key; bool first; bool second; };
        const Row rows[] = {{"test", true, false}, {"test", false, true}, {"group.test", true, false}, {"group.test", false, true}};
        for (const Row& r : rows) {
            NodeMap m;
            m.Set(N(r.key, r.first));
            const bool changed = m.Set(N(r.key, r.second));
            sink.Check(changed && m.Size() == 1, "store: a node of another value replaces the first");
            sink.Check(m.Nodes()[0].value == r.second, "store: the stored value is the second's");
        }
    }
    // 6d: a different expiry instant replaces
    {
        struct Row { const char* key; int firstDays; int secondDays; };
        const Row rows[] = {{"test", 1, 5}, {"test", 5, 1}, {"group.test", 1, 5}, {"group.test", 5, 1}};
        for (const Row& r : rows) {
            NodeMap m;
            m.Set(N(r.key, true, kNow + r.firstDays * kDay));
            const bool changed = m.Set(N(r.key, true, kNow + r.secondDays * kDay));
            sink.Check(changed && m.Size() == 1, "store: a node of another expiry replaces the first");
            sink.Check(m.Nodes()[0].expiry == kNow + r.secondDays * kDay, "store: the stored expiry is the second's");
        }
    }
    // 6e
    {
        NodeMap m;
        m.Set(N("test1"));
        m.Set(N("test2", false));
        m.Set(N("test3", true, kNow + 3600));
        m.Set(N("test4", true, 0, Ctx({{"hello", "world"}})));
        sink.Check(m.Size() == 4, "store: four nodes of four identities");
        sink.Check(!m.Unset(N("test1", true, 0, Ctx({{"hello", "world"}}))) && m.Size() == 4,
                   "store: Unset of another context removes nothing");
        sink.Check(!m.Unset(N("test3")) && m.Size() == 4, "store: Unset of the permanent twin of a temporary removes nothing");
        sink.Check(!m.Unset(N("test4")) && m.Size() == 4, "store: Unset of the global twin of a contextual node removes nothing");
        sink.Check(!m.Unset(N("test4", true, 0, Ctx({{"hello", "world"}, {"aaa", "bbb"}}))) && m.Size() == 4,
                   "store: Unset of a wider context removes nothing");
        sink.Check(!m.Unset(N("test5")) && m.Size() == 4, "store: Unset of an unknown key removes nothing");
        sink.Check(m.Unset(N("test1")), "store: Unset of test1 removes it");
        sink.Check(m.Unset(N("test2", true)), "store: Unset ignores the value");
        sink.Check(m.Unset(N("test3", true, kNow + 7200)), "store: Unset ignores the expiry instant");
        sink.Check(m.Unset(N("test4", true, 0, Ctx({{"hello", "world"}}))), "store: Unset of the contextual node removes it");
        sink.Check(m.Size() == 0, "store: size 0 after the four Unsets");
    }
    // 6g
    {
        NodeMap m;
        m.Set(N("a"));
        m.Set(N("b"));
        m.Set(N("c"));
        m.Clear();
        sink.Check(m.Size() == 0, "store: Clear empties the store");
    }
}

void ApplicableCases(CheckSink& sink) {
    std::vector<const Node*> out;
    // 6o
    {
        NodeMap m;
        m.Set(N("test1"));
        m.Set(N("group.test2", true, 0, Ctx({{"server", "test"}})));
        m.Applicable(Ctx(), kNow, out);
        sink.Check(KeysAre(out, {"test1"}), "store: the empty subject sees the global node");
        m.Applicable(Ctx({{"server", "test"}}), kNow, out);
        sink.Check(KeysAre(out, {"group.test2", "test1"}), "store: server=test sees both, the more specific first");
        m.Applicable(Ctx({{"world", "test"}}), kNow, out);
        sink.Check(KeysAre(out, {"test1"}), "store: world=test sees the global node only");
    }
    // 6p
    {
        NodeMap m;
        const ContextSet sets[4] = {Ctx(), Ctx({{"server", "test"}}), Ctx({{"world", "test"}}),
                                    Ctx({{"server", "test"}, {"world", "test"}})};
        const char* const plain[4] = {"test1", "test2", "test3", "test4"};
        const char* const group[4] = {"group.test1", "group.test2", "group.test3", "group.test4"};
        for (int i = 0; i < 4; ++i) {
            m.Set(N(plain[i], true, 0, sets[i]));
            m.Set(N(group[i], true, 0, sets[i]));
        }
        m.Applicable(Ctx(), kNow, out);
        sink.Check(out.size() == 2, "store: the empty subject sees 2 of 8");
        m.Applicable(Ctx({{"server", "test"}}), kNow, out);
        sink.Check(out.size() == 4, "store: server=test sees 4 of 8");
        m.Applicable(Ctx({{"world", "test"}}), kNow, out);
        sink.Check(out.size() == 4, "store: world=test sees 4 of 8");
        m.Applicable(Ctx({{"server", "test"}, {"world", "test"}}), kNow, out);
        sink.Check(out.size() == 8, "store: server+world sees 8 of 8");
        m.Applicable(Ctx({{"server", "test"}, {"world", "test"}, {"test", "test"}}), kNow, out);
        sink.Check(out.size() == 8, "store: extra subject keys change nothing");
    }
}

void OrderCases(CheckSink& sink) {
    std::vector<const Node*> out;
    const ContextSet subject = Ctx({{"server", "foo"}});
    // NodeComparatorTest: a temporary before a permanent of the same key.
    {
        NodeMap m;
        m.Set(N("hello.world"));
        m.Set(N("hello.world", true, kNow + 60));
        m.Applicable(subject, kNow, out);
        sink.Check(out.size() == 2 && out[0]->expiry != 0 && out[1]->expiry == 0,
                   "order: a temporary node before the permanent one of its key");
    }
    // Two temporaries of different keys: the earlier expiry first, before the key rule.
    {
        NodeMap m;
        m.Set(N("a", true, kNow + 120));
        m.Set(N("b", true, kNow + 60));
        m.Applicable(subject, kNow, out);
        sink.Check(KeysAre(out, {"b", "a"}), "order: temporaries by earlier expiry, before the key");
    }
    // Context specificity before node properties.
    {
        NodeMap m;
        m.Set(N("hello.world", true, kNow + 60));
        m.Set(N("hello.world", true, 0, Ctx({{"server", "foo"}})));
        m.Applicable(subject, kNow, out);
        sink.Check(out.size() == 2 && out[0]->expiry == 0 && !out[0]->contexts.Empty(),
                   "order: a permanent @server before a temporary global");
    }
    {
        NodeMap m;
        m.Set(N("hello.world"));
        m.Set(N("hello.world", true, 0, Ctx({{"server", "foo"}})));
        m.Set(N("hello.world", true, kNow + 60, Ctx({{"server", "foo"}})));
        m.Applicable(subject, kNow, out);
        sink.Check(out.size() == 3 && out[0]->expiry != 0 && !out[0]->contexts.Empty() &&
                       out[1]->expiry == 0 && !out[1]->contexts.Empty() && out[2]->contexts.Empty(),
                   "order: [temporary@server, @server, global]");
    }
}

void ExpiryCases(CheckSink& sink) {
    NodeMap m;
    m.Set(N("p"));
    m.Set(N("t1", true, kNow + 100));
    m.Set(N("t2", true, kNow + 200));
    sink.Check(m.NextExpiry(kNow) == kNow + 100, "expiry: the next expiry is the earlier temporary node's");
    sink.Check(m.NextExpiry(kNow + 150) == kNow + 200, "expiry: past the first, the second");
    sink.Check(m.NextExpiry(kNow + 201) == 0, "expiry: past both, none");
    std::vector<const Node*> out;
    m.Applicable(Ctx(), kNow + 150, out);
    sink.Check(KeysAre(out, {"t2", "p"}), "expiry: an expired node is absent from Applicable");
}

}  // namespace

void RunStoreCases(CheckSink& sink) {
    ParseCases(sink);
    KeyCases(sink);
    SimpleStoreCases(sink);
    ApplicableCases(sink);
    OrderCases(sink);
    ExpiryCases(sink);
}

}  // namespace coop::permissions
