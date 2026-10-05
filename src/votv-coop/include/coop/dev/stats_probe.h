// coop/dev/stats_probe.h -- probe: every player stat read, written and restored in one frame (ini
// stats_probe=on|red / env VOTVCOOP_STATS_PROBE; BOTH peers, each its own local player).
//
// Once per session, in one Tick call, each peer walks the 22 rows of ue_wrap::vitals in Field
// order: a writable row is read, written a test value, read back, restored through Write and read
// again; a read-only row is read. Then the profile snapshot's nine members are checked by name
// against their rows, the refusals (a read-only write, a non-finite write, a wire id past the
// last row) and the class defaults are checked. Nothing written outlives the call, so the pose and
// profile streams never carry a test value. `red` expects a wrong read-back on Health, so it must
// fail. A client starts once ClientReady holds (its join's profile is applied); the host once one
// row of each owner reads.
// [STATS-PROBE]: one line per row, the snapshot, the refusals and the defaults, then
// 'DONE bad=<m> ok=<n>' (a green run prints bad=0 ok=25).

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::stats_probe {

// Run the probe once the peer is ready. Every pump tick in a world; a latched read when off. Game
// thread.
void Tick(coop::net::Session* session);

// The session ended: the probe runs again in the next one.
void OnDisconnect();

}  // namespace coop::dev::stats_probe
