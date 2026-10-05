// coop/permissions/permissions_cases_edit.cpp -- the checks of coop/permissions/permission_edit.h:
// an edit planned over the store's texts as read, the file text it yields, the candidate model the
// loader builds from the texts with that one replaced, the owner invariant, who refused, the
// problem lines a host answers and the action log's line.

#include "coop/permissions/permission_edit.h"
#include "coop/permissions/permissions_selftest.h"
#include "coop/permissions/resolution.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace coop::permissions {
namespace {

constexpr int64_t kNow = 1'700'000'000;

std::string Id(char c) { return std::string(32, c); }

// The host's own id in these cases: 32 x `f`.
const std::string& OwnerId() {
    static const std::string owner = Id('f');
    return owner;
}

Node N(const char* key, bool value = true) {
    Node n;
    n.key = key;
    n.value = value;
    return n;
}

ContextSet ListenSubject() {
    ContextSet s;
    s.Add("mode", "listen");
    return s;
}

bool Has(const std::string& text, std::string_view needle) { return text.find(needle) != std::string::npos; }

// A plan over `texts` for `key`, planned by the owner unless `delegate`.
EditPlan Plan(const std::vector<HolderText>& texts, const HolderKey& key,
              const std::function<bool(Model&, std::string*)>& change, bool delegate = false,
              const std::vector<std::string>& nodes = {}, const std::string& owner = OwnerId()) {
    return PlanEdit(texts, key, change, !delegate, owner, ListenSubject(), kNow, nodes);
}

bool Allows(const Model& m, const std::string& id, const char* node) {
    return Evaluate(*Resolve(m, id, ListenSubject(), kNow), node, false).value == Tristate::True;
}

void SetNodeCases(CheckSink& sink) {
    const HolderKey user{HolderKind::User, Id('a')};
    const auto set = [&](Model& m, std::string*) {
        m.SetNode(HolderKind::User, Id('a'), N("a.b"));
        return true;
    };
    const EditPlan first = Plan({}, user, set);
    const Holder* made = first.candidate.FindUser(Id('a'));
    sink.Check(first.result == EditResult::Changed && !first.deleteFile && made != nullptr &&
                   first.text == SerializeHolder(*made, kNow, true) && Allows(first.candidate, Id('a'), "a.b"),
               "edit: a node set on a user is a change, its text is the candidate's user pruned");

    const EditPlan again = Plan({{Id('a'), false, first.text}}, user, set);
    sink.Check(again.result == EditResult::NoChange, "edit: the same set over the text it wrote is no change");
}

void ExpiredCases(CheckSink& sink) {
    const std::string id = Id('b');
    const HolderKey user{HolderKind::User, id};
    const std::string expiredOnly =
        R"({"primaryGroup":"default","permissions":["group.default","x.y",{"permission":"old.node","expiry":1000}]})";
    Node old = N("old.node");
    old.expiry = 1000;
    const EditPlan unset = Plan({{id, false, expiredOnly}}, user, [&](Model& m, std::string*) {
        m.UnsetNode(HolderKind::User, id, old);
        return true;
    });
    sink.Check(unset.result == EditResult::Changed && !unset.deleteFile && !Has(unset.text, "old.node") &&
                   Has(unset.text, "x.y"),
               "edit: unsetting an expired node is a change and the text no longer has it");

    // The member holds an expired temporary parent beside group.default: deleting the group would
    // leave a file the next start refuses.
    const std::string member =
        R"({"primaryGroup":"default","permissions":["group.default",{"permission":"group.staff","expiry":1000}]})";
    const std::vector<HolderText> staffed = {{"staff", true, R"({"permissions":["staff.use"]})"},
                                             {id, false, member}};
    const EditPlan deleted = Plan(staffed, {HolderKind::Group, "staff"}, [](Model& m, std::string*) {
        m.DeleteGroup("staff");
        return true;
    });
    sink.Check(deleted.result == EditResult::Refused && Has(deleted.why, id) && Has(deleted.why, "staff") &&
                   !deleted.problems.empty(),
               "edit: deleting a group a file still names, even as an expired parent, is refused naming both");

    const std::string primaryOnly = R"({"primaryGroup":"staff","permissions":[{"permission":"group.staff","expiry":1000}]})";
    const EditPlan primary = Plan({{"staff", true, "{}"}, {id, false, primaryOnly}}, {HolderKind::Group, "staff"},
                                  [](Model& m, std::string*) {
                                      m.DeleteGroup("staff");
                                      return true;
                                  });
    sink.Check(primary.result == EditResult::Refused && Has(primary.why, "primary group"),
               "edit: deleting the group a user's stored primary names is refused");
}

void UntouchedCases(CheckSink& sink) {
    const std::string id = Id('c');
    const std::string untouched =
        R"({"primaryGroup":"staff","permissions":[{"permission":"group.staff","expiry":1000}]})";
    const std::vector<HolderText> texts = {{"staff", true, R"({"permissions":["staff.use"]})"}, {id, false, untouched}};
    const EditPlan plan = Plan(texts, {HolderKind::Group, "staff"}, [](Model& m, std::string*) {
        m.SetNode(HolderKind::Group, "staff", N("staff.more"));
        return true;
    });
    Model before;
    LoadHolders(texts, before);
    const Holder* was = before.FindUser(id);
    const Holder* after = plan.candidate.FindUser(id);
    sink.Check(plan.result == EditResult::Changed && was != nullptr && after != nullptr &&
                   SerializeHolder(*after, kNow, false) == SerializeHolder(*was, kNow, false),
               "edit: a holder the edit did not touch is the loader's reading of its file, as before");
}

void DeleteCases(CheckSink& sink) {
    const EditPlan group = Plan({{"staff", true, "{}"}}, {HolderKind::Group, "staff"}, [](Model& m, std::string*) {
        m.DeleteGroup("staff");
        return true;
    });
    sink.Check(group.result == EditResult::Changed && group.deleteFile && group.candidate.FindGroup("staff") == nullptr,
               "edit: a group with no member is deleted, its file goes");

    const std::string id = Id('d');
    const EditPlan user = Plan({{id, false, R"({"permissions":["group.default","only.node"]})"}},
                               {HolderKind::User, id}, [&](Model& m, std::string*) {
                                   m.UnsetNode(HolderKind::User, id, N("only.node"));
                                   return true;
                               });
    sink.Check(user.result == EditResult::Changed && user.deleteFile,
               "edit: a user left in the default state has no file, its file goes");

    const EditPlan parent = Plan({}, {HolderKind::User, Id('e')}, [&](Model& m, std::string*) {
        m.SetNode(HolderKind::User, Id('e'), N("group.default"));
        return true;
    });
    sink.Check(parent.result == EditResult::NoChange, "edit: a user with no file made default is no change");
}

void OwnerCases(CheckSink& sink) {
    const std::string owner = Id('f');
    const std::vector<std::string> nodes = {"multivoid.kick"};
    const auto denyOnDefault = [](Model& m, std::string*) {
        m.SetNode(HolderKind::Group, "default", N("multivoid.kick", false));
        return true;
    };
    const HolderKey defaultGroup{HolderKind::Group, "default"};
    const EditPlan delegate = Plan({}, defaultGroup, denyOnDefault, true, nodes);
    sink.Check(delegate.result == EditResult::OwnerLoses && delegate.why == "multivoid.kick",
               "owner: a delegate's deny on the default group is refused, naming the node");
    sink.Check(Plan({}, defaultGroup, denyOnDefault, false, nodes).result == EditResult::Changed,
               "owner: the owner's own edit of it is a change");

    const EditPlan self = Plan({}, {HolderKind::User, owner}, [&](Model& m, std::string*) {
        m.SetNode(HolderKind::User, owner, N("multivoid.kick", false));
        return true;
    }, true, nodes);
    sink.Check(self.result == EditResult::OwnerLoses, "owner: a delegate's deny on the owner's own user is refused");

    const EditPlan unknown = Plan({}, defaultGroup, denyOnDefault, true, nodes, std::string());
    sink.Check(unknown.result == EditResult::Refused && unknown.why == "The host's identity is not loaded.",
               "owner: a delegate's edit with no host identity is refused");
}

void RefusalCases(CheckSink& sink) {
    const EditPlan refused = Plan({}, {HolderKind::Group, "nowhere"}, [](Model&, std::string* why) {
        *why = "No such group.";
        return false;
    });
    sink.Check(refused.result == EditResult::Refused && refused.why == "No such group.",
               "edit: a change that refuses is refused with its own text");

    const EditPlan broken = Plan({{"a", true, R"({"parents":["ghost"]})"}}, {HolderKind::Group, "a"},
                                 [](Model&, std::string*) { return true; });
    sink.Check(broken.result == EditResult::Refused &&
                   broken.why.rfind("The permission files do not load: ", 0) == 0 && !broken.problems.empty(),
               "edit: texts that do not load refuse every edit, naming the loader's first problem");
}

// True when `text`, as the file of the user `id` beside `others`, loads with no problem and the loaded
// user serialises, pruned, to `text` again.
bool LoadsBackEqual(const std::vector<HolderText>& others, const std::string& id, const std::string& text,
                    std::string* primary) {
    std::vector<HolderText> texts = others;
    texts.push_back({id, false, text});
    Model m;
    if (!ShouldLoad(LoadHolders(texts, m))) return false;
    const Holder* h = m.FindUser(id);
    if (h == nullptr) return false;
    *primary = h->primaryGroup;
    return SerializeHolder(*h, kNow, true) == text;
}

// The text an edit writes is the loader's reading of the holder, not the copy's.
void LoaderReadingCases(CheckSink& sink) {
    const std::string id = Id('9');
    const std::vector<HolderText> gold = {{"gold", true, "{}"}};
    const auto unrelated = [&](Model& m, std::string*) {
        m.SetNode(HolderKind::User, id, N("a.b"));
        return true;
    };
    const std::string heldByExpired =
        R"({"primaryGroup":"gold","permissions":["x.y",{"permission":"group.gold","expiry":1000}]})";
    std::vector<HolderText> texts = gold;
    texts.push_back({id, false, heldByExpired});
    const EditPlan pruned = Plan(texts, {HolderKind::User, id}, unrelated);
    std::string primary;
    sink.Check(pruned.result == EditResult::Changed && !pruned.deleteFile &&
                   LoadsBackEqual(gold, id, pruned.text, &primary) && primary == "default" &&
                   Has(pruned.text, R"("primaryGroup": "default")") && !Has(pruned.text, "gold"),
               "edit: a primary held only by the expired parent the write drops is written as default");

    const std::string denied = R"({"primaryGroup":"default","permissions":["x.y"]})";
    const EditPlan deny = Plan({{id, false, denied}}, {HolderKind::User, id}, [&](Model& m, std::string*) {
        m.SetNode(HolderKind::User, id, N("group.default", false));
        return true;
    });
    sink.Check(deny.result == EditResult::Changed && !deny.deleteFile &&
                   LoadsBackEqual({}, id, deny.text, &primary) && !Has(deny.text, "false") &&
                   Has(deny.text, "group.default") && Allows(deny.candidate, id, "x.y"),
               "edit: a deny of group.default, which the default step overrides, is not written");
}

// The canonical text of a holder after the loader read `text` for it: what an edit writes, and so
// what the store holds once an edit has written it.
std::string CanonicalUser(const std::vector<HolderText>& others, const std::string& id, const std::string& text) {
    std::vector<HolderText> texts = others;
    texts.push_back({id, false, text});
    Model m;
    LoadHolders(texts, m);
    const Holder* h = m.FindUser(id);
    return h != nullptr ? SerializeHolder(*h, kNow, true) : std::string();
}

// NoChange is the stored bytes' verdict, and its candidate is what the disk holds.
void StoredBytesCases(CheckSink& sink) {
    Model made;
    made.CreateGroup("staff");
    made.SetNode(HolderKind::Group, "staff", N("staff.use"));
    made.SetNode(HolderKind::User, Id('a'), N("group.staff"));
    const std::string staffText = SerializeHolder(*made.FindGroup("staff"), kNow, true);
    const std::string memberText = SerializeHolder(*made.FindUser(Id('a')), kNow, true);
    const std::vector<HolderText> texts = {{"staff", true, staffText}, {Id('a'), false, memberText}};
    const EditPlan same = Plan(texts, {HolderKind::Group, "staff"}, [](Model& m, std::string*) {
        m.SetNode(HolderKind::Group, "staff", N("staff.use"));
        return true;
    });
    sink.Check(same.result == EditResult::NoChange && Allows(same.candidate, Id('a'), "staff.use"),
               "edit: no change, and the candidate is what the texts grant");

    // A hand-written file the edit does not change in meaning is rewritten in canonical form.
    const std::string id = Id('1');
    const std::string handWritten = R"({"primaryGroup":"default","permissions":["group.default","a.b"]})";
    const EditPlan rewrite = Plan({{id, false, handWritten}}, {HolderKind::User, id}, [&](Model& m, std::string*) {
        m.SetNode(HolderKind::User, id, N("a.b"));
        return true;
    });
    sink.Check(rewrite.result == EditResult::Changed && !rewrite.deleteFile && rewrite.text != handWritten &&
                   rewrite.text == CanonicalUser({}, id, handWritten),
               "edit: a hand-written file the edit leaves the same in meaning is rewritten as a change");

    // A deny of group.default is overridden by the default step: over the canonical file it writes
    // nothing, so it is no change.
    const std::string denied = R"({"primaryGroup":"default","permissions":["x.y"]})";
    const std::string canonical = CanonicalUser({}, id, denied);
    const EditPlan deny = Plan({{id, false, canonical}}, {HolderKind::User, id}, [&](Model& m, std::string*) {
        m.SetNode(HolderKind::User, id, N("group.default", false));
        return true;
    });
    sink.Check(deny.result == EditResult::NoChange && Allows(deny.candidate, id, "x.y"),
               "edit: a deny of group.default over the canonical file is no change");
}

// Who refused is told apart: the closure, the store as it is, the result.
void RefusedByCases(CheckSink& sink) {
    const EditPlan change = Plan({}, {HolderKind::Group, "nowhere"}, [](Model&, std::string* why) {
        *why = "No such group.";
        return false;
    });
    sink.Check(change.result == EditResult::Refused && change.refusedBy == RefusedBy::Change,
               "edit: a closure's refusal is the change's");

    const EditPlan store = Plan({{"a", true, R"({"parents":["ghost"]})"}}, {HolderKind::Group, "a"},
                                [](Model&, std::string*) { return true; });
    sink.Check(store.result == EditResult::Refused && store.refusedBy == RefusedBy::Store,
               "edit: texts that do not load are the store's refusal");

    const std::string id = Id('2');
    const std::string member = R"({"primaryGroup":"staff","permissions":["group.staff"]})";
    const EditPlan candidate = Plan({{"staff", true, "{}"}, {id, false, member}}, {HolderKind::Group, "staff"},
                                    [](Model& m, std::string*) {
                                        m.DeleteGroup("staff");
                                        return true;
                                    });
    sink.Check(candidate.result == EditResult::Refused && candidate.refusedBy == RefusedBy::Candidate &&
                   !candidate.problems.empty(),
               "edit: a delete of a group a holder names is the result's refusal");
}

// The empty group `default` is the default state: it keeps no file.
void DefaultGroupCases(CheckSink& sink) {
    const HolderKey key{HolderKind::Group, "default"};
    const EditPlan noop = Plan({}, key, [](Model& m, std::string*) {
        m.UnsetNode(HolderKind::Group, "default", N("x"));
        return true;
    });
    sink.Check(noop.result == EditResult::NoChange,
               "edit: unsetting what the default group lacks, with no file, is no change");

    const EditPlan set = Plan({}, key, [](Model& m, std::string*) {
        m.SetNode(HolderKind::Group, "default", N("x"));
        return true;
    });
    sink.Check(set.result == EditResult::Changed && !set.deleteFile && Has(set.text, "\"x\""),
               "edit: a node set on the default group writes its file");

    const EditPlan unset = Plan({{"default", true, set.text}}, key, [](Model& m, std::string*) {
        m.UnsetNode(HolderKind::Group, "default", N("x"));
        return true;
    });
    sink.Check(unset.result == EditResult::Changed && unset.deleteFile,
               "edit: the default group left empty has its file deleted");
}

void ProblemLinesCases(CheckSink& sink) {
    const std::vector<std::string> three = {"a", "b", "c"};
    sink.Check(ProblemLines(three) == three, "problem lines: three problems are three lines");

    std::vector<std::string> seven;
    for (int i = 1; i <= 7; ++i) seven.push_back("p" + std::to_string(i));
    const std::vector<std::string> cut = ProblemLines(seven);
    sink.Check(cut.size() == 6 && cut[4] == "p5" && cut[5] == "... and 2 more",
               "problem lines: seven problems are five lines and the count of the rest");

    const std::vector<std::string> ascii = ProblemLines({std::string(250, 'x')});
    sink.Check(ascii.size() == 1 && ascii[0] == std::string(197, 'x') + "..." && ascii[0].size() == 200,
               "problem lines: a long ASCII problem is its first 197 bytes and the dots");

    // A 3-byte character (U+20AC) over bytes 195..197 is dropped whole; one that starts at byte 197
    // is dropped whole as well.
    const std::string euro = "\xE2\x82\xAC";
    const std::vector<std::string> across = ProblemLines({std::string(195, 'x') + euro + std::string(60, 'y')});
    sink.Check(across.size() == 1 && across[0] == std::string(195, 'x') + "...",
               "problem lines: a character across byte 197 is cut before it");

    const std::vector<std::string> atCut = ProblemLines({std::string(197, 'x') + euro + std::string(60, 'y')});
    sink.Check(atCut.size() == 1 && atCut[0] == std::string(197, 'x') + "...",
               "problem lines: a character that starts at byte 197 is dropped whole");

    const std::vector<std::string> exact = ProblemLines({std::string(200, 'z'), std::string(201, 'z')});
    sink.Check(exact.size() == 2 && exact[0] == std::string(200, 'z') && exact[1] == std::string(197, 'z') + "...",
               "problem lines: 200 bytes stay whole and 201 are cut");
}

void ActionJsonCases(CheckSink& sink) {
    Action a;
    a.sourceId = Id('a');
    a.sourceName = "Host";
    a.targetType = "user";
    a.targetId = Id('b');
    a.targetName = "Bob";
    a.description = "/mv user " + Id('b') + " permission set a.b true";
    const std::string want = R"({"timestamp":1700000000,"source":{"id":")" + Id('a') +
                             R"(","name":"Host"},"target":{"type":"user","id":")" + Id('b') +
                             R"(","name":"Bob"},"description":"/mv user )" + Id('b') +
                             R"( permission set a.b true"})";
    sink.Check(ActionJson(a, kNow) == want, "action json: the compact line, its keys in order");

    a.sourceName = std::string("Bo\"b\x01");
    a.targetName = "\xFF";
    const std::string escaped = ActionJson(a, kNow);
    sink.Check(Has(escaped, R"("name":"Bo\"b\u0001")") && Has(escaped, "\"name\":\"\xEF\xBF\xBD\"") &&
                   escaped.find('\n') == std::string::npos,
               "action json: a quote and a control byte are escaped, a byte that is not UTF-8 is replaced");
}

}  // namespace

void RunEditCases(CheckSink& sink) {
    SetNodeCases(sink);
    ExpiredCases(sink);
    UntouchedCases(sink);
    DeleteCases(sink);
    OwnerCases(sink);
    RefusalCases(sink);
    LoaderReadingCases(sink);
    StoredBytesCases(sink);
    RefusedByCases(sink);
    DefaultGroupCases(sink);
    ProblemLinesCases(sink);
    ActionJsonCases(sink);
}

}  // namespace coop::permissions
