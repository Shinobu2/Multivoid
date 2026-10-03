// coop/moderation/moderation.cpp -- see coop/moderation/moderation.h.

#include "coop/moderation/moderation.h"

#include "coop/moderation/ban_list.h"
#include "coop/moderation/seen_players.h"
#include "coop/session/teleport_client.h"
#include "coop/net/link_kind.h"
#include "coop/net/session.h"
#include "coop/player/roster.h"
#include "coop/player/roster_ledger.h"
#include "coop/player/players_registry.h"
#include "coop/text/utf8_codec.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"

#include <windows.h>

#include <atomic>
#include <string>

namespace coop::moderation {

namespace GT = ue_wrap::game_thread;

namespace {

std::atomic<coop::net::Session*> g_session{nullptr};

// Resolve the Session iff we are the host. Returns nullptr (and logs) otherwise,
// so every action is a no-op off-host.
coop::net::Session* HostSession(const char* action) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Host) {
        UE_LOGI("moderation: %s ignored -- host-only (local role is not Host)", action);
        return nullptr;
    }
    return s;
}

bool ValidClientSlot(int slot) {
    return slot >= 1 && slot < static_cast<int>(coop::players::kMaxPeers);
}

}  // namespace

void SetSession(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
}

std::string EnforceableAddress(const coop::net::Session& s, int slot, uint32_t generation) {
    char a[64] = {};
    if (!s.GetPeerAddressWithToken(slot, generation, a, sizeof(a))) return {};
    if (!coop::ban_list::IsBannableAddress(a)) return {};
    const coop::net::LinkKind kind = s.LinkKindForSlot(slot);
    if (kind != coop::net::LinkKind::Lan && kind != coop::net::LinkKind::Direct) return {};
    // The kind read is not token-gated: the slot must still be the same occupancy after it.
    if (s.peerGenerationForSlot(slot) != generation) return {};
    return a;
}

void KickPlayer(const PlayerToken& token) {
    if (!token.valid()) return;
    GT::Post([token] {
        auto* s = HostSession("kick");
        if (!s) return;
        if (s->KickWithToken(token.slot, token.generation, coop::net::EndReason::KickedByHost,
                             "kicked by host"))
            UE_LOGI("moderation: kicked #%u (slot %d)",
                    static_cast<unsigned>(token.playerNo), token.slot);
        else
            UE_LOGW("moderation: kick of #%u (slot %d) did nothing -- they are already "
                    "gone, or someone else now holds that slot",
                    static_cast<unsigned>(token.playerNo), token.slot);
    });
}

void BanPlayer(const PlayerToken& token, const char* reason, bool byAddress) {
    if (!token.valid()) return;
    GT::Post([token, byAddress, reason = std::string(reason ? reason : "")] {
        auto* s = HostSession("ban");
        if (!s) return;
        // The proved id FIRST, token-gated: it is the ban's key, and reading a successor's id and
        // writing it to the permanent list is exactly the failure this path exists to prevent.
        // Read before the kick, which clears the slot.
        const std::string id = s->ProvedGuidForSlotWithToken(token.slot, token.generation);
        if (id.empty()) {
            UE_LOGW("moderation: ban of #%u ABORTED -- slot %d no longer holds that player, or "
                    "their identity proof has not landed",
                    static_cast<unsigned>(token.playerNo), token.slot);
            return;
        }
        char nick[coop::text::kNickBufBytes] = {};
        coop::text::CopyUtf8ToBuffer(nick, coop::roster_ledger::Get(token.slot).nick);

        // The second abort: the ledger's player number, for a slot whose roster row moved on while
        // the net layer's generation still matches.
        if (coop::roster_ledger::Get(token.slot).playerNo != token.playerNo) {
            UE_LOGW("moderation: ban of #%u ABORTED -- slot %d now holds #%u",
                    static_cast<unsigned>(token.playerNo), token.slot,
                    static_cast<unsigned>(coop::roster_ledger::Get(token.slot).playerNo));
            return;
        }

        const std::string address =
            byAddress ? EnforceableAddress(*s, token.slot, token.generation) : std::string();
        if (!coop::ban_list::Add(id.c_str(), nick, address.c_str(), reason.c_str())) return;
        // The typed reason rides the close as its text, so the banned player reads it under the
        // code; the constant stands in when none was typed.
        const char* why = reason.empty() ? "banned by host" : reason.c_str();
        // Accepted window: the banned player's re-proof over a second connection, whose ban check
        // ran on the net thread just before the Add above, is seated for this session. MTA has
        // none: its join packets are queued by the sync thread and handled on the main thread
        // that also runs its bans (CNetBuffer.cpp:1289-1294, :932-952). Ours proves on the net
        // thread and runs the verbs on the game thread.
        if (!s->KickWithToken(token.slot, token.generation, coop::net::EndReason::BannedByHost, why))
            UE_LOGW("moderation: ban #%u -- kick did nothing (already gone?)",
                    static_cast<unsigned>(token.playerNo));
        else
            UE_LOGI("moderation: banned + kicked #%u (slot %d, id %.8s..., address %s)",
                    static_cast<unsigned>(token.playerNo), token.slot, id.c_str(),
                    address.empty() ? "not enforced" : address.c_str());
    });
}

