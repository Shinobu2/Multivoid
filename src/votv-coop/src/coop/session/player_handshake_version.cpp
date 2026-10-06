// coop/session/player_handshake_version.cpp -- the wire version gate.
//
// Third TU of the player_handshake module (see player_handshake_detail.h). Owns the validation that
// runs at the TOP of the Join handler: extract the peer's game target and build claim from the Join
// payload (pure pre-pass, zero identity minting), byte-equality check of the game target against
// our own coop::version::kGameTarget, on a host the verdict on the build claim, and the refuse
// action -- host: Kick-with-reason plus a deduped feed line; client: join_progress::Fail popup. The
// identity's other half, the build number, IS the packet header's protocol version: anything not
// byte-equal was cut upstream by ParseHeader, so this gate only ever runs between same-build
// peers.

#include "player_handshake_detail.h"

#include "coop/build_trust/build_trust.h"
#include "coop/comms/chat_feed.h"
#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/net/session.h"
#include "coop/session/join_progress.h"
#include "coop/version.h"
#include "ue_wrap/core/log.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace coop::player_handshake {
namespace {

// Pure pre-pass over the Join payload chain (eid, nick, skin, flags, colour, game, build claim):
// extracts the game-target field, the nick (for the host feed line) and the build claim (a pointer
// into the payload and the official bit), WITHOUT any side effects.
// Returns false on a malformed chain -- any length prefix overrunning the payload -- fail-closed,
// because both peers are the same build by the header prologue, so a well-formed Join always
// carries the full chain.
//
// THIS IS THE SECOND WALKER OF THAT CHAIN, and the two must agree field for field;
// HandleJoinMessage in player_handshake.cpp is the other; it stops after the colour, so the game
// target and the build claim are read only here. When the guid field was deleted from the
// Join, this walker still consumed TWO length-prefixed fields where one remained, read the flags
// byte as a length, ran off the end and refused every Join with "version field missing" -- on BOTH
// peers, with the pose stream then never starting. A field added or removed here must be changed in
// both, and the failure is loud but names the wrong field.
bool ExtractJoinVersionFields(const uint8_t* payload, size_t len,
                              std::string* outGame, std::wstring* outNick,
                              const uint8_t** outSha, bool* outOfficial) {
    size_t off = 4;  // [u32 senderElementId] (caller already checked len >= 4)
    // [u8 nicklen][nick]
    if (off + 1 > len) return false;
    {
        const size_t n = payload[off];
        if (off + 1 + n > len) return false;
        if (n > 0 && outNick)
            *outNick = FromUtf8(payload + off + 1, static_cast<int>(n));
        off += 1 + n;
    }
    // [u8 skinlen][skin] -- no guid field precedes it any more; the host derives the guid from the
    // proved key instead.
    {
        if (off + 1 > len) return false;
        const size_t n = payload[off];
        if (off + 1 + n > len) return false;
        off += 1 + n;
    }
    // [u8 flags] + [u8 has][r][g][b]
    if (off + 1 + 4 > len) return false;
    off += 1 + 4;
    // [u8 gamelen][game]
    if (off + 1 > len) return false;
    const size_t n = payload[off];
    if (off + 1 + n > len || n > 23) return false;
    outGame->assign(reinterpret_cast<const char*>(payload + off + 1), n);
    off += 1 + n;
    // [sha256 32][u8 flags] -- the build claim; bit 0 = the build verified its own release signature.
    if (off + coop::build_trust::kShaBytes + 1 > len) return false;
    *outSha = payload + off;
    *outOfficial = (payload[off + coop::build_trust::kShaBytes] & 1) != 0;
    return true;
}

// The wire-gate verdict (server/client-phrased -- the reason string travels in
// the GNS close and is read on EITHER end). `peerIsClient` = the validated peer
// is a client joining us-the-host; false = we-the-client validate the host's
// Join. Empty = compatible.
std::string WireVersionVerdict(const std::string& peerGame, bool peerIsClient) {
    const char* ourGame = coop::version::kGameTarget;
    if (peerGame != ourGame) {
        const std::string& srvGame = peerIsClient ? ourGame : peerGame;
        const std::string& cliGame = peerIsClient ? peerGame : ourGame;
        return "Game version mismatch: server plays VOTV " + srvGame +
               ", client has VOTV " + cliGame + ".";
    }
    return {};
}

// The first four bytes of a build's hash as 8 lowercase hex, enough to tell two builds apart in a
// log line and a popup's detail.
std::string ShaPrefixHex(const uint8_t* sha) {
    static const char kHex[] = "0123456789abcdef";
    std::string out;
    for (int i = 0; i < 4; ++i) {
        out.push_back(kHex[sha[i] >> 4]);
        out.push_back(kHex[sha[i] & 0xF]);
    }
    return out;
}

// Host feed-line dedup (a reconnect-looping refused client would otherwise spam
// the chat feed once per connection). One line per (nick + reason) per window;
// the WARN log stays undeduped -- it is the diagnostic.
uint64_t g_lastRefuseFeedMs = 0;
std::string g_lastRefuseFeedKey;
constexpr uint64_t kRefuseFeedDedupMs = 30000;

void PushRefuseFeedLineDeduped(const std::wstring& nick, const std::string& reason) {
    const std::string key = std::string(nick.begin(), nick.end()) + "|" + reason;
    const uint64_t now = ::GetTickCount64();
    if (key == g_lastRefuseFeedKey && now - g_lastRefuseFeedMs < kRefuseFeedDedupMs) return;
    g_lastRefuseFeedKey = key;
    g_lastRefuseFeedMs = now;
    const std::wstring wreason(reason.begin(), reason.end());  // ASCII by construction
    // History: a refused join is an EVENT in this lobby, not a passing notice.
    coop::chat_feed::Push(nick + L" was turned away: " + wreason,
                          coop::chat_feed::Keep::History);
}

}  // namespace

