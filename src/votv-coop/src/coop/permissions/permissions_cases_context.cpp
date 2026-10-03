// coop/permissions/permissions_cases_context.cpp -- the checks of coop/permissions/context_set.h:
// LuckPerms' ImmutableContextSetTest and ContextSetComparatorTest rows, then the cases this port adds.

#include "coop/permissions/context_set.h"
#include "coop/permissions/permissions_selftest.h"

#include <algorithm>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

namespace coop::permissions {
namespace {

using Pair = std::pair<const char*, const char*>;
using Pairs = std::vector<std::pair<std::string, std::string>>;

ContextSet Make(std::initializer_list<Pair> pairs) {
    ContextSet s;
    for (const Pair& p : pairs) s.Add(p.first, p.second);
    return s;
}

bool LessSpecific(const ContextSet& x, const ContextSet& y) { return CompareSpecificity(x, y) < 0; }
bool MoreSpecific(const ContextSet& x, const ContextSet& y) { return CompareSpecificity(x, y) > 0; }

void BuilderCases(CheckSink& sink) {
    // testBuilder: five add orders on key `test` give the same three pairs.
    const std::vector<std::vector<const char*>> orders = {
        {"a", "b", "c"}, {"c", "b", "a"}, {"b", "a", "c"}, {"b", "c", "a"}, {"a", "a", "b", "c"}};
    for (const auto& order : orders) {
        ContextSet s;
        for (const char* v : order) s.Add("test", v);
        const bool same = s.Pairs() == Pairs{{"test", "a"}, {"test", "b"}, {"test", "c"}} &&
                          s.Contains("test", "a") && s.Contains("test", "b") && s.Contains("test", "c");
        sink.Check(same, "context: an add order gives the same three pairs");
        sink.Check(s.Size() == 3, "context: duplicates collapse, size 3");
    }

    // testContains
    {
        ContextSet s;
        for (const char* v : {"a", "a", "b", "c"}) s.Add("test", v);
        sink.Check(s.Contains("test", "a"), "context: contains a present pair");
        sink.Check(!s.Contains("test", "z"), "context: not a missing value");
        sink.Check(!s.Contains("aaa", "a"), "context: not a missing key");
        sink.Check(s.ContainsKey("test"), "context: contains a present key");
        sink.Check(!s.ContainsKey("aaa"), "context: not a missing key (key query)");
    }
}

void SatisfiedCases(CheckSink& sink) {
    // The node's set: aaa in {a,b,c}, bbb in {a,b}.
    const ContextSet node = Make({{"aaa", "a"}, {"aaa", "b"}, {"aaa", "c"}, {"bbb", "a"}, {"bbb", "b"}});
    // testContainsAllTrue
    sink.Check(node.IsSatisfiedBy(Make({{"aaa", "a"}, {"bbb", "a"}})), "context: aaa=a bbb=a satisfies");
    sink.Check(node.IsSatisfiedBy(Make({{"aaa", "b"}, {"bbb", "a"}})), "context: aaa=b bbb=a satisfies");
    sink.Check(node.IsSatisfiedBy(Make({{"aaa", "c"}, {"bbb", "a"}})), "context: aaa=c bbb=a satisfies");
    sink.Check(node.IsSatisfiedBy(Make({{"aaa", "c"}, {"bbb", "b"}})), "context: aaa=c bbb=b satisfies");
    // testContainsAllFalse
    sink.Check(!node.IsSatisfiedBy(Make({{"aaa", "a"}, {"bbb", "z"}})), "context: bbb=z does not satisfy");
    sink.Check(!node.IsSatisfiedBy(Make({{"aaa", "b"}, {"bbb", "z"}})), "context: aaa=b bbb=z does not");
    sink.Check(!node.IsSatisfiedBy(Make({{"aaa", "b"}})), "context: a missing key does not satisfy");
    sink.Check(!node.IsSatisfiedBy(Make({{"aaa", "c"}})), "context: a missing key does not satisfy (c)");
    sink.Check(!node.IsSatisfiedBy(ContextSet{}), "context: the empty subject does not satisfy");
}

void OursCases(CheckSink& sink) {
    const ContextSet empty;
    sink.Check(empty.IsSatisfiedBy(ContextSet{}), "context: an empty node set applies to the empty subject");
    sink.Check(empty.IsSatisfiedBy(Make({{"aaa", "a"}})), "context: an empty node set applies to any subject");

    {
        ContextSet s;
        sink.Check(s.Add("Server", "Global") && s.Empty(), "context: server=global is dropped, accepted");
    }
    {
        ContextSet s;
        sink.Check(s.Add("WORLD", "global") && s.Empty(), "context: world=global is dropped, accepted");
    }
    {
        ContextSet s;
        s.Add("Foo", "BaR");
        sink.Check(s.Size() == 1 && s.Contains("foo", "bar"), "context: key and value are lowered");
    }

    struct Bad {
        const char* key;
        const char* value;
        const char* what;
    };
    const Bad bad[] = {
        {"world", "", "context: an empty value is refused"},
        {"", "x", "context: an empty key is refused"},
        {" ", "x", "context: an all-space key is refused"},
        {"a", "b\x01", "context: a control byte in a value is refused"},
        {"a", "b\x7f", "context: DEL in a value is refused"},
        {"a", " ", "context: an all-space value is refused"},
    };
    for (const Bad& b : bad) {
        ContextSet s;
        sink.Check(!s.Add(b.key, b.value) && s.Empty(), b.what);
    }
}

void ComparatorCases(CheckSink& sink) {
    const ContextSet kEmpty;
    const ContextSet kJustServer = Make({{"server", "foo"}});
    const ContextSet kJustWorld = Make({{"world", "foo"}});
    const ContextSet kServerAndWorld = Make({{"server", "foo"}, {"world", "foo"}});
    const ContextSet kMisc = Make({{"foo", "foo"}, {"foo", "bar"}});
    const ContextSet kMisc2 = Make({{"foo", "foo"}, {"foo", "bar"}, {"bar", "foo"}});
    const ContextSet kServerAndMisc = Make({{"server", "foo"}, {"foo", "foo"}, {"foo", "bar"}});
    const ContextSet kWorldAndMisc = Make({{"world", "foo"}, {"foo", "foo"}, {"foo", "bar"}});
    const ContextSet kSwm1 = Make({{"server", "foo"}, {"world", "foo"}, {"foo", "foo"}});
    const ContextSet kSwm2 = Make({{"server", "foo"}, {"world", "foo"}, {"foo", "foo"}, {"foo", "bar"}});
    const std::vector<const ContextSet*> all = {&kEmpty,         &kJustServer,    &kJustWorld,
                                                &kServerAndWorld, &kMisc,          &kMisc2,
                                                &kServerAndMisc, &kWorldAndMisc,  &kSwm1,
                                                &kSwm2};

    // testEquals
    for (const ContextSet* s : all) sink.Check(CompareSpecificity(*s, *s) == 0, "comparator: a set equals itself");
    // testEmpty
    sink.Check(CompareSpecificity(kJustServer, kEmpty) > 0, "comparator: non-empty above empty");
    sink.Check(CompareSpecificity(kEmpty, kJustServer) < 0, "comparator: empty below non-empty");
    // testServerPresence
    sink.Check(CompareSpecificity(kJustServer, kMisc) > 0, "comparator: server above misc");
    sink.Check(CompareSpecificity(kJustServer, kWorldAndMisc) > 0, "comparator: server above world+misc");
    // testWorldPresence
    sink.Check(CompareSpecificity(kJustWorld, kMisc) > 0, "comparator: world above misc");
    // testOverallSize
    sink.Check(CompareSpecificity(kServerAndMisc, kJustServer) > 0, "comparator: server+misc above server");
    sink.Check(CompareSpecificity(kWorldAndMisc, kJustWorld) > 0, "comparator: world+misc above world");
    // testOverallSizeAll
    for (const ContextSet* s : all) {
        if (s == &kSwm2) continue;
        sink.Check(CompareSpecificity(kSwm2, *s) > 0, "comparator: server+world+misc(2) above every other");
    }
    // testTransitivity: the reversed sort is the reverse of the ascending sort.
    const std::vector<std::vector<ContextSet>> rows = {
        {Make({{"a", "-"}}), Make({{"b", "-"}}), Make({{"c", "-"}})},
        {Make({{"-", "a"}}), Make({{"-", "b"}}), Make({{"-", "c"}})},
    };
    for (const auto& row : rows) {
        std::vector<ContextSet> asc = row, desc = row;
        std::sort(asc.begin(), asc.end(), LessSpecific);
        std::sort(desc.begin(), desc.end(), MoreSpecific);
        std::reverse(asc.begin(), asc.end());
        sink.Check(asc == desc, "comparator: the reversed sort reverses the ascending sort");
    }
    // testPriorityOrdering
    {
        std::vector<ContextSet> asc = {kEmpty, kJustServer, kJustWorld, kMisc, kMisc2};
        std::sort(asc.begin(), asc.end(), LessSpecific);
        const std::vector<ContextSet> want = {kEmpty, kMisc, kMisc2, kJustWorld, kJustServer};
        sink.Check(asc == want, "comparator: ascending priority order");
        std::vector<ContextSet> desc = asc;
        std::sort(desc.begin(), desc.end(), MoreSpecific);
        std::vector<ContextSet> wantDesc = want;
        std::reverse(wantDesc.begin(), wantDesc.end());
        sink.Check(desc == wantDesc, "comparator: descending priority order");
    }
}

}  // namespace

void RunContextCases(CheckSink& sink) {
    BuilderCases(sink);
    SatisfiedCases(sink);
    OursCases(sink);
    ComparatorCases(sink);
}

}  // namespace coop::permissions
