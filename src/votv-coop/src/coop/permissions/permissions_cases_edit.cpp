// coop/permissions/permissions_cases_edit.cpp -- the checks of coop/permissions/permission_edit.h:
// an edit planned over the store's texts as read, the file text it yields, the candidate model the
// loader builds from the texts with that one replaced, and the owner invariant.

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
    sink.Check(primary.result == EditResult::Refused && Has(primary.why, "staff"),
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

}  // namespace

void RunEditCases(CheckSink& sink) {
    SetNodeCases(sink);
    ExpiredCases(sink);
    UntouchedCases(sink);
    DeleteCases(sink);
    OwnerCases(sink);
    RefusalCases(sink);
}

}  // namespace coop::permissions
