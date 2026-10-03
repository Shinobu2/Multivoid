// coop/permissions/permission_host.h -- the hosted server's LIVE permission model: loaded from the
// store at each host start, answered on the game thread. The in-game glue of coop/permissions
// (the other files are engine-free); it logs.
//
// Threads: OnHostStart runs on the TimelineThread, in StartCoopSession's host block before the
// session's Start spawns the net thread, and PUBLISHES the model it built into a hand-off slot (a
// mutex-guarded pointer, then an atomic flag set with release). Allows / HoldsExplicitly run on the
// game thread only, and adopt a published model first (an acquire exchange of the flag, then the
// swap under the mutex); a check after the adoption takes no lock. The order is happens-before: a
// client's line reaches a check only after the net thread exists, so the publish precedes every
// check it governs. The model is never cleared at a session's end; each host start replaces it.
// Nothing here is called outside a running hosted session (the console passes every node there).
#pragma once

#include <string>
#include <string_view>

namespace coop::permissions::host {

// Builds a fresh Model from `<serverDir>\permissions` (an empty `serverDir` gives the empty store,
// logged), logs the load and each problem, and publishes the model with its subject
// (`mode=listen`, and `server=<serverId>` except for the id `global`, which ContextSet drops as
// LuckPerms does, so that subject has no server). The store loads whole or not at all (ShouldLoad): with any
// problem the published model is empty and BROKEN, and every check but the console's is refused
// until the store is fixed. A server with no store is not broken: the defaults apply.
// `serverId` is the hosted server's id. TimelineThread.
void OnHostStart(const std::wstring& serverDir, std::string serverId);

// The decision for `playerId` (32 hex) on `node`: the resolved chain's True / False, else
// `defaultGranted`. `owner` fills what the chain leaves undefined with True (the server's console).
// With a broken store it is `owner` alone: default-granted nodes are refused too. GAME THREAD.
bool Allows(std::string_view playerId, std::string_view node, bool defaultGranted, bool owner);

// True when `node` is set on the player or a group it inherits, with value true; a wildcard that
// implies it does not count (IsSetExplicitly). Always false with a broken store. GAME THREAD.
bool HoldsExplicitly(std::string_view playerId, std::string_view node);

}  // namespace coop::permissions::host
