// coop/dev/mv_drill.h -- drill: the /mv commands end to end, on BOTH peers (ini mv_drill=on|red /
// env VOTVCOOP_MV_DRILL), run against the rig's seeded permission store.
//
// Each peer runs an ordered script through the chat input's own entry (command_sync::Submit). The
// reply observer keeps two lists in arrival order: notices (lines starting `[mv] `) and replies
// (every other line). A Send judges the first reply past the size the list had when it was
// submitted; a WaitNotice searches every notice seen so far. The host starts once a client is
// seated, proved and world-ready, edits the store (breaks it on purpose, reloads, sets, creates and
// deletes a group) and reads the files it wrote; the client waits for the host's grant and notice,
// then sends its own lines, one of which is a delegate edit the host's feed is told of. No step
// waits on a clock: a budget only fails a hung step. `red` expects a wrong reply at the host's
// step 5.1, so it must fail. The drill refuses to start when command_drill or settings_drill is on
// (the reply observer is one slot).
// [MV-DRILL]: '<role> PASS' or '<role> FAIL: step <item>.<k>: ...'; <role> is `host` or `c<slot>`.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::mv_drill {

// Advance this peer's script. Every pump tick in a world; a latched read when off. Game thread.
void Tick(coop::net::Session* session);

// The session ended: the script starts over and the reply observer is cleared.
void OnDisconnect();

}  // namespace coop::dev::mv_drill
