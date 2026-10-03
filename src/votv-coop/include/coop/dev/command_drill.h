// coop/dev/command_drill.h -- drill: a typed command is run by the host and answered privately (ini
// command_drill=on|red / env VOTVCOOP_COMMAND_DRILL; BOTH peers).
//
// Once its world is ready, each peer submits `help` and an unknown command through the chat input's
// own entry (command_sync::Submit) and checks the reply lines in order. The host's lines run locally;
// the client's travel as CommandRequest and come back as CommandReply. Then the client submits six
// unknown commands at once: the host's per-sender rate accepts the first few, drops the rest and tells
// the client once, and the client counts the accepted answers before that notice. `red` expects a wrong
// first reply line, so the drill must fail.
// Lines tagged [CMD-DRILL]; 'host DONE' and 'client DONE' end it, FAIL names the reply that differed, or an answered count outside 1..5.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::command_drill {

// Advance this peer's phases. Every pump tick in a world; a latched read when off. Game thread.
void Tick(coop::net::Session* session);

// The session ended: the phases start over and the reply observer is cleared.
void OnDisconnect();

}  // namespace coop::dev::command_drill
