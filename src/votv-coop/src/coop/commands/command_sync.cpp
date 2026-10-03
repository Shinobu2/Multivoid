// coop/commands/command_sync.cpp -- see coop/commands/command_sync.h.
//
// Shapes: chat_sync (the render-thread post, the host check, the slot fan-out), desk_ping_sync (the
// per-slot token bucket and the once-per-10-s notice), the ChatMessage / ChatLine dispatch cases.
// MTA runs a console command for the player the packet's socket names
// (reference/mtasa-blue/Server/mods/deathmatch/logic/CPacketTranslator.cpp:226-246) and Source
// runs a client's command for the client that sent it
// (reference/source-sdk-2013/src/game/server/client.cpp:1549-1625); the sender is never read
// from the payload here either.

#include "coop/commands/command_sync.h"

#include "coop/commands/command_dispatcher.h"
#include "coop/commands/command_line.h"

#include "coop/comms/chat_feed.h"
#include "coop/net/intent_bucket.h"
#include "coop/net/peer_identity.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/player/roster_ledger.h"
#include "coop/session/player_handshake.h"
#include "coop/text/utf8_codec.h"

#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/engine/engine.h"

#include <windows.h>

#include <atomic>
#include <cstring>
#include <random>
#include <string>
#include <vector>

namespace coop::command_sync {
namespace {

namespace GT = ue_wrap::game_thread;
using coop::commands::Caller;
using coop::commands::PlayerView;
using coop::net::kMaxPeers;

std::atomic<coop::net::Session*> g_session{nullptr};

// A sender's budget: a burst of three for a typed correction, then two a second. Deliberate
// divergence: LuckPerms limits per sender with one 500 ms window
// (reference/LuckPerms/common/src/main/java/me/lucko/luckperms/common/command/CommandManager.java:105)
// and MTA's console has no limit (reference/mtasa-blue/Server/mods/deathmatch/logic/CGame.cpp:2406-2415);
// the burst is ours.
constexpr coop::net::IntentBudget kBudget{3.0f, 2.0f};
// An over-budget or unreadable line is dropped; the sender is told at most this often. Deliberate
// divergence: LuckPerms tells the sender nothing, only the log
// (CommandManager.java:158-160); ours tells a person typing, who otherwise sees nothing.
constexpr uint64_t kSayEveryMs = 10000;
// The longest command word a log line carries.
constexpr size_t kLogWordMax = 32;

coop::net::IntentBucket g_bucket[kMaxPeers];
uint64_t g_nextSayMs[kMaxPeers] = {};
uint64_t g_nextBadMs[kMaxPeers] = {};

bool g_warnedUnreadableReply = false;
void (*g_observer)(std::string_view line) = nullptr;

// The check until the permission system installs its own: what it answers a player with nothing
// granted (the declared default) and an operator (everything).
// The permission system's check replaces this one when it lands; until then no explicit false exists.
bool InterimCheck(const Caller& caller, std::string_view /*node*/, bool defaultGranted) {
    return caller.isOperator || defaultGranted;
}

// The one source of chance (`@r`). Game thread only.
int Pick(int count) {
    static std::mt19937 rng{std::random_device{}()};
    if (count <= 0) return 0;
    return std::uniform_int_distribution<int>(0, count - 1)(rng);
}

const coop::commands::Policy g_policy{&InterimCheck, &Pick};

struct RegistryHolder {
    coop::commands::Registry registry;
    RegistryHolder() {
        if (!coop::commands::RegisterBuiltins(registry))
            UE_LOGE("command_sync: builtin registration refused");
    }
};

// A reply line on this peer's own feed: private, never in the history.
void Deliver(std::string_view utf8, const std::wstring& wide) {
    coop::chat_feed::Push(wide, coop::chat_feed::Keep::Transient);
    UE_LOGI("command_sync: reply '%s'", std::string(utf8).c_str());
    if (g_observer) g_observer(utf8);
}

void ReadPosition(PlayerView& v, void* actor) {
    ue_wrap::FVector p{};
    if (!actor || !ue_wrap::engine::TryGetActorLocation(actor, p)) return;
    v.hasPosition = true;
    v.x = p.X;
    v.y = p.Y;
    v.z = p.Z;
}

// The player record a command sees, built when the command runs, on the game thread: who sits in
// each slot from the roster ledger, each position read now. Nothing is cached. It runs only for a
// line this process dispatches itself, and only a server dispatches: the host's own line or a
// client's request on the host, or solo play. That slot reads the local nickname, guid and player,
// every other slot the ledger and its puppet.
std::vector<PlayerView> BuildPlayers(coop::net::Session* s) {
    std::vector<PlayerView> out;
    coop::players::Registry& reg = coop::players::Registry::Get();
    if (!s || !s->running()) {
        // Out of session: the local player alone, as the roster's lone row.
        PlayerView v;
        v.slot = 0;
        v.nick = coop::text::ToUtf8(coop::player_handshake::LocalNickname());
        v.playerId = coop::net::peer_identity::LocalGuid();
        ReadPosition(v, reg.Local());
        out.push_back(std::move(v));
        return out;
    }
    // DispatchLocal and OnRequest run only on their own server, the listen host or solo, whose
    // slot is 0.
    constexpr int ownSlot = 0;
    for (int slot = 0; slot < kMaxPeers; ++slot) {
        const coop::roster_ledger::Row& row = coop::roster_ledger::Get(slot);
        if (!row.occupied()) continue;
        PlayerView v;
        v.slot = slot;
        v.playerNo = row.playerNo;
        if (slot == ownSlot) {
            v.nick = coop::text::ToUtf8(coop::player_handshake::LocalNickname());
            v.playerId = coop::net::peer_identity::LocalGuid();
            ReadPosition(v, reg.Local());
        } else {
            v.nick = coop::text::ToUtf8(coop::roster_ledger::DisplayName(slot));
            v.playerId = row.guid;
            coop::RemotePlayer* puppet = reg.Puppet(static_cast<uint8_t>(slot));
            ue_wrap::FVector p{};
            if (puppet && puppet->TryGetLocation(p)) {
                v.hasPosition = true;
                v.x = p.X;
                v.y = p.Y;
                v.z = p.Z;
            }
        }
        out.push_back(std::move(v));
    }
    return out;
}

// The local operator's own line: the listen host, or solo play. Deliberate divergence: it passes
// every check because Source's listen host is the admin
// (reference/source-sdk-2013/src/game/server/util.cpp:622-647); MTA has no listen host, its console
// is an ACL account.
void DispatchLocal(const std::string& line) {
    const Caller self{0, 0, true};
    const auto result = coop::commands::Dispatch(Commands(), self, line,
                                                 BuildPlayers(g_session.load(std::memory_order_acquire)),
                                                 g_policy);
    for (const std::string& reply : result.replies) ReplyTo(self, reply);
}

// The command word of a line for the log, never its arguments (a /msg carries private text). A
// command name is [a-z0-9], so every byte outside 0x21..0x7E is written as '?': a quoted word may
// hold spaces (they would forge the "-> ran" marker) and U+0085 / U+2028 / U+2029 are multi-byte
// line breaks a reader splits on, none of which a client's word may put in a log line.
std::string LogWord(std::string_view line) {
    const coop::commands::ParsedLine parsed = coop::commands::SplitLine(line);
    std::string word = parsed.words.empty() ? std::string() : parsed.words[0];
    word = coop::text::CapUtf8Bytes(std::move(word), kLogWordMax);
    for (char& c : word) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x21 || u > 0x7E) c = '?';
    }
    return word;
}

