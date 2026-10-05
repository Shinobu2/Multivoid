// coop/permissions/permission_edit.h -- the plan of one edit of the permission store. An edit is a
// reload with one file replaced: the store's texts as read are loaded (`before`), the change is
// applied to a copy of that model, the one holder it touched is serialised, and the texts with that
// one replaced are loaded again (`candidate`). What the host publishes is what the next host start
// loads, for every holder, because the candidate is the loader's own reading of the files. Engine-
// free: no ue_wrap include, no logging, no file access (reading and writing the disk are the
// host's); time and the host's identity are parameters.
//
// LuckPerms rewrites the whole holder from its model after a command (StorageAssistant.java:77-100,
// AbstractConfigurateStorage.java:221-244) and deletes the file of a user in the default state
// (AbstractUserManager.java:130-147); MTA rewrites acl.xml whole (CAccessControlListManager.cpp:
// 237-278). Neither plans against a loader that can refuse the files: ours refuses an edit whose
// result the next start would refuse.
#pragma once

#include "coop/permissions/context_set.h"
#include "coop/permissions/model.h"
#include "coop/permissions/permission_files.h"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace coop::permissions {

// A user id (32 hex) or a group name, lower-cased.
struct HolderKey {
    HolderKind kind;
    std::string name;
};

enum class EditResult : uint8_t { Changed, NoChange, Refused, OwnerLoses };

// Who refused: the change itself (a rule of the leaf, a missing group), the store as it is (`before`
// did not load), or the result (the candidate did not load: the next host start would refuse it).
enum class RefusedBy : uint8_t { Change, Store, Candidate };

struct EditPlan {
    EditResult result = EditResult::Refused;
    RefusedBy refusedBy = RefusedBy::Change;
    // Refused: the change's text, or the loader's first problem; OwnerLoses: the node.
    std::string why;
    // Refused by the loader (Store, Candidate): every problem (the host answers the first five).
    std::vector<std::string> problems;
    // Changed: the next live model. NoChange: `before`, the disk's reading, which the host publishes.
    Model candidate;
    // Changed: the holder's file goes.
    bool deleteFile = false;
    // Changed and !deleteFile: the holder's new file text.
    std::string text;
};

// Plans `change` applied to the holder `key` of the store `texts` (as ReadStoreTexts returned them).
// `change` edits the copy through the Model API. It returns false ONLY to refuse (a missing group,
// an invalid key, a rule of the leaf), with `*why`; a Model setter's own `false` that means
// "already so" is NOT a refusal and the closure returns true. When `callerIsOwner` is false, every
// node of `nodes` is judged for `ownerId`, and an empty `ownerId` refuses ("The host's identity is
// not loaded."). Steps: the texts must load; `change`; the new text (pruned) or a delete; the texts
// with that one replaced, added or removed must load; the text is then the loader's own reading of
// the holder (the candidate's holder serialised again, pruned: when it differs it replaces the
// text and the candidate is built once more); NoChange when that final text equals the holder's
// entry in `texts` byte for byte, or both are absent (a hand-written file the change left the same
// in meaning is still rewritten in canonical form; a deny the default step overrides is no
// change; the empty group `default` and a user in the default state keep no file, so an edit that
// leaves one so is no change over no file and a delete over one); the owner invariant. PURE:
// nothing is read or written.
EditPlan PlanEdit(const std::vector<HolderText>& texts, const HolderKey& key,
                  const std::function<bool(Model& copy, std::string* why)>& change, bool callerIsOwner,
                  std::string_view ownerId, const ContextSet& subject, int64_t now,
                  const std::vector<std::string>& nodes);

// True (and `*which` the first such node) when a node of `nodes` is true for `ownerId` in `before`
// and false in `after`, each judged as Evaluate(*Resolve(m, ownerId, subject, now), node,
// /*owner=*/true): the owner passes what the chain leaves undefined, so only an explicit deny
// takes a node away from the owner. PURE.
bool OwnerLoses(const Model& before, const Model& after, std::string_view ownerId, const ContextSet& subject,
                int64_t now, const std::vector<std::string>& nodes, std::string* which);

// One permission change as the host logs it and appends it to the action log: who (the proved id
// and the nick), what it acted on (`user` or `group`, its id or name, its display name) and the
// description (`/mv ` and the canonical line). LuckPerms' LoggedAction fields
// (LoggedAction.java:69-72).
struct Action {
    std::string sourceId, sourceName, targetType, targetId, targetName, description;
};

// The lines a host answers for a list of loader problems: at most five, each at most 200 bytes
// INCLUDING the `...` it ends with when it was cut (cut on a UTF-8 boundary at or before byte 197),
// then `... and N more` when there are more than five. PURE.
std::vector<std::string> ProblemLines(const std::vector<std::string>& problems);

// The action log's line without its `\n`: the compact `ordered_json` `{"timestamp", "source": {"id",
// "name"}, "target": {"type", "id", "name"}, "description"}` in that key order, `timestamp` in epoch
// seconds, a byte that is not UTF-8 replaced (ActionJsonSerializer's keys). PURE.
std::string ActionJson(const Action& a, int64_t timestamp);

}  // namespace coop::permissions
