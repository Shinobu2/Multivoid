// coop/moderation/moderation.h -- host-side player-admin actions: kick, ban, unban, teleport-to-me.
//
// The command handlers (coop/commands/moderation_commands) are the callers, and the dispatcher
// decides who may run them: a verb here never asks the permission system. Each verb runs on the
// GAME THREAD, synchronously, and returns a ModResult the handler turns into the caller's reply.
//
// A verb acts only inside a running HOSTED session (HostedSessionRunning): defence in depth,
// because a destructive kick or ban must never run off a client or after a hosted session ended
// even if a future caller misuses it. Outside one it does nothing and says NoSession.
//
// Ban keys: the PROVED player id and an enforceable address (when one was stored: a direct path,
// never loopback or a relay), both checked at the identity proof through the injected
// Session::SetBanCheck. Principle 7: policy over a policy-free net layer, which never calls us.

#pragma once

#include <cstdint>
#include <string>

namespace coop::net { class Session; }

namespace coop::moderation {

// THE TOKEN. Every slot-addressed destructive action takes a PlayerToken, not a bare slot, and
// the token is in the SIGNATURE so a tokenless call does not compile -- the defence cannot be
// forgotten at a call site.
//
// The measured defect it closes: a ban's target slot was captured when its modal OPENED, then
// executed after an arbitrary typing delay, while slots recycle lowest-free. A permanent ban
// could therefore land on the SUCCESSOR -- a different person who merely inherited the seat.
// The UI now names the person by the proved player id (an id survives slot reuse), and the
// command handler builds the token from the player record of the moment the command runs.
//
// A token is (slot, playerNo, generation) read from ONE ledger row. The generation is validated
// against the LIVE net-layer authority at execution time, so a stale capture fails CLOSED.
// Validating playerNo against playerNo inside the same mirror would fail OPEN and merely narrow
// the window to a tick.

// The captured identity of a moderation target. Build it with TokenFor from the ledger row.
struct PlayerToken {
    int      slot = -1;
    uint16_t playerNo = 0;   // for the log lines and the ledger check
    uint32_t generation = 0; // what the net layer validates against
    bool valid() const { return slot >= 1 && playerNo != 0 && generation != 0; }
};

// Build a token from a ledger row's fields. PURE and thread-free.
inline PlayerToken TokenFor(int slot, uint16_t playerNo, uint32_t generation) {
    PlayerToken t;
    t.slot = slot;
    t.playerNo = playerNo;
    t.generation = generation;
    return t;
}

// What a verb did. Done: it acted. NoSession: no running hosted session. Gone: the token's player
// is no longer in that seat. NoId: the target has no (valid) player id to key a ban on. NotBanned:
// an unban of an id that was not banned.
enum class ModResult : uint8_t { Done, NoSession, Gone, NoId, NotBanned };

// Cache the Session pointer (used by the verbs). Called once at host boot, alongside the other
// modules' SetSession.
void SetSession(coop::net::Session* session);

// True while a hosted session runs in this process: the session pointer is set, the session is
// running and its role is Host. The one answer to "is there a running hosted session". Game
// thread; logs nothing.
bool HostedSessionRunning();

// The slot's remote address as a ban key, or empty. An address is a ban key only when it is the
// player's own, on a direct path: GNS clears the address of an ICE-relayed path
// (p2p_ice.cpp:621-623) and an unknown path has none. The read is gated on `generation`, so it
// never names a successor. Any thread.
std::string EnforceableAddress(const coop::net::Session& s, int slot, uint32_t generation);

// Disconnect the captured player; `reason` rides the close (null/empty reads "kicked by host").
// Gone when the target has since left or been replaced -- see the token note above.
ModResult KickPlayer(const PlayerToken& token, const char* reason);

// Permanently ban the captured player by the id their identity proof established and, when
// `byAddress` is set and the host saw their own address on a direct path, that address too; then
// kick them -- MTA's order in CStaticFunctionDefinitions::BanPlayer. Divergence: MTA's BanPlayer/
// AddBan also kick every seated player matching the ban's IP (CStaticFunctionDefinitions.cpp:
// 11998-12001, :12157-12170, under mtasa-blue's Server/mods/deathmatch/logic); ours kicks the
// banned id only, because bystanders at a shared address (a household router, a carrier address) keep their
// session and meet the refusal at their next proof -- the named cost, undone by Unban.
// The ban survives host restarts and is checked at the identity proof of every future join;
// `reason` is stored on the record and rides the banned player's close (null/empty reads "banned
// by host", applied before the ban is stored).
// Gone -- writing no ban and kicking nobody -- if the slot no longer holds the captured player;
// NoId if it does but their identity proof has not landed. This is the whole point of the token:
// a permanent ban is the least reversible thing the host can do, so it must never land on
// whoever happens to hold the seat now.
ModResult BanPlayer(const PlayerToken& token, const char* reason, bool byAddress);

// Permanently ban a player id nobody seated holds (an offline player, or one never seen here:
// MTA bans a serial it has not seen). `id` is 32 hex digits, lowered here; NoId for anything
// else. The nick and last enforceable address come from coop::seen_players when it has a record
// (the address only when `byAddress`), else the ban has an empty nick. A matching player still
// seated is kicked (by id only; see the BanPlayer note on MTA's wider IP kick).
ModResult BanOffline(const char* id, const char* reason, bool byAddress);

// Remove a ban. NotBanned for an id that was not banned.
ModResult Unban(const char* playerId);

// Teleport the captured player to the host's current pose. Token-taking like the others:
// teleporting the wrong person is not destructive, but a consistent rule is what keeps the check
// from being forgotten where it matters.
ModResult TeleportPlayerToMe(const PlayerToken& token);

}  // namespace coop::moderation
