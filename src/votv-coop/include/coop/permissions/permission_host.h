// coop/permissions/permission_host.h -- the hosted server's LIVE permission model: loaded from the
// store at each host start, replaced by an in-game edit or reload, answered on the game thread. The
// in-game glue of coop/permissions (the other files are engine-free); it logs.
//
// Threads: OnHostStart runs on the TimelineThread, in StartCoopSession's host block before the
// session's Start spawns the net thread, and PUBLISHES the model it built (with the server folder)
// into a hand-off slot (a mutex-guarded pointer, then an atomic flag set with release). Everything
// else runs on the game thread only, and adopts a published model first (an acquire exchange of the
// flag, then the swap under the mutex); after the adoption nothing takes a lock. The order is
// happens-before: a client's line reaches a check only after the net thread exists, so the publish
// precedes every check it governs. The model is never cleared at a session's end; each host start
// replaces it. Nothing here is called outside a running hosted session (the console passes every
// node there).
#pragma once

#include "coop/permissions/permission_edit.h"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace coop::permissions::host {

// Builds a fresh Model from `<serverDir>\permissions` (an empty `serverDir` gives the empty store,
// logged), logs the load and each problem, and publishes the model with its subject
// (`mode=listen`, and `server=<serverId>` except for the id `global`, which ContextSet drops as
// LuckPerms does, so that subject has no server) and the folder. The store loads whole or not at all
// (ShouldLoad): with any problem the published model is empty and BROKEN, and every check but the
// console's is refused until the store is fixed (`Reload` after fixing it). A server with no store
// is not broken: the defaults apply. `serverId` is the hosted server's id. TimelineThread.
void OnHostStart(const std::wstring& serverDir, std::string serverId);

// The decision for `playerId` (32 hex) on `node`: the resolved chain's True / False, else
// `defaultGranted`. `owner` fills what the chain leaves undefined with True (the server's console).
// With a broken store it is `owner` alone: default-granted nodes are refused too. GAME THREAD.
bool Allows(std::string_view playerId, std::string_view node, bool defaultGranted, bool owner);

// True when `node` is set on the player or a group it inherits, with value true; a wildcard that
// implies it does not count (IsSetExplicitly). Always false with a broken store. GAME THREAD.
bool HoldsExplicitly(std::string_view playerId, std::string_view node);

// The clock the checks use: unix seconds. Any thread.
int64_t NowSeconds();

// A counter of the live model's replacements: it advances when the game thread adopts a model a host
// start published (adopting a pending one first, as the first check does) and at each in-game edit
// or reload that publishes one, so a changed value means every answer must be asked again. GAME
// THREAD.
uint64_t Revision();

// The earliest second at which an answer for `playerId` changes by itself (the resolved chain's
// validUntil), 0 for none and for a broken store. It resolves the player, which allocates: ask it only
// while computing the answers, never per tick. GAME THREAD.
int64_t NextExpiry(const std::string& playerId);

// What an edit did: `Changed` (written, published, logged), `NoChange` (what the disk holds is
// published, nothing written), `Refused`, or `NoSession` (no running hosted session: no replies,
// the caller answers). `replies` is one entry per reply LINE, the host's own composition.
enum class ApplyOutcome : uint8_t { Changed, NoChange, Refused, NoSession };
struct ApplyResult {
    ApplyOutcome outcome = ApplyOutcome::Refused;
    std::vector<std::string> replies;
};

// One edit of one holder, in this order: no running hosted session (NoSession); adopt a pending
// publish; no server folder; read the store's texts from DISK (a folder or a file it cannot read, a
// stem that is no group name or player id, each refuse the edit before anything else); PlanEdit
// with the host's own id as the owner and the live subject; then NoChange publishes the disk's
// reading, a refusal answers its reason, a change is written (one file, whole), published (the
// loader's reading of the files), logged and appended to `<folder>\permissions\actions.jsonl` as
// `action`. An edit saved by hand to the SAME holder's file between the read and the move is lost;
// a hand edit to any other file is part of what the next edit reads. GAME THREAD.
ApplyResult Apply(const HolderKey& key, const std::function<bool(Model& copy, std::string* why)>& change,
                  bool callerIsOwner, const std::vector<std::string>& nodes, const Action& action);

// Reads the store from disk and, when it loads whole, makes it the live model (and clears the
// broken flag): the lines say "Reloaded: <g> groups, <u> users." or the problems, the live model
// kept. Empty when there is no running hosted session: the caller answers that. `actorId` is
// logged. GAME THREAD.
std::vector<std::string> Reload(const std::string& actorId);

// The live model, for `info` and `listgroups`. Valid until the next Apply, Reload or adoption: read
// it within one handler call and keep no reference or `Holder*` of it. With a broken store it is the
// empty one. GAME THREAD.
const Model& Live();

// The live model is the empty one of a store that did not load at the host start (and no reload has
// loaded it since). GAME THREAD.
bool StoreBroken();

}  // namespace coop::permissions::host
