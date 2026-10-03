// coop/dev/settings_drill.h -- [dev] the host sets one of its own rows through SetValue, resets it, and
// the module shows it followed each time. The step is SET -> PROBE -> RESET -> PROBE, host only,
// started once the client's world is up: the probe is posted to the game thread right after the set,
// the queue is FIFO and the set queued the row's subscribers first, so when the probe runs the
// subscriber has run -- no clock, no tick count. Tagged [SETTINGS-DRILL]; the [dev] settings_drill row.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::settings_drill {

// The [dev] settings_drill row's steps; a latched enum compare when off. Game thread.
void Tick(coop::net::Session* session);

// The local session's last link dropped: the steps start over.
void OnDisconnect();

}  // namespace coop::dev::settings_drill
