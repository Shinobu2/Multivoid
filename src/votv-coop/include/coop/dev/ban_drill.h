// coop/dev/ban_drill.h -- the host ban drill (ini ban_drill=1 / env VOTVCOOP_BAN_DRILL; HOST only).
// Once the first client has finished joining and every reliable byte sent to it is
// acknowledged, the host bans that client's slot ONCE through the moderation verb and reports
// whether the ban was stored under the player's proved id and the seat closed. Tagged [BAN-DRILL].
//
// The rejoin refusal (a banned id refused at the next join) is not here: it needs a peer that
// rejoins, which the rig's scenario foundation owns. ban_list's selftest proves the codec, the
// address rule and the index; the check itself is proved by the hands-on and, later, authdrill's
// rejoin arm.
//
// The red arm (ban_drill_stale_token=1) aims the ban with a generation one past the slot's, so the
// token check refuses it and the drill reports FAIL. The drill writes the host's bans.json: run it
// under lane.py prove, which restores the server folders, never in a plain smoke.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::ban_drill {

// Game thread, once per pump tick; two latched bool reads when off.
void Tick(coop::net::Session* s);

// The session ended. The wait for ARMED holds no per-session state, and a drill that has banned
// (POSTED, waiting for the seat's free) or reported (DONE) is left alone, and it never re-arms in
// the process.
void OnSessionEnd();

}  // namespace coop::dev::ban_drill
