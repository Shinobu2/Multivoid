// coop/dev/ban_drill.cpp -- see coop/dev/ban_drill.h.

#include "coop/dev/ban_drill.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/moderation/ban_list.h"
#include "coop/moderation/moderation.h"
#include "coop/net/session.h"
#include "coop/player/roster_ledger.h"
#include "coop/props/prop_snapshot.h"
#include "coop/save/join_window_baseline.h"

#include "ue_wrap/core/log.h"

#include <cstdint>
#include <string>
#include <vector>

namespace coop::dev::ban_drill {
namespace {

constexpr int kSlot = 1;  // the pair's client

enum class Step : uint8_t { Armed, Posted, Done };
Step g_step = Step::Armed;
// What the poll waits on: the kicked occupancy's generation, and the id its ban is stored under.
uint32_t g_gen = 0;
std::string g_id;

// The store is read as a list, not asked "is this banned?": that question logs a refusal.
bool IsStored(const std::string& id) {
    std::vector<coop::ban_list::Entry> bans;
    coop::ban_list::GetSnapshot(bans);
    for (const auto& e : bans) {
        if (id == e.id) return true;
    }
    return false;
}

void Report(const std::string& id, bool closed) {
    const bool stored = IsStored(id);
    if (stored && closed)
        UE_LOGI("[BAN-DRILL] PASS %.8s... is banned and its seat closed", id.c_str());
    else
        UE_LOGW("[BAN-DRILL] FAIL stored=%d closed=%d for %.8s...", stored ? 1 : 0,
                closed ? 1 : 0, id.c_str());
    g_step = Step::Done;
}

// The client's join traffic has all been DELIVERED, not merely queued: the settled-client test
// the other drills use (world ready, the snapshot bracket closed, the host's late window shut)
// plus every reliable byte to the slot acknowledged, so the kick drops nothing the join sent.
bool ClientSettled(coop::net::Session& s) {
    return s.IsSlotWorldReady(kSlot) && coop::prop_snapshot::IsBracketClosed(kSlot) &&
           !coop::join_window_baseline::IsLateWindowOpen(kSlot) && s.SlotReliableIdle(kSlot);
}

}  // namespace

void Tick(coop::net::Session* s) {
    static const bool s_on = coop::config::ResolveFlag(::coop::config_registry::rows::ban_drill);
    static const bool s_stale =
        coop::config::ResolveFlag(::coop::config_registry::rows::ban_drill_stale_token);
    if (!s_on) return;
    if (!s || s->role() != coop::net::Role::Host) return;
    // Before the settled gate: the kicked client is no longer settled, and the poll is what ends
    // the drill. No clock of its own -- a seat that is never freed is ended by the rig's phase budget.
    if (g_step == Step::Posted) {
        // The ban is stored and the kick's synchronous half is done when the verb returns, but the
        // slot is freed (its generation cleared) by the net thread at the top of its next pass.
        if (s->peerGenerationForSlot(kSlot) == g_gen) return;
        Report(g_id, /*closed*/true);
        return;
    }
    if (g_step != Step::Armed || !ClientSettled(*s)) return;

    const uint32_t gen = s->peerGenerationForSlot(kSlot);
    const std::string id = s->ProvedGuidForSlotWithToken(kSlot, gen);
    const auto& row = coop::roster_ledger::Get(kSlot);
    UE_LOGI("[BAN-DRILL] host banning slot %d (id %.8s..., gen %u)", kSlot, id.c_str(),
            static_cast<unsigned>(gen));
    const coop::moderation::ModResult res = coop::moderation::BanPlayer(
        coop::moderation::TokenFor(kSlot, row.playerNo, s_stale ? gen + 1 : gen), "ban drill", true);
    if (res != coop::moderation::ModResult::Done) {
        // Gone (the red arm's stale token) and the other refusals: the ban acted on nobody, so
        // there is no seat to wait for, and the verdict is read at once.
        Report(id, s->peerGenerationForSlot(kSlot) != gen);
        return;
    }
    g_gen = gen;
    g_id = id;
    g_step = Step::Posted;
}

void OnSessionEnd() {}

}  // namespace coop::dev::ban_drill
