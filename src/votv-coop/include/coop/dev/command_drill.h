// coop/dev/command_drill.h -- drill: a typed command is run by the host and answered privately (ini
// command_drill=on|red|grant|grantred|hostdeny / env VOTVCOOP_COMMAND_DRILL; BOTH peers for on /
// red; grant and grantred a client, hostdeny the host).
//
// Once its world is ready, each peer submits `help` and an unknown command through the chat input's
// own entry (command_sync::Submit) and checks the reply lines in order. The host's lines run locally;
// the client's travel as CommandRequest and come back as CommandReply. Then each peer submits a line
// one byte past the request's limit and expects the local refusal (no command token spent); the host
// also submits a line of exactly the limit and expects the unknown-command reply, uncut. Then the
// client submits six unknown commands at once: the host's rate accepts the first few and tells the
// client once; the client counts the accepted answers. `red` expects a wrong first reply line, so it
// must fail. grant / grantred (a client) send `unban` / `banid`, one the host's permission files
// grant and one they do not, the first reply the verdict; hostdeny (the host) an `unban` its own file
// denies it; those skip the phases above. [CMD-DRILL]: 'host DONE', 'client DONE', 'grant PASS' and
// 'hostdeny PASS' end it; FAIL names the differing reply, or an answered count outside 1..5.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::command_drill {

// Advance this peer's phases. Every pump tick in a world; a latched read when off. Game thread.
void Tick(coop::net::Session* session);

// The session ended: the phases start over and the reply observer is cleared.
void OnDisconnect();

// A client's world is up and its join is over: connected, the world-ready announced, the join
// progress idle. The one predicate the command drill and the /mv drill start a client on. Game
// thread.
bool ClientReady(coop::net::Session* s);

}  // namespace coop::dev::command_drill
