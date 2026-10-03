// coop/dev/settings_drill.h -- [dev] the host sets its own rows through SetValue (one row per step; the
// flags token sets two), resets them, and the module that follows each row shows it followed both times.
// The step is SET -> PROBE -> RESET -> PROBE, host only, started once the client's world is up: the
// probe is posted to the game thread right after the set, the queue is FIFO and the set queued the
// rows' subscribers first, so when the probe runs the subscribers have run -- no clock, no tick count.
// The scale and font tokens follow their rows on the render thread instead, and the voicemode token
// follows its row at the voice tick: each reads its module's counter before the set and the reset,
// and waits each Tick until the counter moved.
// Tagged [SETTINGS-DRILL]; the [dev] settings_drill row.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::settings_drill {

// The [dev] settings_drill row's steps; a latched enum compare when off. Game thread.
void Tick(coop::net::Session* session);

// The local session's last link dropped: the steps start over.
void OnDisconnect();

}  // namespace coop::dev::settings_drill
