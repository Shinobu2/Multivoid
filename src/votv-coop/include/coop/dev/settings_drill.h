// coop/dev/settings_drill.h -- [dev] the host sets its own rows through SetValue (one row per step; the
// flags token sets two), resets them, and the module that follows each row shows it followed both times.
// The step is SET -> PROBE -> RESET -> PROBE (servercmd and servercmdspellred: SET -> PROBE -> RESPELL ->
// RESET -> PROBE), host only, started once the client's world is up: the probe is posted to the game
// thread right after the set, the queue is FIFO and the set queued the rows' subscribers first, so when
// the probe runs the subscribers have run -- no clock, no tick count. The scale and font tokens follow
// their rows on the render thread, the voicemode token at the voice tick: each reads its module's
// counter before the set and the reset and waits until it moved. The server tokens set voice.distance_cm,
// a server-scope row: the host's reads answer it at once and the client's log shows it arrive. serverjoin
// sets BEFORE any client connects, so the joiner's snapshot carries the value, and resets once the
// joiner's world is up. The command tokens (servercmd, servercmdred, servercmdspellred) run the step
// through /set and /reset lines, the probe posted when the reply is read; another reply ends it with a
// FAIL line. servercmd and servercmdspellred respell the set value (servercmdred's set is refused, so it
// never respells): servercmd as the same value (6000.0), servercmdspellred as a real change (6001).
// Each completed step prints DONE with a cycle number, per process. Tagged [SETTINGS-DRILL]; [dev] settings_drill.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::settings_drill {

// The [dev] settings_drill row's steps; a latched enum compare when off. Game thread.
void Tick(coop::net::Session* session);

// The local session's last link dropped: the steps start over.
void OnDisconnect();

}  // namespace coop::dev::settings_drill