// A request that cannot be read is dropped and told at most once per kSayEveryMs a sender.
// Deliberate divergence: MTA drops an unreadable command packet silently
// (reference/mtasa-blue/Server/mods/deathmatch/logic/packets/CCommandPacket.cpp:15-35,
// CPacketTranslator.cpp:251-256); ours answers, as a person typing would otherwise see nothing.
void BadRequest(const Caller& caller, uint64_t now) {
    const int slot = caller.slot;
    if (now < g_nextBadMs[slot]) return;
    g_nextBadMs[slot] = now + kSayEveryMs;
    ReplyTo(caller, "Could not read that command.");
    UE_LOGW("command_sync: slot %d sent an unreadable command line", slot);
}

}  // namespace

void Install(coop::net::Session* s) { g_session.store(s, std::memory_order_release); }

coop::commands::Registry& Commands() {
    UE_ASSERT_GAME_THREAD("command_sync::Commands");
    static RegistryHolder holder;
    return holder.registry;
}

void Submit(std::string line) {
    GT::Post([line = std::move(line)] {
        coop::net::Session* s = g_session.load(std::memory_order_acquire);
        const bool inSession = s && s->running();
        if (!inSession || s->role() == coop::net::Role::Host) {
            DispatchLocal(line);
            return;
        }
        // A client never dispatches: its line runs where its authority is, on the host. A bare `/`
        // has nothing to send; the hint is local text.
        if (line.empty()) {
            const std::string_view hint = coop::commands::kHelpHint;
            Deliver(hint, coop::text::FromUtf8Lossy(hint.data(), hint.size()));
            return;
        }
        coop::net::CommandRequestPayload p{};
        const std::string cut = coop::text::CapUtf8Bytes(line, sizeof(p.text));
        p.len = static_cast<uint8_t>(cut.size());
        std::memcpy(p.text, cut.data(), cut.size());
        if (!s->SendReliable(coop::net::ReliableKind::CommandRequest, &p, sizeof(p))) {
            const std::string_view failed = "Could not send the command.";
            Deliver(failed, coop::text::FromUtf8Lossy(failed.data(), failed.size()));
        }
    });
}

