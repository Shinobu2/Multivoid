// coop/dev/stat_order_drill.h -- drill: the host orders and queries a real client's stats and
// effects and checks every answer (ini stat_order_drill=on|red / env VOTVCOOP_STAT_ORDER_DRILL; the
// HOST acts, a client only answers).
//
// A phase machine of twelve steps, each started by the answer to the one before, never by a clock:
// the host gives itself an effect before any client has joined (the measurement of whether a joiner
// inherits the host's effects), queries the joiner, sets its food and checks the host's stored
// profile took it, is refused a write to a read-only row, adds, queries and removes an effect,
// restores the two stats it set, removes its own effect and checks nothing is left unanswered.
// The first step that is not OK ends the drill with its line: the later steps rest on its answer.
// `red` expects a wrong answer at step 3, so it fails. A run that ends early may leave the test
// players changed; each proof run is a fresh world.
// [STAT-ORDER]: 'step <n> OK|MISMATCH|UNMEASURABLE', 'joiner effects: <names>', then 'DONE bad=<m>
// ok=<n>' (a green run prints bad=0 ok=12).

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::stat_order_drill {

// Advance the drill once the host is ready. Every pump tick in a world; a latched read when off.
// Game thread.
void Tick(coop::net::Session* session);

// The session ended: the drill starts over in the next one, and an answer still in flight is
// ignored.
void OnDisconnect();

}  // namespace coop::dev::stat_order_drill