void BanOffline(const char* guid, const char* reason, bool byAddress) {
    if (!guid || !guid[0]) return;
    GT::Post([guid = std::string(guid), reason = std::string(reason ? reason : ""), byAddress] {
        auto* s = HostSession("offline ban");
        if (!s) return;
        coop::seen_players::Entry e;
        if (!coop::seen_players::FindByGuid(guid.c_str(), e)) {
            UE_LOGW("moderation: offline ban -- unknown GUID %s", guid.c_str());
            return;
        }
        // The record's last address, only when it can name one machine: an old record's "::" or
        // loopback is dropped.
        const std::string address =
            (byAddress && coop::ban_list::IsBannableAddress(e.ip)) ? std::string(e.ip) : std::string();
        if (!coop::ban_list::Add(guid.c_str(), e.nick, address.c_str(), reason.c_str())) return;
        UE_LOGI("moderation: offline-banned '%s' (id %.8s..., address %s)", e.nick, guid.c_str(),
                address.empty() ? "not enforced" : address.c_str());
        // A matching player still seated goes too. MTA's AddBan also kicks every seated player
        // matching the ban's IP (CStaticFunctionDefinitions.cpp:12157-12170); ours kicks the banned
        // id only, so bystanders at a shared address keep their session and meet the refusal at
        // their next proof (the named cost of an address key, undone by Unban).
        // Accepted window, both conditions at once: a joiner whose ban check ran on the net
        // thread just before the Add above AND whose id is published just after this scan is
        // seated for this session. MTA has none: its join packets are queued by the sync thread
        // and handled on the main thread that also runs its bans (CNetBuffer.cpp:1289-1294,
        // :932-952). Ours proves on the net thread and runs the verbs on the game thread.
        for (int k = 1; k < static_cast<int>(coop::players::kMaxPeers); ++k) {
            const uint32_t gen = s->peerGenerationForSlot(k);
            if (gen != 0 && s->ProvedGuidForSlotWithToken(k, gen) == guid)
                s->KickWithToken(k, gen, coop::net::EndReason::BannedByHost,
                                 reason.empty() ? "banned by host" : reason.c_str());
        }
    });
}

void Unban(const char* playerId) {
    if (!playerId || !playerId[0]) return;
    if (!coop::roster::LocalIsHost()) {
        UE_LOGI("moderation: unban ignored -- host-only (local role is not Host)");
        return;
    }
    // Posted, not inline: the render-thread click does no net or disk work.
    GT::Post([id = std::string(playerId)] { coop::ban_list::Remove(id.c_str()); });
}

void TeleportPlayerToMe(const PlayerToken& token) {
    if (!token.valid()) return;
    // teleport_client self-gates on host + posts to the game thread itself. The
    // token is re-checked there, on the game thread, where the ledger is legal to
    // read -- a slot whose occupant changed teleports nobody.
    GT::Post([token] {
        if (coop::roster_ledger::Get(token.slot).playerNo != token.playerNo) {
            UE_LOGW("moderation: teleport of #%u skipped -- slot %d changed hands",
                    static_cast<unsigned>(token.playerNo), token.slot);
            return;
        }
        coop::teleport_client::TeleportSlotToHost(token.slot);
    });
}

}  // namespace coop::moderation
