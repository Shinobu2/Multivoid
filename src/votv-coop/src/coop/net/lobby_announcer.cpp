// coop/net/lobby_announcer.cpp -- see coop/net/lobby_announcer.h.

#include "coop/net/lobby_announcer.h"

#include "coop/net/http_client.h"
#include "coop/net/peer_identity.h"  // LocalIdentityString -- the identity joiners dial
#include "coop/net/protocol.h"  // kProtocolVersion -- announced for the browser's join gate
#include "coop/session/shutdown.h"
#include "coop/version.h"  // kGameTarget -- the announced identity pair's game half
#include "ice_config.h"  // co-located: AppliedTurnUser, QueueTurnRenewal
#include "json_util.h"  // internal, co-located in src/coop/net/ (not a public API header)
#include "ue_wrap/core/log.h"

#include <chrono>
#include <thread>
#include <utility>

namespace coop::net::lobby {
namespace {

namespace J = coop::net::jsonu;

}  // namespace

LobbyAnnouncer::~LobbyAnnouncer() { Stop(); }

HostInfo LobbyAnnouncer::Host(const std::string& masterUrl, const std::string& name,
                              const std::string& world,
                              bool locked, int playersMax, int timeoutMs, int directPort) {
    HostInfo info;
    Stop();  // retire any prior lobby first (sends /leave, joins) -- on a worker, safe

    // nlohmann builds + escapes the body (name/world are user-supplied -> may contain
    // quotes/backslashes; never string-concat them into JSON). J::Dump uses the
    // replace error handler so invalid-UTF-8 input can't throw out of this worker.
    J::Json b;
    b["name"] = name;
    // The Paper-pair identity: the game target plus the build number (proto). There is no "version"
    // field -- the mod-semver axis was retired whole (RULE 2).
    b["game"] = coop::version::kGameTarget;      // join gate tier 1
    b["proto"] = static_cast<int>(coop::net::kProtocolVersion);  // the build number (tier 2)
    b["world"] = world;
    // The identity joiners dial. It is this install's durable PUBLIC KEY rendered `gen:<64 hex>` --
    // the same value the admission challenge proves possession of, so the rendezvous name and the
    // provable name are one thing (RULE 2). It is REQUIRED: the master mints no per-session name to
    // stand in for a host that sends none, and `/v1/host` answers a missing or malformed identity
    // with a named 400. There is no fallback, because a minted name is one the master asserts on
    // our behalf, and the relay registers only names their holder signed for.
    b["identity"] = coop::net::peer_identity::LocalIdentityString();
    b["locked"] = locked;
    b["players_max"] = playersMax;
    if (directPort > 0) {  // DIRECT lobby (see header): master advertises src-ip:port
        b["conn"] = "direct";
        b["direct_port"] = directPort;
    }
    const http::Response resp = http::Post(masterUrl, "/v1/host", J::Dump(b), timeoutMs);
    // No master address in these lines: they reach the console, and which master was asked is the
    // caller's to name (master_slots::DisplayName).
    if (!resp.ok) { UE_LOGW("lobby: host announce -- master unreachable"); return info; }
    if (resp.status != 200) { UE_LOGW("lobby: host announce -- master returned %d", resp.status); return info; }

    J::Json j;
    if (!J::ParseObject(resp.body, j)) { UE_LOGW("lobby: host announce -- malformed response"); return info; }
    info.sessionId      = J::Str(j, "sessionId");
    info.lobbyId        = J::Str(j, "lobbyId");
    info.token          = J::Str(j, "token");
    info.hostIdentity   = J::Str(j, "hostIdentity");
    info.signalingUrl   = J::Str(j, "signalingUrl");
    info.signalingToken = J::Str(j, "signalingToken");
    info.stun           = J::Str(j, "stun");
    info.turn           = J::ParseTurnCredential(j);
    // A DIRECT host is answered without the signaling block (the master's h_host: a direct host
    // never uses signaling), so only a P2P announce needs signalingUrl.
    info.ok = !info.sessionId.empty() && !info.token.empty() &&
              !info.hostIdentity.empty() && (directPort > 0 || !info.signalingUrl.empty());
    if (!info.ok) {
        UE_LOGW("lobby: host announce -- response missing session/token/identity%s",
                directPort > 0 ? "" : "/signaling");
        return info;
    }

    // Stash the creds + (re)start the heartbeat under threadMu_ so a concurrent Host
    // can never move-assign over a joinable hbThread_ (which would std::terminate).
    {
        std::lock_guard<std::mutex> tl(threadMu_);
        StopHeartbeatLocked();  // join anything that snuck in since the top Stop()
        {
            std::lock_guard<std::mutex> lk(mu_);
            masterUrl_ = masterUrl;
            sessionId_ = info.sessionId;
            token_     = info.token;
            lobbyId_   = info.lobbyId;
            turnCur_   = info.turn.user;
            turnPrev_.clear();
            listed_    = true;
        }
        stop_.store(false);
        active_.store(true);
        hbThread_ = std::thread(&LobbyAnnouncer::HeartbeatLoop, this);
    }
    UE_LOGI("lobby: announced '%s' lobbyId=%s -- heartbeating", name.c_str(), info.lobbyId.c_str());
    return info;
}

void LobbyAnnouncer::HeartbeatLoop() {
    // The master stamped last_seen at /v1/host, so the first beat is one interval out.
    // 30 s: three beats to the master's 90 s lobby expiry, so one lost beat does not delist.
    // Sleep in 0.5s slices so Stop()/shutdown is responsive; bail on global shutdown so
    // we never issue HTTP into a tearing-down process.
    while (!stop_.load() && !coop::shutdown::IsShuttingDown()) {
        for (int i = 0; i < 60 && !stop_.load() && !coop::shutdown::IsShuttingDown(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        if (stop_.load() || coop::shutdown::IsShuttingDown()) break;

        // The credential the host's session holds is reported while it is one of this lobby's, so the
        // master can renew it or re-send the renewal the host missed; a session holding another is not
        // this lobby's (a late beat of a retired lobby) and asks for nothing.
        const std::string held = AppliedTurnUser();
        std::string url, sid, tok;
        bool listed, ours;
        {
            std::lock_guard<std::mutex> lk(mu_);
            url = masterUrl_; sid = sessionId_; tok = token_; listed = listed_;
            ours = !held.empty() && (held == turnCur_ || held == turnPrev_);
        }
        int (*const countFn)() = playerCountFn_.load(std::memory_order_acquire);
        const int pc = countFn ? countFn() : 1;

        J::Json b;
        b["sessionId"] = sid;
        b["token"] = tok;
        b["players_cur"] = pc;
        b["listed"] = listed;
        if (LobbyLinks (*const linksFn)() = linksFn_.load(std::memory_order_acquire)) {
            const LobbyLinks l = linksFn();
            b["links"] = J::Json{{"relayed", l.relayed}, {"direct", l.direct}, {"lan", l.lan}};
        }
        if (ours) b["turn_user"] = held;
        const http::Response resp = http::Post(url, "/v1/heartbeat", J::Dump(b), 8000);
        if (!resp.ok || resp.status != 200) {
            UE_LOGW("lobby: heartbeat -> %s (status=%d) -- lobby may expire if this persists",
                    resp.ok ? "non-200" : "unreachable", resp.status);
            continue;
        }
        // A renewal in the answer goes to the net thread, which writes it where no accept runs.
        if (!ours || stop_.load()) continue;
        J::Json j;
        if (!J::ParseObject(resp.body, j)) continue;
        const TurnCredential fresh = J::ParseTurnCredential(j);
        if (fresh.user.empty()) continue;
        // Only a queued renewal becomes one of this lobby's: rotating on a refused one would leave the
        // session's own credential in neither slot, and the host would never report it again.
        if (!QueueTurnRenewal(held, fresh)) continue;
        std::lock_guard<std::mutex> lk(mu_);
        if (fresh.user != turnCur_) {
            turnPrev_ = turnCur_;
            turnCur_ = fresh.user;
        }
    }
}

void LobbyAnnouncer::SetListed(bool listed) {
    std::string url, pub, tok;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!active_.load() || lobbyId_.empty()) return;
        listed_ = listed;
        url = masterUrl_; pub = lobbyId_; tok = token_;
    }
    // Fire-and-forget on a detached worker (don't block the UI thread on a round-trip).
    J::Json b;
    b["lobbyId"] = pub;
    b["token"] = tok;
    b["listed"] = listed;
    const std::string body = J::Dump(b);
    std::thread([url, body, listed] {
        if (coop::shutdown::IsShuttingDown()) return;
        const http::Response resp = http::Post(url, "/v1/visibility", body, 8000);
        UE_LOGI("lobby: visibility listed=%d -> %s (%d)", listed ? 1 : 0,
                resp.ok ? "ok" : "unreachable", resp.status);
    }).detach();
}

void LobbyAnnouncer::StopHeartbeatLocked() {
    // Caller holds threadMu_. Signals + joins the heartbeat thread. Does NOT send
    // /leave (Stop()'s job). Safe when no thread is running (joinable() is false).
    stop_.store(true);
    if (hbThread_.joinable()) hbThread_.join();
}

void LobbyAnnouncer::Stop() {
    bool was;
    std::string url, sid, tok;
    {
        std::lock_guard<std::mutex> tl(threadMu_);
        was = active_.exchange(false);
        StopHeartbeatLocked();
        // Copy + clear the creds INSIDE threadMu_: Host() writes them under threadMu_
        // too, so a concurrent re-announce is fully serialized against this Stop() --
        // its fresh sessionId can never be consumed by THIS call's /leave below.
        std::lock_guard<std::mutex> lk(mu_);
        url = masterUrl_; sid = sessionId_; tok = token_;
        sessionId_.clear(); token_.clear(); lobbyId_.clear(); turnCur_.clear(); turnPrev_.clear();
    }
    if (!was) return;  // nothing was announced -> no /leave to send
    if (!url.empty() && !sid.empty()) {
        J::Json b;
        b["sessionId"] = sid;
        b["token"] = tok;
        const http::Response resp = http::Post(url, "/v1/leave", J::Dump(b), 5000);
        UE_LOGI("lobby: left lobby -> %s (%d)", resp.ok ? "ok" : "unreachable", resp.status);
    }
}

}  // namespace coop::net::lobby
