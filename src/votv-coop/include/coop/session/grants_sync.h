// coop/session/grants_sync.h -- the local dev grants cross the wire: the host computes its own and
// sends each client its own (ReliableKind::PermissionGrants), at the client's proof, on a change of
// the host's permission model and past an expiry in it; the client stores what it is sent
// (coop/session/local_grants). The projected table and its rules are coop/permissions/grants_core.h.
// Precedents for sending a client its own answer: Source replicates sv_cheats, the server owns the
// value and the client reads its local copy for its local debug features (FCVAR_CHEAT and
// FCVAR_REPLICATED, reference/source-sdk-2013/src/public/tier1/iconvar.h:51,57-62; WireFrameMode,
// reference/source-sdk-2013/src/game/client/view.h:65-90); MTA pushes a per-client switch
// (TOGGLE_DEBUGGER, reference/mtasa-blue/Client/mods/deathmatch/logic/rpc/COutputRPCs.cpp:21-27).
// Shapes only.
#pragma once

#include "coop/net/session.h"

namespace coop::session::grants_sync {

// Game thread, every pump tick: on the host, computes the host's own bits and each proved client's
// when the answers are due, and sends a client its bits when it holds none or holds others. A
// client does nothing.
void HostTick(coop::net::Session& session);

// Game thread, the state family's receiver: a client takes its own bits from the host.
void HandleGrants(coop::net::Session& session, const coop::net::Session::ReliableMessage& msg);

}  // namespace coop::session::grants_sync
