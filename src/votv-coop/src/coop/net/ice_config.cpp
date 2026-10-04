// coop/net/ice_config.cpp -- see ice_config.h.

#include "ice_config.h"

#include "coop/net/endpoint_log.h"
#include "coop/net/net_clock.h"  // NowMs, the net layer's one steady clock
#include "ue_wrap/core/log.h"

#include <atomic>
#include <mutex>
#include <optional>
#include <utility>

#pragma warning(push)
#pragma warning(disable: 4100 4127 4191 4244 4245 4267 4310 4324 4458)
#include <steam/isteamnetworkingutils.h>
#include <steam/steamnetworkingtypes.h>
#pragma warning(pop)

namespace coop::net {
namespace {

// The applied credential's lapse on NowMs, and its stated lifetime for the line; 0 when the applied
// TURN list has no credential with a stated lifetime. Process-global, as the values GNS holds are.
std::atomic<uint64_t> g_turnLapseAtMs{0};
std::atomic<int>      g_turnTtlS{0};

// The applied credential's user, the renewals written since the apply, and a renewal waiting for the net
// thread; the mutex holds an apply, a renewal's queueing and its write apart.
struct QueuedRenewal {
    std::string    replaces;
    TurnCredential fresh;
};
std::mutex                   g_turnMu;
std::string                  g_appliedTurnUser;
int                          g_turnRenewals = 0;
std::optional<QueuedRenewal> g_queuedRenewal;
std::atomic<bool>            g_renewalQueued{false};

// A new lifetime count from now; none for a credential with no stated lifetime.
void ArmTurnLapse(bool timed, int ttlS) {
    g_turnTtlS.store(timed ? ttlS : 0, std::memory_order_relaxed);
    g_turnLapseAtMs.store(timed ? NowMs() + uint64_t(ttlS) * 1000 : 0, std::memory_order_relaxed);
}

// The net thread's write of a queued renewal.
void WriteQueuedRenewal() {
    if (!g_renewalQueued.load(std::memory_order_acquire)) return;
    std::lock_guard<std::mutex> lk(g_turnMu);
    g_renewalQueued.store(false, std::memory_order_relaxed);
    if (!g_queuedRenewal) return;
    const QueuedRenewal q = std::move(*g_queuedRenewal);
    g_queuedRenewal.reset();
    if (g_appliedTurnUser != q.replaces) {
        UE_LOGI("ice: a renewed turn credential was not written -- the session holds another than the one "
                "it replaces");
        return;
    }
    auto* utils = SteamNetworkingUtils();
    // GNS refuses a string write only for a key or type it does not know, so a refusal here is a broken
    // build; the session's credential is then unknown, and no later renewal claims it.
    const auto set = [utils](ESteamNetworkingConfigValue key, const std::string& value) {
        return utils->SetGlobalConfigValueString(key, value.c_str());
    };
    const char* refused = nullptr;
    if (!utils)
        refused = "every list (no utils)";
    else if (!set(k_ESteamNetworkingConfig_P2P_TURN_ServerList, q.fresh.uri))
        refused = "the TURN list";
    else if (!set(k_ESteamNetworkingConfig_P2P_TURN_UserList, q.fresh.user))
        refused = "the TURN user list";
    else if (!set(k_ESteamNetworkingConfig_P2P_TURN_PassList, q.fresh.pass))
        refused = "the TURN password list";
    if (refused) {
        g_appliedTurnUser.clear();
        UE_LOGE("ice: GNS refused %s of a renewed turn credential -- the session's credential is unknown",
                refused);
        return;
    }
    g_appliedTurnUser = q.fresh.user;
    ++g_turnRenewals;
    ArmTurnLapse(q.fresh.ttlS > 0, q.fresh.ttlS);
    if (q.fresh.ttlS > 0)
        UE_LOGI("ice: turn credential renewed (%d), valid %d s", g_turnRenewals, q.fresh.ttlS);
    else
        UE_LOGI("ice: turn credential renewed (%d), with no stated lifetime", g_turnRenewals);
}

}  // namespace

bool ApplyGlobalIceConfig(const IceConfig& ice) {
    auto* utils = SteamNetworkingUtils();
    if (!utils) {
        UE_LOGE("ice: SteamNetworkingUtils() null -- GNS not initialized");
        return false;
    }
    std::lock_guard<std::mutex> lk(g_turnMu);
    // Whatever the writes below leave, no earlier credential or renewal is the session's any more.
    g_appliedTurnUser.clear();
    g_turnRenewals = 0;
    g_queuedRenewal.reset();
    g_renewalQueued.store(false, std::memory_order_relaxed);
    ArmTurnLapse(false, 0);

    // Every value is written, because these are process-global and a session must not run on its
    // predecessor's: a previous session's relay-only policy, or a TURN credential minted for a
    // lobby that ended, stays in effect until something overwrites it. An empty STUN list is
    // meaningful to GNS ("NAT piercing will not be attempted"), and an empty TURN list offers no
    // relay candidate. Each write is checked by name; the first refusal ends the chain and the
    // start, since the values after it are still the previous session's.
    const char* refused = nullptr;
    if (!utils->SetGlobalConfigValueInt32(
            k_ESteamNetworkingConfig_P2P_Transport_ICE_Enable,
            ice.relayOnly ? k_nSteamNetworkingConfig_P2P_Transport_ICE_Enable_Relay
                          : k_nSteamNetworkingConfig_P2P_Transport_ICE_Enable_All))
        refused = "the candidate policy";
    else if (!utils->SetGlobalConfigValueString(
                 k_ESteamNetworkingConfig_P2P_STUN_ServerList, ice.stunList.c_str()))
        refused = "the STUN list";
    else if (!utils->SetGlobalConfigValueString(
                 k_ESteamNetworkingConfig_P2P_TURN_ServerList, ice.turnList.c_str()))
        refused = "the TURN list";
    else if (!utils->SetGlobalConfigValueString(
                 k_ESteamNetworkingConfig_P2P_TURN_UserList, ice.turnUser.c_str()))
        refused = "the TURN user list";
    else if (!utils->SetGlobalConfigValueString(
                 k_ESteamNetworkingConfig_P2P_TURN_PassList, ice.turnPass.c_str()))
        refused = "the TURN password list";
    if (refused) {
        UE_LOGE("ice: GNS refused %s -- not applied (policy=%s)", refused,
                ice.relayOnly ? "relay" : "all");
        return false;
    }

    UE_LOGI("ice: applied policy=%s stun='%s' turn='%s'",
            ice.relayOnly ? "relay" : "all",
            ice.stunList.empty() ? "(none)" : endpoint_log::LogEndpointList(ice.stunList).c_str(),
            ice.turnList.empty() ? "(none)" : endpoint_log::LogEndpointList(ice.turnList).c_str());
    // Counted from here, not from the mint: the mint came first, so a lapse printed by this count
    // is never early.
    const bool timed = !ice.turnList.empty() && ice.turnTtlS > 0;
    ArmTurnLapse(timed, ice.turnTtlS);
    g_appliedTurnUser = ice.turnList.empty() ? std::string() : ice.turnUser;
    if (timed)
        UE_LOGI("ice: turn credential valid %d s", ice.turnTtlS);
    else if (!ice.turnList.empty())
        UE_LOGI("ice: turn credential with no stated lifetime");
    return true;
}

void TickTurnCredential(uint64_t nowMs) {
    WriteQueuedRenewal();
    uint64_t at = g_turnLapseAtMs.load(std::memory_order_relaxed);
    if (at == 0 || nowMs < at) return;
    // The exchange makes the line once per apply, whichever thread gets there first.
    if (!g_turnLapseAtMs.compare_exchange_strong(at, 0, std::memory_order_relaxed)) return;
    UE_LOGI("ice: turn credential lapsed, %d s after it was written -- a connection whose ICE starts "
            "from now gets no relay candidate from it", g_turnTtlS.load(std::memory_order_relaxed));
}

std::string AppliedTurnUser() {
    std::lock_guard<std::mutex> lk(g_turnMu);
    return g_appliedTurnUser;
}

void ForgetTurnCredential() {
    std::lock_guard<std::mutex> lk(g_turnMu);
    g_appliedTurnUser.clear();
    g_turnRenewals = 0;
    g_queuedRenewal.reset();
    g_renewalQueued.store(false, std::memory_order_relaxed);
    ArmTurnLapse(false, 0);
}

bool QueueTurnRenewal(const std::string& replaces, const TurnCredential& fresh) {
    if (replaces.empty() || fresh.uri.empty() || fresh.user.empty() || fresh.pass.empty()) {
        UE_LOGW("ice: a renewed turn credential came with a field missing -- not queued");
        return false;
    }
    std::lock_guard<std::mutex> lk(g_turnMu);
    g_queuedRenewal = QueuedRenewal{replaces, fresh};
    g_renewalQueued.store(true, std::memory_order_release);
    return true;
}

}  // namespace coop::net
