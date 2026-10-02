// coop/dev/mannequin_drill.h -- [dev] spawn a native walking mannequin; the baseline drill.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::mannequin_drill {

// Spawn one native walking mannequin the way the game's own spawner does: every spawn point is
// shown for a moment (prepareSpawn), and half a second later the nearest point to the local
// player that is not on screen spawns the walker (wMannequinSpawn_C.spawn). The walker is the
// game's real one: it opens doors, is saved with the world and returns angry after a load.
// Refused on a client in a session (dev_gate), while an earlier request is still running, and
// when five walkers are alive (the game itself culls above four). Any thread.
void SpawnWalker();

// The frame tail's entry (harness::pump::TickFrameTail): a single atomic read when no spawn is
// pending. Game thread.
void TickFrame();

// The [dev] mannequin_drill row's phases; a single enum read when it is off. Game thread.
void Tick(coop::net::Session* session);

// The local session's last link dropped: the client's samples start over. The host's half runs
// once per process and is not re-armed.
void OnDisconnect();

}  // namespace coop::dev::mannequin_drill
