// coop/permissions/permission_edit.cpp -- see coop/permissions/permission_edit.h.

#include "coop/permissions/permission_edit.h"

#include "coop/permissions/resolution.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <string>
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

// The file a holder is written as: false for none (the holder is absent, or a user in the default
// state once what has expired is pruned), else true with the pruned text.
bool FileText(const Holder* h, int64_t now, std::string* text) {
    if (h == nullptr) return false;
    if (h->kind == HolderKind::User && IsDefaultUser(*h, now, true)) return false;
    *text = SerializeHolder(*h, now, true);
    return true;
}

// The entry of `key` in `texts` (a group or a user, its lower-cased stem), or null.
const HolderText* EntryOf(const std::vector<HolderText>& texts, const HolderKey& key) {
    const bool group = key.kind == HolderKind::Group;
    const auto it = std::find_if(texts.begin(), texts.end(), [&](const HolderText& t) {
        return t.group == group && Lowered(t.stem) == key.name;
    });
    return it == texts.end() ? nullptr : &*it;
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

EditPlan Refuse(RefusedBy by, std::string why, std::vector<std::string> problems = {}) {
    EditPlan plan;
    plan.result = EditResult::Refused;
    plan.refusedBy = by;
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
    if (!callerIsOwner && ownerId.empty()) return Refuse(RefusedBy::Change, "The host's identity is not loaded.");
    const HolderKey key{keyIn.kind, Lowered(keyIn.name)};

    Model before;
    LoadReport beforeReport = LoadHolders(texts, before);
    if (!ShouldLoad(beforeReport)) {
        // The message is built before the call: Refuse takes the vector by value.
        std::string why = "The permission files do not load: " + beforeReport.problems.front();
        return Refuse(RefusedBy::Store, std::move(why), std::move(beforeReport.problems));
    }

    Model copy = before;
    std::string why;
    if (!change(copy, &why)) return Refuse(RefusedBy::Change, std::move(why));

    // The text written is what the loader reads back. The first text is the copy's holder, pruned of
    // what has expired; the candidate built from it may differ from the copy (a pruned parent leaves
    // its stored primary without a holder, the default step overrides a deny of group.default). So the
    // candidate's own holder is serialised again and, when that differs, it is the text and the
    // candidate is built once more: a loaded holder re-serialises byte-equal, the fixed point.
    std::string newText;
    bool deleteFile = !FileText(FindHolder(copy, key), now, &newText);

    EditPlan plan;
    const auto build = [&]() {
        plan.candidate = Model();
        LoadReport report = LoadHolders(WithEntry(texts, key, deleteFile ? nullptr : &newText), plan.candidate);
        if (ShouldLoad(report)) return true;
        // The next host start would refuse the same files.
        plan.result = EditResult::Refused;
        plan.refusedBy = RefusedBy::Candidate;
        plan.why = report.problems.front();
        plan.problems = std::move(report.problems);
        return false;
    };
    if (!build()) return plan;
    std::string reread;
    const bool rereadDelete = !FileText(FindHolder(plan.candidate, key), now, &reread);
    if (rereadDelete != deleteFile || reread != newText) {
        deleteFile = rereadDelete;
        newText = std::move(reread);
        if (!build()) return plan;
    }
    // NoChange is judged on the bytes the store holds: the final text against the holder's entry, or
    // both absent. A hand-written file the command leaves the same in meaning is rewritten in
    // canonical form (a change); a deny the default step overrides writes nothing (no change).
    const HolderText* stored = EntryOf(texts, key);
    if (deleteFile ? stored == nullptr : (stored != nullptr && stored->text == newText)) {
        EditPlan same;
        same.result = EditResult::NoChange;
        same.candidate = std::move(before);  // the disk's reading, which the host publishes
        return same;
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

std::vector<std::string> ProblemLines(const std::vector<std::string>& problems) {
    constexpr size_t kMaxLines = 5;
    constexpr size_t kMaxBytes = 200;
    constexpr size_t kDots = 3;  // the `...` a cut line ends with, counted in kMaxBytes
    std::vector<std::string> lines;
    const size_t shown = std::min(problems.size(), kMaxLines);
    for (size_t i = 0; i < shown; ++i) {
        const std::string& p = problems[i];
        if (p.size() <= kMaxBytes) {
            lines.push_back(p);
            continue;
        }
        // The kept prefix ends where the first dropped byte starts a character.
        size_t keep = kMaxBytes - kDots;
        while (keep > 0 && (static_cast<unsigned char>(p[keep]) & 0xC0) == 0x80) --keep;
        lines.push_back(p.substr(0, keep) + "...");
    }
    if (problems.size() > kMaxLines)
        lines.push_back("... and " + std::to_string(problems.size() - kMaxLines) + " more");
    return lines;
}

std::string ActionJson(const Action& a, int64_t timestamp) {
    using OJson = nlohmann::ordered_json;
    OJson line = OJson::object();
    line["timestamp"] = timestamp;
    line["source"] = OJson::object();
    line["source"]["id"] = a.sourceId;
    line["source"]["name"] = a.sourceName;
    line["target"] = OJson::object();
    line["target"]["type"] = a.targetType;
    line["target"]["id"] = a.targetId;
    line["target"]["name"] = a.targetName;
    line["description"] = a.description;
    return line.dump(-1, ' ', false, OJson::error_handler_t::replace);
}

}  // namespace coop::permissions
