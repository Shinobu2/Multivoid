// coop/net/session_teardown.cpp -- the slot teardown family: the one close every kick, ban and
// leave runs for a connection whose slot the caller has claimed.

#include "coop/net/session.h"

#include "ue_wrap/core/log.h"

#pragma warning(push)
#pragma warning(disable: 4100 4127 4191 4244 4245 4267 4310 4324 4458)
#include <steam/steamnetworkingsockets.h>
#pragma warning(pop)

namespace coop::net {

// The teardown for a connection whose slot the caller has already claimed (exchanged or CAS'd to
// 0), shared by the blind and the token-checked entry points.
bool Session::KickClaimed(int peerSlot, uint32_t hConn, EndReason code, const char* reason) {
    peerLanesConfigured_[peerSlot].store(false, std::memory_order_release);
    // The delivery guarantee is scoped to the connection: the queued state dies with the peer, and
    // the link measurement with it.
    backlog_.FreeSlot(peerSlot);
    rateControl_.FreeSlot(peerSlot);
    admission_.FreeSlot(peerSlot);
    relayEligible_[peerSlot].store(0, std::memory_order_release);

    if (auto* sockets = SteamNetworkingSockets()) {
        // No linger: a kick drops the peer at once. The reason rides to the peer's status callback,
        // so a kicked client can say why.
        sockets->CloseConnection(static_cast<HSteamNetConnection>(hConn),
                                 ToTransportEnd(code),
                                 reason ? reason : Describe(code).text, /*bEnableLinger*/false);
    }

    // A connection we close is not closed by its peer, so the close path's teardown does not run for
    // it and is replicated here; the claim above makes a racing callback's FindPeerSlotForConn
    // return -1, or its own claim fail if it found the slot first, so the teardown runs exactly once.
    { std::lock_guard<std::mutex> lk(remoteMutex_); ResetPeerRemoteState(peerSlot); }
    { std::lock_guard<std::mutex> lk(reliableInboxMutex_);
      for (auto it = reliableInbox_.begin(); it != reliableInbox_.end();) {
          if (it->senderPeerSlot == peerSlot) it = reliableInbox_.erase(it);
          else ++it;
      } }
    // The proved identity goes first: a recycled slot must not carry its predecessor's storage
    // name. The generation clear is the last per-slot write, after the inbox erase (the
    // ClosedByPeer path says why the order matters).
    SetProvedGuidForSlot(peerSlot, 0, std::string());
    peerGenBySlot_[peerSlot].store(0, std::memory_order_release);

    // Aggregate state, as in the ClosedByPeer branch.
    if (connectedPeerCount() == 0) {
        state_.store(ConnState::Disconnected);
        linkStage_.store(static_cast<uint8_t>(LinkStage::Idle), std::memory_order_release);
        { std::lock_guard<std::mutex> lk(remoteMutex_);
          for (int i = 0; i < kMaxPeers; ++i) ResetPeerRemoteState(i); }
        { std::lock_guard<std::mutex> lk(reliableInboxMutex_); reliableInbox_.clear(); }
        for (auto& r : rttMsBySlot_) r.store(-1, std::memory_order_relaxed);  // per-slot RTT reset
    }
    UE_LOGI("net: kicked peer slot %d [%s] (reason='%s')", peerSlot, Describe(code).id,
            reason ? reason : Describe(code).text);
    return true;
}

}  // namespace coop::net