void OnRequest(const uint8_t* bytes, size_t len, int slot) {
    UE_ASSERT_GAME_THREAD("command_sync::OnRequest");
    coop::net::Session* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Host) return;
    if (slot < 1 || slot >= kMaxPeers) return;
    // A seated slot whose Join has not landed has no player id yet: its line is dropped, before the
    // bucket. MTA's server ignores a not-yet-joined player's commands
    // (reference/mtasa-blue/Server/mods/deathmatch/logic/CGame.cpp:2410, IsJoined); an unmodified
    // client sends its Join one tick after its slot arrives, so no typed line is lost.
    if (coop::roster_ledger::Get(slot).guid.empty()) return;
    const Caller caller{slot, s->peerGenerationForSlot(slot), false};

    const uint64_t now = ::GetTickCount64();
    if (!g_bucket[slot].Take(kBudget, now)) {
        if (now >= g_nextSayMs[slot]) {
            g_nextSayMs[slot] = now + kSayEveryMs;
            ReplyTo(caller, "Too many commands -- wait a moment.");
            UE_LOGW("command_sync: slot %d over the command rate; lines dropped", slot);
        }
        return;
    }

    coop::net::CommandRequestPayload p{};
    std::wstring wide;
    if (len != sizeof(p)) { BadRequest(caller, now); return; }
    std::memcpy(&p, bytes, sizeof(p));
    if (p.len < 1 || p.len > sizeof(p.text) || !coop::text::FromUtf8Strict(p.text, p.len, &wide)) {
        BadRequest(caller, now);
        return;
    }

    // MTA strips control codes where a command enters the console, as chat_sync does at its boundary
    // (reference/mtasa-blue/Server/mods/deathmatch/logic/CConsole.cpp:44). TAB is kept: MTA's
    // stripControlCodes drops every byte below 32
    // (reference/mtasa-blue/Shared/mods/deathmatch/logic/Utils.cpp:277-293), SanitizeUtf8 keeps TAB
    // as chat does, and SplitLine splits on ' ' only.
    const std::string line = coop::text::SanitizeUtf8(p.text, p.len);
    const auto result = coop::commands::Dispatch(Commands(), caller, line, BuildPlayers(s), g_policy);
    UE_LOGI("command_sync: slot %d /%s -> %s", slot, LogWord(line).c_str(),
            result.ran ? "ran" : "refused");
    for (const std::string& reply : result.replies) ReplyTo(caller, reply);
}

void ReplyTo(const Caller& to, std::string_view line) {
    UE_ASSERT_GAME_THREAD("command_sync::ReplyTo");
    if (to.slot <= 0) {
        Deliver(line, coop::text::FromUtf8Lossy(line.data(), line.size()));
        return;
    }
    coop::net::Session* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Host) return;
    // The slot may have changed hands since the asker typed: the line belongs to that person only.
    if (s->peerGenerationForSlot(to.slot) != to.generation) return;
    coop::net::CommandReplyPayload p{};
    const std::string cut = coop::text::CapUtf8Bytes(std::string(line), sizeof(p.text));
    p.len = static_cast<uint8_t>(cut.size());
    std::memcpy(p.text, cut.data(), cut.size());
    s->SendReliableToSlot(to.slot, coop::net::ReliableKind::CommandReply, &p, sizeof(p));
}

void OnReply(const coop::net::CommandReplyPayload& p) {
    UE_ASSERT_GAME_THREAD("command_sync::OnReply");
    std::wstring wide;
    // The strict decode of the RAW bytes is the gate: stripping a control byte out of an ill-formed
    // sequence could splice its neighbours into a valid one, a repair nobody sent.
    bool readable = p.len <= sizeof(p.text) && coop::text::FromUtf8Strict(p.text, p.len, &wide);
    // MTA's client strips control codes from every echo it shows
    // (reference/mtasa-blue/Client/mods/deathmatch/logic/CPacketHandler.cpp:1443), as our chat receiver
    // does (coop/comms/chat_sync.cpp:316).
    std::string line;
    if (readable) {
        line = coop::text::SanitizeUtf8(p.text, p.len);
        readable = coop::text::FromUtf8Strict(line.data(), line.size(), &wide);
    }
    if (!readable) {
        if (!g_warnedUnreadableReply) {
            g_warnedUnreadableReply = true;
            UE_LOGW("command_sync: an unreadable reply from the host");
        }
        return;
    }
    Deliver(line, wide);
}

void SetReplyObserver(void (*fn)(std::string_view line)) { g_observer = fn; }

void OnSlotDisconnected(int slot) {
    if (slot < 0 || slot >= kMaxPeers) return;
    g_bucket[slot].Reset();
    g_nextSayMs[slot] = 0;
    g_nextBadMs[slot] = 0;
}

void OnDisconnect() {
    for (int slot = 0; slot < kMaxPeers; ++slot) OnSlotDisconnected(slot);
    g_warnedUnreadableReply = false;
}

}  // namespace coop::command_sync
