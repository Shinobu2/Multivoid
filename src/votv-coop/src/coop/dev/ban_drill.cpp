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

#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"

#include <cstdint>
#include <string>
#include <vector>

namespace coop::dev::ban_drill {
namespace {

namespace GT = ue_wrap::game_thread;

constexpr int kSlot = 1;  // the pair's client

enum class Step : uint8_t { Armed, Posted, Done };
Step g_step = Step::Armed;

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
    if (g_step != Step::Armed || !ClientSettled(*s)) return;

    const uint32_t gen = s->peerGenerationForSlot(kSlot);
    const std::string id = s->ProvedGuidForSlotWithToken(kSlot, gen);
    const auto& row = coop::roster_ledger::Get(kSlot);
    UE_LOGI("[BAN-DRILL] host banning slot %d (id %.8s..., gen %u)", kSlot, id.c_str(),
            static_cast<unsigned>(gen));
    coop::moderation::BanPlayer(
        coop::moderation::TokenFor(kSlot, row.playerNo, s_stale ? gen + 1 : gen), "ban drill", true);
    g_step = Step::Posted;

    // The ban above is already done (the verb is synchronous), and the kick's teardown is
    // synchronous too (KickClaimed), so the generation is already cleared when the ban took.
    GT::Post([s, id, gen] {
        // The store is read as a list, not asked "is this banned?": that question logs a refusal.
        std::vector<coop::ban_list::Entry> bans;
        coop::ban_list::GetSnapshot(bans);
        bool stored = false;
        for (const auto& e : bans) {
            if (id == e.id) {
                stored = true;
                break;
            }
        }
        const bool closed = s->peerGenerationForSlot(kSlot) != gen;
        if (stored && closed)
            UE_LOGI("[BAN-DRILL] PASS %.8s... is banned and its seat closed", id.c_str());
        else
            UE_LOGW("[BAN-DRILL] FAIL stored=%d closed=%d for %.8s...", stored ? 1 : 0,
                    closed ? 1 : 0, id.c_str());
        g_step = Step::Done;
    });
}

void OnSessionEnd() {}

}  // namespace coop::dev::ban_drill