bool ValidateJoinVersionOrRefuse(coop::net::Session& session, int senderSlot,
                                 const uint8_t* payload, size_t payloadLen) {
    std::string peerGame;
    std::wstring refuseNick = L"A player";
    const uint8_t* peerSha = nullptr;
    bool peerOfficial = false;
    std::string verdict;
    net::EndReason code = net::EndReason::GameVersionRefused;
    if (!ExtractJoinVersionFields(payload, payloadLen, &peerGame, &refuseNick, &peerSha,
                                  &peerOfficial)) {
        verdict = "malformed join (version field missing)";
    } else {
        verdict = WireVersionVerdict(peerGame,
                                     session.role() == net::Role::Host);
    }
    // The host decides, both ways. MTA's server refuses a client below minclientversion
    // (reference/mtasa-blue/Server/mods/deathmatch/logic/CGame.cpp:1930); its build notes say a custom
    // public server admits only custom clients (reference/mtasa-blue/Shared/sdk/version.h:36), and the
    // open code enforces it: a non-public build's netcode version carries its branch id
    // (reference/mtasa-blue/Shared/sdk/version.h:119-131), the join is admitted only on equality
    // (reference/mtasa-blue/Server/mods/deathmatch/logic/CGame.cpp:1913) and the rest are refused as
    // DIFFERENT_BRANCH (CGame.cpp:2038-2055). Here the host holds that rule. net.allow_other_builds is
    // a deliberate divergence: no MTA setting relaxes its kind check, and here the host may choose to
    // admit another build. A client judges no host.
    bool admittedOther = false;
    if (verdict.empty() && session.role() == net::Role::Host) {
        const bool sameBytes =
            std::memcmp(peerSha, coop::build_trust::Self().sha256, coop::build_trust::kShaBytes) == 0;
        // The signature check runs only for a differing hash: a same-build join cannot change the
        // verdict. An official host refuses any other hash, a modified host an official client.
        bool hostOfficial = false;
        bool refusedKind = false;
        if (!sameBytes) {
            hostOfficial = coop::build_trust::SelfIsOfficial();
            refusedKind = hostOfficial || peerOfficial;
        }
        if (refusedKind) {
            if (coop::config::ResolveFlag(coop::config_registry::rows::net_allow_other_builds)) {
                admittedOther = true;
            } else if (hostOfficial) {
                verdict = "unofficial build " + ShaPrefixHex(peerSha);
                code = net::EndReason::UnofficialClientRefused;
            } else {
                verdict = "official build " + ShaPrefixHex(peerSha);
                code = net::EndReason::OfficialClientRefused;
            }
        }
    }
    if (verdict.empty()) {
        if (admittedOther) {
            UE_LOGI("player_handshake: admitted another build (slot=%d nick='%ls' sha=%s): "
                    "net.allow_other_builds is on",
                    senderSlot, SanitizeNickname(refuseNick).c_str(), ShaPrefixHex(peerSha).c_str());
        }
        return false;
    }
    refuseNick = SanitizeNickname(refuseNick);
    if (session.role() == net::Role::Host) {
        UE_LOGW("player_handshake: Join REFUSED (slot=%d nick='%ls' game='%s'): %s",
                senderSlot, refuseNick.c_str(), peerGame.c_str(), verdict.c_str());
        PushRefuseFeedLineDeduped(refuseNick, verdict + " [" + net::Describe(code).id + "]");
        char reason[128];
        std::snprintf(reason, sizeof(reason), "%s", verdict.c_str());
        // The close carries the code and the sentence: their popup names both.
        session.Kick(senderSlot, code, reason);
    } else {
        // We-the-client refused the HOST's Join: surface our own popup -- a browser or direct join
        // is Active, so Fail pops the dialog and aborts, while an env boot is log-only -- and let
        // the host's SYMMETRIC gate close the wire, since the mismatch is byte-symmetric.
        UE_LOGW("player_handshake: host's Join REFUSED (game='%s'): %s",
                peerGame.c_str(), verdict.c_str());
        coop::join_progress::Fail(net::EndReason::GameVersionMismatch, verdict);
    }
    return true;
}

}  // namespace coop::player_handshake
