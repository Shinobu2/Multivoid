#pragma once

// coop/world/spawn_authority.h -- the ONE owner of client-side shared-world spawner suppression.

// INVARIANT: a connected CLIENT ticks NO shared-world spawner and rolls NO shared-world spawn RNG;
// shared-world content arrives ONLY via the host wire (npc_sync / world_actor / the prop mirrors), or
// it is not shared: a spawner whose product has no mirror yet runs on every peer, each rolling its
// own. The deer, bp7 and hexahive spawners, measured rolling on a connected client, are such.

// One class/function table covers shared spawners and host-mirrored creatures' gameplay bodies.
// Script-gate watches cover late instances and bytecode-internal calls. They refuse only during
// an active client session, including its joining menu world; host and solo bodies keep running.
// Per-player spawners remain local. See docs/npcs-and-kerfur.md, "Which spawners a client refuses".

namespace coop::net {
class Session;
}  // namespace coop::net

namespace coop::spawn_authority {

// Register the table's watches, once for the process (this module never retires them), and store
// `session` for the refusal's gate. Called at the session's start, ahead of a joining client's world
// load: the lanes' own Install waits for the local pawn, and nothing orders the pawn before the
// world's first tick, so a watch registered there could let the first spawner ticks through. Each
// watch goes live once the game thread resolves its names, and refuses while the session holds the
// gate, which it does from its first tick. Any thread (the start runs on the timeline thread).
//
// A class is added to the table only once its product has a mirror: refusing a spawner whose output
// nothing replaces just deletes the content. A spawner that reaches its work through Delay or
// SetTimerDelegate needs a finer row than a class and a function: the first such family (beehive,
// mannequin) brings it, and mannequinSpawner also carries K2_DestroyActor reap duty, so it will need
// a (class, latent-UUID) grain. Discovery of new classes stays with the ini-gated rng_roll_census,
// not this module.
void Install(coop::net::Session* session);

}  // namespace coop::spawn_authority
