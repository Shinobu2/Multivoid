// coop/moderation/moderation.h -- host-side player-admin actions (the player-list action menu).
//
// The host's interactive scoreboard (ui::scoreboard) calls these when the host clicks a player
// row, and this module is the single entry point for the three actions: KICK and BAN are always
// available to the host, TELEPORT-TO-ME is the dev-gated one -- the scoreboard shows it only
// when [dev] devkeys is on, and this module does not re-check the flag, it performs the
// teleport.
//
// All three are HOST-only and self-gate on Session::Role::Host: defence in depth, because a
// destructive kick or ban must never run off a client even if a future caller misuses it. Each
// action marshals onto the game thread, so the scoreboard's render-thread click does no net or
// disk work inline and the game-thread-asserted nick lookup is legal.
//
// Principle 7: policy orchestration over a policy-free net layer, which learns about bans only
// through the injected Session::SetBanCheck predicate (asked at the proof), never by calling us.

#pragma once

#include <cstdint>
#include <string>

namespace coop::net { class Session; }

namespace coop::moderation {

// THE TOKEN. Every slot-addressed destructive action takes a PlayerToken, not a bare slot, and
// the token is in the SIGNATURE so a tokenless call does not compile -- the defence cannot be
// forgotten at a call site.
//
// The measured defect it closes: the ban modal captured its target slot when the modal OPENED,
// then executed after an arbitrary typing delay, while slots recycle lowest-free. A permanent
// ban could therefore land on the SUCCESSOR -- a different person who merely inherited the
// seat.
//
// A token is (slot, playerNo, generation) read from ONE ledger row. The generation is validated
// against the LIVE net-layer authority at execution time, so a stale capture fails CLOSED.
// Validating playerNo against playerNo inside the same mirror would fail OPEN and merely narrow
// the window to a tick.

// The captured identity of a moderation target. Build it with TokenForSlot at
// the moment the admin picks the row; carry it, unchanged, to the action.
struct PlayerToken {
    int      slot = -1;
    uint16_t playerNo = 0;   // for the log line + the confirm dialog
    uint32_t generation = 0; // what the net layer validates against
    bool valid() const { return slot >= 1 && playerNo != 0 && generation != 0; }
};

// Build a token from a published roster row. PURE and thread-free on purpose:
// the scoreboard runs on the RENDER thread and must not read the game-thread
// ledger, so the capture rides the POD snapshot the game thread already
// publishes (coop::roster::Row).
inline PlayerToken TokenFor(int slot, uint16_t playerNo, uint32_t generation) {
    PlayerToken t;
    t.slot = slot;
    t.playerNo = playerNo;
    t.generation = generation;
    return t;
}

// Cache the Session pointer (used by KickPlayer / BanPlayer). Called once at host
// boot, alongside the other modules' SetSession.
void SetSession(coop::net::Session* session);

// The slot's remote address as a ban key, or empty. An address is a ban key only when it is the
// player's own, on a direct path: GNS clears the address of an ICE-relayed path
// (p2p_ice.cpp:621-623) and an unknown path has none. The read is gated on `generation`, so it
// never names a successor. Any thread.
std::string EnforceableAddress(const coop::net::Session& s, int slot, uint32_t generation);

// Disconnect the captured player. Host-only. Safe to call from the render thread
// (posts to the game thread). No-op if the target has since left or been
// replaced -- see the token note above.
void KickPlayer(const PlayerToken& token);

// Permanently ban the captured player by the id their identity proof established and, when
// `byAddress` is set and the host saw their own address on a direct path, that address too; then
// kick them -- MTA's order in CStaticFunctionDefinitions::BanPlayer, whose KickPlayer/BanPlayer
// pair this module mirrors. Host-only. The ban survives host restarts (coop::ban_list persists to
// disk) and is checked at the identity proof of every future join; `reason` is stored on the
// record and rides the banned player's close (null/empty is fine). Safe to call from the render
// thread. The id and the address are read BEFORE the kick because the kick clears the slot.
//
// ABORTS -- writing no ban and kicking nobody -- if the slot no longer holds the captured player,
// or their identity proof has not landed. This is the whole point of the token: a permanent ban
// is the least reversible thing the host can do, so it must never land on whoever happens to hold
// the seat now.
void BanPlayer(const PlayerToken& token, const char* reason, bool byAddress);

// Permanently ban an OFFLINE player by its seen-players GUID (the F1 Administration panel's
// Offline-section ban). Resolves the player's nick and last enforceable address from
// coop::seen_players (the address only when `byAddress`); warns and does nothing for an unknown
// GUID. A matching player still seated is kicked, as MTA's AddBan disconnects a matching player.
// Host-only. Safe to call from the render thread.
void BanOffline(const char* guid, const char* reason, bool byAddress);

// Remove a ban (the F1 Administration panel's Unban button). Host-only; posted to the game thread
// like the other verbs, so the scoreboard's render-thread click does no net or disk work inline.
// Any thread.
void Unban(const char* playerId);

// Teleport the captured player to the host's current pose. Host-only. Token-taking
// like the others: teleporting the wrong person is not destructive, but a
// consistent rule is what keeps the check from being forgotten where it matters.
void TeleportPlayerToMe(const PlayerToken& token);

}  // namespace coop::moderation
