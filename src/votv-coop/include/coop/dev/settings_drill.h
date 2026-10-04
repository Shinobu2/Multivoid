// coop/dev/settings_drill.h -- [dev] the host sets its own rows through SetValue (one row per step; the
// flags token sets two), resets them, and the module that follows each row shows it followed both times.
// The step is SET -> PROBE -> RESET -> PROBE, host only, started once the client's world is up: the
// probe is posted to the game thread right after the set, the queue is FIFO and the set queued the
// rows' subscribers first, so when the probe runs the subscribers have run -- no clock, no tick count.
// The scale and font tokens follow their rows on the render thread instead, and the voicemode token
// follows its row at the voice tick: each reads its module's counter before the set and the reset,
// and waits each Tick until the counter moved.
// The server tokens set voice.distance_cm, a server-scope row: the host's own reads answer it at
// once and the client's log shows it arrive. serverjoin is the one step that sets BEFORE any
// client connects, so the joiner's snapshot carries the value, and resets once the joiner's world
// is up. servercmd and servercmdred run the step through /set and /reset submitted as lines, the
// probe posted when the reply is read; another reply ends the step with a FAIL line. Each completed
// step prints its DONE line with a cycle number, counted per process.
// Tagged [SETTINGS-DRILL]; the [dev] settings_drill row.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::settings_drill {

// The [dev] settings_drill row's steps; a latched enum compare when off. Game thread.
void Tick(coop::net::Session* session);

// The local session's last link dropped: the steps start over.
void OnDisconnect();

}  // namespace coop::dev::settings_drill
