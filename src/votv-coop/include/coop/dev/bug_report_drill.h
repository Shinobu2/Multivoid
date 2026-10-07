// coop/dev/bug_report_drill.h -- drill: a bug report is saved and its zip judged inside the game
// (ini bug_report_drill=on|red / env VOTVCOOP_BUG_REPORT_DRILL; BOTH peers).
//
// Each peer plants one value of every class the redactor knows (a marked address, a player id, an
// identity key, a path under this machine's profile), requests a bundle, waits for it, opens the zip
// and reads every entry: none of the planted values or the peer's own player id may appear in any
// of them, and the log must hold the tokens that replaced them. The host waits for a joined
// client first, and starts over each time one joins again. `red` puts the address in the form's
// own text, which is never marked, so the zip holds it and the drill must fail.
// [REPORT-DRILL]: 'PASS (<n> entries, <n> bytes)', 'FAIL: <why>', 'ABORT: <why>' (the bundle
// did not finish: not a measurement).

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::bug_report_drill {

// Advance this peer's phases. Every pump tick in a world; a latched read when off. Game thread.
void Tick(coop::net::Session* session);

}  // namespace coop::dev::bug_report_drill
