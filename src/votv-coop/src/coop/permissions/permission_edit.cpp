// coop/permissions/permission_edit.cpp -- see coop/permissions/permission_edit.h.

#include "coop/permissions/permission_edit.h"

#include "coop/permissions/resolution.h"

#include <algorithm>
#include <utility>

namespace coop::permissions {
namespace {

std::string Lowered(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    }
    return s;
}

const Holder* FindHolder(const Model& m, const HolderKey& key) {
    return key.kind == HolderKind::Group ? m.FindGroup(key.name) : m.FindUser(key.name);
}

// What the holder's file is, as the edit sees it: false for no file (the holder is absent, or a user
// in the default state), else true with the text the holder serialises to without pruning. An
// expired node is part of it: removing one is a change, and the file may go with it.
bool Outcome(const Holder* h, int64_t now, std::string* text) {
    if (h == nullptr) return false;
    if (h->kind == HolderKind::User && IsDefaultUser(*h, now, false)) return false;
    *text = SerializeHolder(*h, now, false);
    return true;
}

// `texts` with the entry of `key` replaced by `text`, removed (null `text`), or, when the store has
// none, inserted where ReadStoreTexts would list it: groups before users, each sorted by stem.
std::vector<HolderText> WithEntry(const std::vector<HolderText>& texts, const HolderKey& key,
                                  const std::string* text) {
    const bool group = key.kind == HolderKind::Group;
    std::vector<HolderText> out = texts;
    const auto same = std::find_if(out.begin(), out.end(), [&](const HolderText& t) {
        return t.group == group && Lowered(t.stem) == key.name;
    });
    if (same != out.end()) {
        if (text == nullptr) {
            out.erase(same);
        } else {
            same->text = *text;
        }
        return out;
    }
    if (text == nullptr) return out;
    const int rank = group ? 0 : 1;
    const auto after = std::find_if(out.begin(), out.end(), [&](const HolderText& t) {
        const int r = t.group ? 0 : 1;
        return r > rank || (r == rank && Lowered(t.stem) > key.name);
    });
    out.insert(after, HolderText{key.name, group, *text});
    return out;
}

EditPlan Refuse(std::string why, std::vector<std::string> problems = {}) {
    EditPlan plan;
    plan.result = EditResult::Refused;
    plan.why = std::move(why);
    plan.problems = std::move(problems);
    return plan;
}

}  // namespace

bool OwnerLoses(const Model& before, const Model& after, std::string_view ownerId, const ContextSet& subject,
                int64_t now, const std::vector<std::string>& nodes, std::string* which) {
    // One Resolve per model serves every node: Evaluate only reads the resolved answers.
    const auto was = Resolve(before, ownerId, subject, now);
    const auto will = Resolve(after, ownerId, subject, now);
    for (const std::string& node : nodes) {
        if (Evaluate(*was, node, true).value == Tristate::True &&
            Evaluate(*will, node, true).value == Tristate::False) {
            if (which != nullptr) *which = node;
            return true;
        }
    }
    return false;
}

EditPlan PlanEdit(const std::vector<HolderText>& texts, const HolderKey& keyIn,
                  const std::function<bool(Model& copy, std::string* why)>& change, bool callerIsOwner,
                  std::string_view ownerId, const ContextSet& subject, int64_t now,
                  const std::vector<std::string>& nodes) {
    if (!callerIsOwner && ownerId.empty()) return Refuse("The host's identity is not loaded.");
    const HolderKey key{keyIn.kind, Lowered(keyIn.name)};

    Model before;
    LoadReport beforeReport = LoadHolders(texts, before);
    if (!ShouldLoad(beforeReport)) {
        // The message is built before the call: Refuse takes the vector by value.
        std::string why = "The permission files do not load: " + beforeReport.problems.front();
        return Refuse(std::move(why), std::move(beforeReport.problems));
    }

    Model copy = before;
    std::string why;
    if (!change(copy, &why)) return Refuse(std::move(why));

    std::string beforeText, copyText;
    const bool beforeHasFile = Outcome(FindHolder(before, key), now, &beforeText);
    const bool copyHasFile = Outcome(FindHolder(copy, key), now, &copyText);
    if (beforeHasFile == copyHasFile && beforeText == copyText) {
        EditPlan plan;
        plan.result = EditResult::NoChange;
        return plan;
    }

    // The text written is the copy's holder, pruned of what has expired: the file is rewritten
    // whole, as LuckPerms rewrites a holder from its model. The candidate is what the loader makes
    // of it, so its default step may differ from the copy.
    const Holder* copyHolder = FindHolder(copy, key);
    const bool deleteFile =
        copyHolder == nullptr || (copyHolder->kind == HolderKind::User && IsDefaultUser(*copyHolder, now, true));
    std::string newText;
    if (!deleteFile) newText = SerializeHolder(*copyHolder, now, true);

    EditPlan plan;
    LoadReport candidateReport = LoadHolders(WithEntry(texts, key, deleteFile ? nullptr : &newText), plan.candidate);
    if (!ShouldLoad(candidateReport)) {
        // The next host start would refuse the same files.
        plan.result = EditResult::Refused;
        plan.why = candidateReport.problems.front();
        plan.problems = std::move(candidateReport.problems);
        return plan;
    }
    if (!callerIsOwner && OwnerLoses(before, plan.candidate, ownerId, subject, now, nodes, &plan.why)) {
        plan.result = EditResult::OwnerLoses;
        return plan;
    }
    plan.result = EditResult::Changed;
    plan.deleteFile = deleteFile;
    plan.text = std::move(newText);
    return plan;
}

}  // namespace coop::permissions
