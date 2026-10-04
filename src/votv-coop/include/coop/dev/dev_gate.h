// dev_gate -- AUTHORITY for the dev features that write the shared world: only the host (or a solo
// game) writes the world, so points, the clock, events, NPC spawns, the vitals broadcast, the Q menu
// and the free camera's self-teleport ask Allowed(); the weather and teleporting clients check the
// host role themselves. A dev feature that touches only this machine (the free camera, the position
// readout, the overlays, one's own stamina) is a permission instead, granted by the host:
// coop/session/local_grants.

#pragma once

namespace coop::net {
class Session;
}

namespace coop::dev_gate {

// Wire the live session. Harness StartCoopSession, alongside the other
// dev-module SetSession calls. Thread-safe.
void SetSession(coop::net::Session* session);

// False iff a session is running and we are not its host. Thread-safe
// (atomic loads only) -- callable from the render thread, hotkey threads,
// and the game thread alike.
bool Allowed();

}  // namespace coop::dev_gate
