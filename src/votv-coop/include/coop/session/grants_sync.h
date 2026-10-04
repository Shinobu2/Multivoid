// coop/session/grants_sync.h -- the local dev grants cross the wire: the host computes its own and
// sends each client its own (ReliableKind::PermissionGrants), at the client's proof, on a change of
// the host's permission model and past an expiry in it; the client stores what it is sent
// (coop/session/local_grants). The projected table and its rules are coop/permissions/grants_core.h.
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
