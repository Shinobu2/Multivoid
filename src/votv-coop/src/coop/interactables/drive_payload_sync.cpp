// coop/interactables/drive_payload_sync.cpp -- see coop/interactables/drive_payload_sync.h.

#include "coop/interactables/drive_payload_sync.h"

#include "drive_payload_retry.h"  // co-located private header: the retry rules, engine-free

#include "coop/element/registry.h"
#include "coop/interactables/desk_snd_fx.h"      // ScopedWireApply (the shared desk wire guard)
#include "coop/interactables/drive_rack_sync.h"  // TryConsumeDenyReap: a denied rack take's ghost
#include "coop/interactables/signal_wire.h"
#include "coop/net/blob_chunks.h"
#include "coop/net/session.h"
#include "coop/props/prop_lifecycle.h"           // DestroyLocalProp (the deny-ghost teardown)
#include "coop/props/prop_save_data.h"           // DeclareClassOwnedElsewhere
#include "coop/session/join_progress.h"          // a joiner's parked rows wait out its join

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/desk/drive_chain.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace coop::drive_payload_sync {
namespace {

namespace R  = ue_wrap::reflection;
namespace DC = ue_wrap::drive_chain;
namespace SD = ue_wrap::signal_dynamic;
namespace sg = ue_wrap::script_gate;
namespace RT = coop::drive_payload_retry;
using Clock = std::chrono::steady_clock;
using coop::element::LivePropActor;
using RT::Gate;
using RT::Leftover;

constexpr int  kTagUpd = 0x44525550;  // 'DRUP'
constexpr auto kPendingTtl = std::chrono::seconds(10);
constexpr auto kNoteTtl = std::chrono::seconds(30);
constexpr auto kSayEvery = std::chrono::seconds(10);
constexpr auto kClassRetry = std::chrono::seconds(5);
constexpr size_t kPendingCap = 256;

std::atomic<coop::net::Session*> g_session{nullptr};

// A row this peer keeps for a drive, with the drive it was kept for: the registry recycles an eid, and a row kept by
// eid alone would be put back into the next drive bound under it. The handle's serial and world tell a successor in
// the same slot, or a drive of a world since replaced, from the drive itself.
struct Kept {
    ue_wrap::CachedObjRef ref;
    uint64_t hash = 0;
    SD::Row  row;
};
std::map<uint32_t, Kept> g_lastSent;  // HOST: what every client holds, by what the host last sent or the class default
std::map<uint32_t, Kept> g_held;      // CLIENT: the host's last row, or this player's own sent row

// A row that arrived before its drive bound here, the newest per eid.
struct Parked { std::vector<uint8_t> blob; uint8_t senderSlot = 0xFF; Clock::time_point since{}; };
std::map<uint32_t, Parked> g_parked;

// A row whose write into the live drive failed (the struct write can stop part way) is retried a bounded number of
// times through the park, with the author's permission kept; the count is per eid and cleared on success. The count
// permits nothing: a retry is held to the same rule as a first write, bar a leftover of its own (below).
constexpr uint32_t kWriteRetries = 30;
std::map<uint32_t, uint32_t> g_writeFails;

// HOST: a drive a failed write left part-written, its rollback not landing either (drive_payload_retry.h). No writer
// chose that row, so no client is sent it; only that author's write may land over it, and only while the drive still
// reads exactly that. Any other row found in its place is a writer's, the host's row from then on. With no retry of the
// write waiting it is put back to the previous row, a bounded number of times. Ends with its drive.
struct LeftoverRec { ue_wrap::CachedObjRef ref; Leftover lo; };
std::map<uint32_t, LeftoverRec> g_leftover;

struct Noted { ue_wrap::CachedObjRef ref; Clock::time_point until; };
std::vector<Noted> g_noted;  // CLIENT: drives this player brought into the world, until their eid binds

// HOST: the drives a client brought into the world, whose one row the host takes from that client alone. By actor
// until the drive enrols (a drop intent's copy has no eid yet), then by eid until the row comes.
struct Brought { ue_wrap::CachedObjRef ref; uint8_t slot; Clock::time_point until; };
std::vector<Brought> g_broughtPending;
struct Author { ue_wrap::CachedObjRef ref; uint8_t slot = 0xFF; };
std::map<uint32_t, Author> g_brought;  // an entry ends with its drive, so a re-issued eid inherits no author

void NoteAuthor(uint32_t eid, void* actor, uint8_t slot) {
    Author& a = g_brought[eid];
    a.ref.Set(actor);
    a.slot = slot;
}

struct Enrolled { void* actor; uint32_t eid; };
std::mutex g_enrolledMu;
std::vector<Enrolled> g_enrolled;  // MarkPropElement's enrolments, drained at Tick

// A class's default row hash, read from its default object; a failed read is retried, never cached.
struct ClassDefault { uint64_t hash = 0; bool ok = false; Clock::time_point retryAt{}; };
std::map<std::wstring, ClassDefault> g_classDefault;

bool g_applying = false;  // inside this lane's own apply or put-back
uint32_t g_nextSeq = 1;
coop::blob_chunks::Assembler g_asm;
Clock::time_point g_nextSlow{};
Clock::time_point g_nextStats{};
Clock::time_point g_nextRefusalSay[coop::net::kMaxPeers] = {};
Counts g_counts;

enum class Reg : uint8_t { Pending, Registered, Refused };
Reg  g_reg = Reg::Pending;
bool g_settled = false;

// The lane's own write is not a writer: a client does not put it back, the host sends it itself. Restored on every
// exit, an absorbed fault's unwind included, so a fault inside a write cannot leave every later deviation standing.
struct ApplyScope {
    bool prev;
    ApplyScope() : prev(g_applying) { g_applying = true; }
    ~ApplyScope() { g_applying = prev; }
};

bool IsHost(coop::net::Session* s) { return s && s->role() == coop::net::Role::Host; }

std::vector<uint8_t> Bytes(const SD::Row& row) { return coop::signal_wire::Serialize(row); }
uint64_t Hash(const SD::Row& row) { return coop::blob_chunks::Fnv64(Bytes(row)); }

// The row a drive of this class materialises with on a peer that ran no writer on it; false while unreadable.
bool ClassDefaultHash(void* actor, uint64_t& out) {
    const std::wstring cls = R::ClassNameOf(actor);
    ClassDefault& c = g_classDefault[cls];
    if (!c.ok) {
        const auto now = Clock::now();
        if (now < c.retryAt) return false;
        SD::Row row;
        void* cdo = R::FindClassDefaultObject(cls.c_str());
        if (!cdo || !DC::ReadDriveRow(cdo, row)) {
            c.retryAt = now + kClassRetry;
            return false;
        }
        c.hash = Hash(row);
        c.ok = true;
    }
    out = c.hash;
    return true;
}
bool IsClassDefault(void* actor, uint64_t hash) {
    uint64_t d = 0;
    return ClassDefaultHash(actor, d) && d == hash;
}

Kept* Find(std::map<uint32_t, Kept>& m, uint32_t eid, void* actor) {
    auto it = m.find(eid);
    if (it == m.end()) return nullptr;
    if (it->second.ref.Get() != actor || !it->second.ref.Alive()) {
        m.erase(it);
        return nullptr;
    }
    return &it->second;
}

void Keep(std::map<uint32_t, Kept>& m, uint32_t eid, void* actor, uint64_t hash, const SD::Row* row) {
    Kept& k = m[eid];
    k.ref.Set(actor);
    k.hash = hash;
    if (row) k.row = *row;
}

// Drops the rows kept for drives that are gone, and the authors of those drives: a drive's rows end with it.
template <typename Map>
void SweepKept(Map& m) {
    for (auto it = m.begin(); it != m.end();) it = it->second.ref.Alive() ? std::next(it) : m.erase(it);
}

// HOST: the leftover of this very drive, or null; one kept for a drive since gone or replaced under the eid is dropped.
Leftover* FindLeftover(uint32_t eid, void* actor) {
    auto it = g_leftover.find(eid);
    if (it == g_leftover.end()) return nullptr;
    if (!it->second.ref.Is(actor)) {
        g_leftover.erase(it);
        return nullptr;
    }
    return &it->second.lo;
}

// Only for a leftover whose residue was read: an unread one is never Changed.
void SayChanged(uint32_t eid, const Leftover& lo, const SD::Row& now) {
    UE_LOGW("drive_payload_sync: eid=%u -- a writer changed the drive a failed write left part-written (it reads '%ls', "
            "size %.0f; the write left '%ls', %.0f; before it '%ls', %.0f): the host's row now, slot %u's write will "
            "not land over it", eid, now.name.c_str(), now.size, lo.left ? lo.left->name.c_str() : L"<unread>",
            lo.left ? lo.left->size : -1.f, lo.pre.name.c_str(), lo.pre.size, lo.slot);
}

// Kept, with the evidence said: `left` names what the drive read, or that the read failed.
void KeepLeftover(uint32_t eid, void* actor, Leftover lo, const char* what) {
    UE_LOGW("drive_payload_sync: eid=%u -- %s: the drive reads %ls'%ls' (size %.0f), not its previous row '%ls' "
            "(slot %u's write); withheld from every client%s", eid, what, lo.left ? L"" : L"<unread> ",
            lo.left ? lo.left->name.c_str() : L"", lo.left ? lo.left->size : -1.f, lo.pre.name.c_str(), lo.slot,
            lo.left ? "" : ", never rewritten or put back by this lane (an unread residue is no row to compare)");
    LeftoverRec& r = g_leftover[eid];
    r.ref.Set(actor);
    r.lo = std::move(lo);
}

// HOST: true while the drive still reads what a failed write left, no writer's row and none to send. A row found in
// its place is a writer's, and the leftover ends there.
bool Withheld(uint32_t eid, void* actor, const SD::Row& now) {
    Leftover* lo = FindLeftover(eid, actor);
    if (!lo) return false;
    if (RT::Withholds(*lo, now)) return true;
    SayChanged(eid, *lo, now);
    g_leftover.erase(eid);
    return false;
}

std::vector<uint8_t> Blob(uint32_t eid, const std::vector<uint8_t>& bytes) {
    std::vector<uint8_t> out(4 + bytes.size());
    std::memcpy(out.data(), &eid, 4);
    std::memcpy(out.data() + 4, bytes.data(), bytes.size());
    return out;
}

// To one slot, or to every client whose world is ready but `exceptSlot`.
bool SendBytes(coop::net::Session* s, uint32_t eid, const std::vector<uint8_t>& bytes, int toSlot, int exceptSlot = -1) {
    if (!s || !s->connected()) return false;
    const std::vector<uint8_t> blob = Blob(eid, bytes);
    if (toSlot >= 0)
        return coop::blob_chunks::SendBlobToSlot(s, toSlot, coop::net::ReliableKind::DrivePayload, g_nextSeq++, blob);
    bool any = false, targets = false;
    const uint32_t seq = g_nextSeq++;
    for (int slot = 1; slot < coop::net::kMaxPeers; ++slot) {
        if (slot == exceptSlot || !s->IsSlotWorldReady(slot)) continue;
        targets = true;
        any = coop::blob_chunks::SendBlobToSlot(s, slot, coop::net::ReliableKind::DrivePayload, seq, blob) || any;
    }
    return !targets || any;  // no client to send to is a vacuous success: a joiner's seed carries the row
}

// HOST: send a drive's row to every ready client, bar `exceptSlot`, when it differs from what they hold.
void HostSendIfChanged(coop::net::Session* s, uint32_t eid, void* actor, int exceptSlot = -1) {
    SD::Row row;
    if (!DC::ReadDriveRow(actor, row) || Withheld(eid, actor, row)) return;
    const std::vector<uint8_t> bytes = Bytes(row);
    const uint64_t h = coop::blob_chunks::Fnv64(bytes);
    if (Kept* k = Find(g_lastSent, eid, actor); k && k->hash == h) return;
    if (!SendBytes(s, eid, bytes, -1, exceptSlot)) return;
    Keep(g_lastSent, eid, actor, h, nullptr);
    ++g_counts.sent;
}

bool TakeNote(void* actor) {
    const auto now = Clock::now();
    for (Noted& n : g_noted) {
        if (n.ref.Get() == actor && now < n.until && n.ref.Alive()) {
            n = Noted{};
            return true;
        }
    }
    return false;
}

// CLIENT: this player's own drive has its eid; its row goes to the host, and is held as sent.
void SendOwn(coop::net::Session* s, uint32_t eid, void* actor) {
    SD::Row row;
    if (!DC::ReadDriveRow(actor, row)) return;
    const std::vector<uint8_t> bytes = Bytes(row);
    const uint64_t h = coop::blob_chunks::Fnv64(bytes);
    if (IsClassDefault(actor, h)) return;  // a class-default drive has nothing the host lacks
    if (!SendBytes(s, eid, bytes, 0)) {
        UE_LOGW("drive_payload_sync: CLIENT own drive eid=%u's row was not sent (the session refused it)", eid);
        return;
    }
    Keep(g_held, eid, actor, h, &row);
    ++g_counts.ownSent;
    UE_LOGI("drive_payload_sync: CLIENT own drive eid=%u ('%ls', size %.0f) -- row to the host", eid, row.name.c_str(),
            row.size);
}

// Every game writer of the row is followed by upd() on its peer. The host sends a change; a client puts back a drive
// whose row left the one it holds, and leaves one it holds nothing for. Neither inside this lane's own apply, which
// does its own sending.
void OnUpdPost(const sg::Call& call) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running() || !s->connected() || g_applying) return;
    void* actor = call.object;
    const uint32_t eid = static_cast<uint32_t>(coop::element::Registry::Get().EidForActor(actor));
    if (eid == coop::element::kInvalidId) return;  // a newborn: its enrolment or its bind carries it
    if (IsHost(s)) {
        HostSendIfChanged(s, eid, actor);
        return;
    }
    Kept* k = Find(g_held, eid, actor);
    if (!k) return;
    SD::Row row;
    if (!DC::ReadDriveRow(actor, row) || Hash(row) == k->hash) return;
    const SD::Row keep = k->row;
    {
        ApplyScope scope;
        coop::desk_snd_fx::ScopedWireApply guard;
        if (DC::WriteDriveRow(actor, keep)) DC::CallDriveUpd(actor);
    }
    ++g_counts.putBack;
    UE_LOGI("drive_payload_sync: CLIENT drive eid=%u put back to the held row ('%ls', size %.0f; it read '%ls', %.0f)",
            eid, keep.name.c_str(), keep.size, row.name.c_str(), row.size);
}

void Park(uint32_t eid, const std::vector<uint8_t>& blob, uint8_t senderSlot) {
    if (g_parked.size() >= kPendingCap && g_parked.find(eid) == g_parked.end()) {
        UE_LOGW("drive_payload_sync: parked-row cap hit -- eid=%u dropped", eid);
        return;
    }
    const bool fresh = g_parked.find(eid) == g_parked.end();
    Parked& p = g_parked[eid];  // the newest row for an eid wins; it waits from the first
    p.blob = blob;
    p.senderSlot = senderSlot;
    if (fresh) {
        p.since = Clock::now();
        UE_LOGI("drive_payload_sync: row for eid=%u parked -- its drive is not bound here yet", eid);
    }
}

// A client's row a host could write: finite and non-negative where the download reads it as an amount.
bool RowSane(const SD::Row& r) {
    const float f[] = {r.size, r.decoded, r.downloadedAtQuality, r.locX, r.locY};
    for (float v : f)
        if (!std::isfinite(v)) return false;
    return r.size >= 0.f && r.decoded >= 0.f;
}

void SayRefusal(uint8_t slot, uint32_t eid, const char* why) {
    const auto now = Clock::now();
    if (slot >= coop::net::kMaxPeers || now < g_nextRefusalSay[slot]) return;
    g_nextRefusalSay[slot] = now + kSayEvery;
    UE_LOGW("drive_payload_sync: HOST refused slot %u's row for eid=%u (%s) -- answered with its own; %llu refused",
            slot, eid, why, static_cast<unsigned long long>(g_counts.refused));
}

// HOST: a client's row, taken once from the client that brought the drive while the host's copy is still at the class
// default. Any other is answered with the host's own row, to that client alone. True when taken.
enum class Take { Taken, Refused, WriteFailed };

Take HostTakeClientRow(coop::net::Session* s, uint32_t eid, void* actor, const SD::Row& row, uint8_t senderSlot) {
    // A denied rack take's ghost names itself by its row: the one the winning take removed.
    if (coop::drive_rack_sync::TryConsumeDenyReap(senderSlot, Hash(row))) {
        coop::prop_lifecycle::DestroyLocalProp(actor, /*deferred*/true);
        UE_LOGW("drive_payload_sync: reaped the denied rack-take ghost eid=%u from slot %u", eid, senderSlot);
        return Take::Refused;
    }
    const char* why = nullptr;
    auto author = g_brought.find(eid);
    SD::Row mine;
    if (!DC::ReadDriveRow(actor, mine)) return Take::WriteFailed;
    // A retry is held to the class default like a first write; only its own failed write's leftover, still unchanged,
    // stands in for it. A failure count is no permission: the host may have written the drive since.
    Leftover* lo = FindLeftover(eid, actor);
    const Gate gate = RT::GateFor(lo, mine, !lo && IsClassDefault(actor, Hash(mine)), senderSlot);
    if (gate == Gate::Changed) {
        SayChanged(eid, *lo, mine);
        g_leftover.erase(eid);
        lo = nullptr;
    }
    if (author == g_brought.end() || author->second.slot != senderSlot || !author->second.ref.Is(actor))
        why = "not a drive that client brought";
    else if (gate == Gate::Changed) why = "a writer changed it after that client's failed write";
    else if (gate == Gate::Held) why = lo ? "a failed write left it, not that client's or unread" : "the host already holds a row for it";
    else if (!RowSane(row)) why = "a row with a non-finite or negative amount";
    if (why) {
        if (!lo) SendBytes(s, eid, Bytes(mine), senderSlot);  // a leftover is no row to answer with
        ++g_counts.refused;
        SayRefusal(senderSlot, eid, why);
        return Take::Refused;
    }
    // The author's permission is spent only by a write that landed: a failed write (the struct write can stop part
    // way) puts the host's previous row back, keeps the permission, and the row is retried. The previous row is the one
    // from before the first failed attempt; a rollback that does not land leaves a leftover. One a retry finds is taken
    // out before the writes, so nothing nested in them inherits it.
    std::optional<Leftover> prior;
    if (lo) {
        prior = std::move(*lo);
        g_leftover.erase(eid);
    }
    {
        ApplyScope scope;
        coop::desk_snd_fx::ScopedWireApply guard;
        SD::Row landed;
        if (!DC::WriteDriveRow(actor, row) || !DC::ReadDriveRow(actor, landed) || Hash(landed) != Hash(row)) {
            SD::Row back;
            DC::WriteDriveRow(actor, prior ? prior->pre : mine);
            const bool read = DC::ReadDriveRow(actor, back);
            if (auto left = RT::AfterFailedWrite(prior ? &*prior : nullptr, mine, read ? &back : nullptr, senderSlot))
                KeepLeftover(eid, actor, std::move(*left), "the failed write's rollback did not land either");
            return Take::WriteFailed;
        }
        DC::CallDriveUpd(actor);
    }
    g_brought.erase(eid);
    // Every other client gets it; its author holds it already (MTA sends an accepted change to all but its source,
    // CGame.cpp:2761-2768).
    HostSendIfChanged(s, eid, actor, /*exceptSlot*/ senderSlot);
    ++g_counts.accepted;
    UE_LOGI("drive_payload_sync: HOST took slot %u's row for the drive it brought, eid=%u ('%ls', size %.0f)", senderSlot,
            eid, row.name.c_str(), row.size);
    return Take::Taken;
}

// A write that did not land goes back to the park for another try, a bounded number of times.
void RetryWrite(uint32_t eid, const std::vector<uint8_t>& blob, uint8_t senderSlot) {
    const uint32_t n = ++g_writeFails[eid];
    if (n <= kWriteRetries) {
        if (n == 1) UE_LOGW("drive_payload_sync: the row for eid=%u did not land -- retrying", eid);
        Park(eid, blob, senderSlot);
        return;
    }
    g_writeFails.erase(eid);
    if (g_leftover.find(eid) != g_leftover.end())
        UE_LOGW("drive_payload_sync: the row for eid=%u did not land after %u tries -- given up; the drive holds what "
                "the failed write left, withheld from every client while its previous row is put back", eid,
                kWriteRetries);
    else
        UE_LOGW("drive_payload_sync: the row for eid=%u did not land after %u tries -- given up, the previous row "
                "stands", eid, kWriteRetries);
}

void ApplyBlob(const std::vector<uint8_t>& blob, uint8_t senderSlot, bool fromParked) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || blob.size() < 5) return;
    uint32_t eid = 0;
    std::memcpy(&eid, blob.data(), 4);
    const bool host = IsHost(s);
    if (host == (senderSlot == 0)) return;  // rows go client to host and host to client, never relayed
    void* actor = LivePropActor(eid);
    if (!actor || !DC::IsDriveClass(R::ClassOf(actor))) {
        if (!fromParked) Park(eid, blob, senderSlot);
        return;
    }
    g_parked.erase(eid);
    SD::Row row;
    const std::vector<uint8_t> rb(blob.begin() + 4, blob.end());
    if (!coop::signal_wire::Deserialize(rb, row)) {
        if (host) SayRefusal(senderSlot, eid, "a malformed row");
        else UE_LOGW("drive_payload_sync: the host's row for eid=%u is malformed -- dropped", eid);
        return;
    }
    if (host) {
        const Take t = HostTakeClientRow(s, eid, actor, row, senderSlot);
        if (t == Take::WriteFailed) RetryWrite(eid, blob, senderSlot);
        else g_writeFails.erase(eid);
        return;
    }
    // Only a row that landed is the host's row here; a partial write is retried, never recorded as applied.
    bool landed = false;
    {
        ApplyScope scope;
        coop::desk_snd_fx::ScopedWireApply guard;
        SD::Row check;
        landed = DC::WriteDriveRow(actor, row) && DC::ReadDriveRow(actor, check) && Hash(check) == Hash(row);
        if (landed) DC::CallDriveUpd(actor);
    }
    if (!landed) {
        RetryWrite(eid, blob, senderSlot);
        return;
    }
    SD::Row applied;
    if (!DC::ReadDriveRow(actor, applied)) applied = row;
    g_writeFails.erase(eid);
    Keep(g_held, eid, actor, Hash(applied), &applied);
    ++g_counts.applied;
    UE_LOGI("drive_payload_sync: row applied eid=%u ('%ls', size %.0f) from the host", eid, row.name.c_str(), row.size);
}

// Parked rows wait for their drive. A joiner's wait starts once its join is over: its drives arrive with the world's
// snapshot, on another lane.
void RetryParked() {
    const auto now = Clock::now();
    const bool joining = coop::join_progress::CurrentPhase() != coop::join_progress::Phase::Idle;
    std::vector<std::pair<uint32_t, Parked>> ready;
    for (auto it = g_parked.begin(); it != g_parked.end();) {
        if (LivePropActor(it->first)) {
            ready.emplace_back(it->first, std::move(it->second));
            it = g_parked.erase(it);
        } else if (joining) {
            it->second.since = now;
            ++it;
        } else if (now - it->second.since >= kPendingTtl) {
            ++g_counts.parkedExpired;
            UE_LOGW("drive_payload_sync: parked row eid=%u expired (its drive never bound here)", it->first);
            it = g_parked.erase(it);
        } else {
            ++it;
        }
    }
    for (auto& [eid, p] : ready) {
        const uint64_t waited = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(now - p.since).count());
        if (waited > g_counts.maxParkedMs) g_counts.maxParkedMs = waited;
        ++g_counts.parkedApplied;
        UE_LOGI("drive_payload_sync: parked row eid=%u applied -- its drive bound after %llu ms", eid,
                static_cast<unsigned long long>(waited));
        ApplyBlob(p.blob, p.senderSlot, /*fromParked*/true);
    }
}

// HOST: a leftover with no retry of its write waiting (given up, its author gone, or its parked row replaced) is put
// back to the drive's previous row once a second, a bounded number of times, while the drive still reads exactly that.
// Past the bound, or with its residue unread, it stays withheld, said once and counted in the minute's stats, until a
// writer replaces it (a read residue only), its author's write lands, or its drive or the session ends.
void RestoreLeftovers(coop::net::Session* s) {
    std::vector<uint32_t> due;
    for (auto it = g_leftover.begin(); it != g_leftover.end();) {
        if (!it->second.ref.Is(it->second.ref.Raw())) {
            it = g_leftover.erase(it);
            continue;
        }
        if (RT::MayRestore(it->second.lo, kWriteRetries) && g_parked.find(it->first) == g_parked.end())
            due.push_back(it->first);
        ++it;
    }
    for (const uint32_t eid : due) {
        void* actor = LivePropActor(eid);
        SD::Row now;
        if (!actor || !FindLeftover(eid, actor) || !DC::ReadDriveRow(actor, now)) continue;
        if (!Withheld(eid, actor, now)) {
            HostSendIfChanged(s, eid, actor);
            continue;
        }
        const Leftover held = std::move(g_leftover[eid].lo);
        g_leftover.erase(eid);
        SD::Row back;
        bool read = false;
        {
            ApplyScope scope;
            coop::desk_snd_fx::ScopedWireApply guard;
            DC::WriteDriveRow(actor, held.pre);
            read = DC::ReadDriveRow(actor, back);
            if (read && back == held.pre) DC::CallDriveUpd(actor);
        }
        auto left = RT::AfterFailedWrite(&held, held.pre, read ? &back : nullptr, held.slot);
        if (!left) {
            UE_LOGI("drive_payload_sync: eid=%u -- the drive a failed write left part-written is back to its previous "
                    "row ('%ls', size %.0f)", eid, held.pre.name.c_str(), held.pre.size);
            HostSendIfChanged(s, eid, actor);
            continue;
        }
        if (!read || ++left->restores == kWriteRetries) {
            KeepLeftover(eid, actor, std::move(*left), "the previous row could not be put back");
            continue;
        }
        LeftoverRec& r = g_leftover[eid];
        r.ref.Set(actor);
        r.lo = std::move(*left);
    }
}

// HOST: a drop intent's drive enrolled; its author's row is the one the host takes for it.
void ResolveBrought(void* actor, uint32_t eid) {
    for (auto it = g_broughtPending.begin(); it != g_broughtPending.end(); ++it) {
        if (it->ref.Get() != actor || !it->ref.Alive()) continue;
        NoteAuthor(eid, actor, it->slot);
        g_broughtPending.erase(it);
        return;
    }
}

void DrainEnrolled(coop::net::Session* s) {
    std::vector<Enrolled> batch;
    {
        std::lock_guard<std::mutex> lk(g_enrolledMu);
        batch.swap(g_enrolled);
    }
    for (const Enrolled& e : batch) {
        if (!e.actor || !R::IsLive(e.actor) || !DC::IsDriveClass(R::ClassOf(e.actor))) continue;
        if (static_cast<uint32_t>(coop::element::Registry::Get().EidForActor(e.actor)) != e.eid) continue;
        if (IsHost(s)) {
            ResolveBrought(e.actor, e.eid);
            // A drive born now reaches every client at its class default; a row that differs goes now, after the
            // birth's PropSpawn left in the same drain.
            uint64_t d = 0;
            if (ClassDefaultHash(e.actor, d)) Keep(g_lastSent, e.eid, e.actor, d, nullptr);
            HostSendIfChanged(s, e.eid, e.actor);
        } else if (TakeNote(e.actor)) {
            SendOwn(s, e.eid, e.actor);
        }
    }
}

// Notes that expired before their drive bound, each said once: that drive's row stays on this peer alone.
void SweepNotes() {
    const auto now = Clock::now();
    for (Noted& n : g_noted) {
        if (!n.ref.Get() || now < n.until) continue;
        UE_LOGW("drive_payload_sync: CLIENT a drive this player brought in never bound in %lld s -- its row stays here",
                static_cast<long long>(std::chrono::duration_cast<std::chrono::seconds>(kNoteTtl).count()));
        n = Noted{};
    }
    for (auto it = g_broughtPending.begin(); it != g_broughtPending.end();)
        it = (now >= it->until || !it->ref.Alive()) ? g_broughtPending.erase(it) : std::next(it);
}

}  // namespace

void Install(coop::net::Session* session) {
    // This lane owns prop_drive's data_0 by eid; the save-record lane must not write it from a second address space.
    coop::prop_save_data::DeclareClassOwnedElsewhere(L"prop_drive_C");
    g_session.store(session, std::memory_order_release);
    if (g_reg != Reg::Pending) return;
    g_reg = sg::WatchClassName(L"prop_drive_C", L"upd", kTagUpd, nullptr, &OnUpdPost) ? Reg::Registered : Reg::Refused;
}

void Tick() {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!g_settled && g_reg != Reg::Pending) {
        sg::ResolvePendingNames();
        if (g_reg == Reg::Registered && sg::ClassNameWatchLive(L"prop_drive_C", L"upd", kTagUpd)) {
            g_settled = true;
            UE_LOGI("drive_payload_sync: the watch on prop_drive_C::upd is live");
        } else if (g_reg == Reg::Refused || sg::ClassNameWatchSettled(L"prop_drive_C", L"upd", kTagUpd)) {
            g_settled = true;
            UE_LOGE("drive_payload_sync: the watch on prop_drive_C::upd is dead -- a drive's row changes cross no peer");
        }
    }
    if (!s || !s->connected()) {
        // Outside a session nothing is held by a client; a joiner's seed carries what differs.
        std::lock_guard<std::mutex> lk(g_enrolledMu);
        g_enrolled.clear();
        return;
    }
    DrainEnrolled(s);
    const auto now = Clock::now();
    if (now >= g_nextSlow) {
        g_nextSlow = now + std::chrono::seconds(1);
        RetryParked();
        if (IsHost(s)) RestoreLeftovers(s);
        SweepNotes();
        SweepKept(g_held);
        SweepKept(g_lastSent);
        SweepKept(g_brought);
        g_asm.Sweep(now, std::chrono::seconds(20));
    }
    if (now >= g_nextStats) {
        g_nextStats = now + std::chrono::seconds(60);
        UE_LOGI("drive_payload_sync: 60s sent=%llu applied=%llu accepted=%llu putBack=%llu ownSent=%llu refused=%llu "
                "parked=%zu withheld=%zu", (unsigned long long)g_counts.sent, (unsigned long long)g_counts.applied,
                (unsigned long long)g_counts.accepted, (unsigned long long)g_counts.putBack,
                (unsigned long long)g_counts.ownSent, (unsigned long long)g_counts.refused, g_parked.size(),
                g_leftover.size());
    }
}

void OnDrivePayloadChunk(const coop::net::BlobChunkPayload& p, uint8_t senderSlot) {
    std::vector<uint8_t> blob;
    if (!g_asm.OnChunk(p, senderSlot, blob)) return;
    if (!DC::EnsureResolved()) return;
    ApplyBlob(blob, senderSlot, /*fromParked*/false);
}

void QueueConnectBroadcastForSlot(int peerSlot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!IsHost(s) || !DC::EnsureResolved()) return;
    std::vector<coop::element::Registry::ActorIdPair> pairs;
    coop::element::Registry::Get().SnapshotActorsByType(coop::element::ElementType::Prop, pairs);
    int sent = 0, drives = 0;
    for (const auto& p : pairs) {
        if (!p.actor || !R::IsLiveByIndex(p.actor, p.internalIdx) || !DC::IsDriveClass(R::ClassOf(p.actor))) continue;
        ++drives;
        SD::Row row;
        if (!DC::ReadDriveRow(p.actor, row) || Withheld(static_cast<uint32_t>(p.id), p.actor, row)) continue;
        const std::vector<uint8_t> bytes = Bytes(row);
        if (IsClassDefault(p.actor, coop::blob_chunks::Fnv64(bytes))) continue;
        if (SendBytes(s, static_cast<uint32_t>(p.id), bytes, peerSlot)) ++sent;
    }
    UE_LOGI("drive_payload_sync: seed -> joiner slot %d (%d of %d drives differ from their class default)", peerSlot,
            sent, drives);
}

void NoteOwnDrive(void* actor) {
    if (!actor) return;
    const auto now = Clock::now();
    for (Noted& n : g_noted) {
        if (!n.ref.Get()) {
            n.ref.Set(actor);
            n.until = now + kNoteTtl;
            return;
        }
    }
    Noted n;
    n.ref.Set(actor);
    n.until = now + kNoteTtl;
    g_noted.push_back(n);
}

void NoteClientBrought(void* actor, uint8_t senderSlot) {
    if (!actor || senderSlot == 0 || senderSlot >= coop::net::kMaxPeers || !DC::IsDriveClass(R::ClassOf(actor))) return;
    Brought b;
    b.ref.Set(actor);
    b.slot = senderSlot;
    b.until = Clock::now() + kNoteTtl;
    g_broughtPending.push_back(b);
}

void OnEnrolled(void* actor, uint32_t eid) {
    if (!actor || eid == 0 || !DC::IsDriveClass(R::ClassOf(actor))) return;
    std::lock_guard<std::mutex> lk(g_enrolledMu);
    g_enrolled.push_back(Enrolled{actor, eid});
}

void OnBound(uint32_t eid, void* actor, int senderSlot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !actor || !DC::IsDriveClass(R::ClassOf(actor))) return;
    if (IsHost(s)) {
        // A client's drive mirrored here: every peer starts it at the class default, and its author's row follows.
        uint64_t d = 0;
        if (ClassDefaultHash(actor, d)) Keep(g_lastSent, eid, actor, d, nullptr);
        if (senderSlot > 0 && senderSlot < coop::net::kMaxPeers)
            NoteAuthor(eid, actor, static_cast<uint8_t>(senderSlot));
        return;
    }
    if (TakeNote(actor)) SendOwn(s, eid, actor);
}

void OnPeerLeft(uint8_t slot) {
    if (slot >= coop::net::kMaxPeers) return;
    g_asm.ClearSlot(slot);
    // The retry counter is the departed author's own write attempt: dropping it with the row
    // keeps a later row for the same eid from inheriting its count.
    for (auto it = g_parked.begin(); it != g_parked.end();) {
        if (it->second.senderSlot == slot) { g_writeFails.erase(it->first); it = g_parked.erase(it); }
        else ++it;
    }
    for (auto it = g_brought.begin(); it != g_brought.end();) {
        if (it->second.slot == slot) { g_writeFails.erase(it->first); it = g_brought.erase(it); }
        else ++it;
    }
    // A leftover of its write is no longer any client's to write over, a later occupant of the slot least of all: its
    // previous row is put back instead.
    for (auto& kv : g_leftover)
        if (kv.second.lo.slot == slot) kv.second.lo.slot = 0xFF;
    for (auto it = g_broughtPending.begin(); it != g_broughtPending.end();)
        it = it->slot == slot ? g_broughtPending.erase(it) : std::next(it);
    g_nextRefusalSay[slot] = {};
}

void OnDisconnect() {
    g_lastSent.clear();
    g_held.clear();
    g_parked.clear();
    g_writeFails.clear();
    if (!g_leftover.empty())
        UE_LOGW("drive_payload_sync: the session ended with %zu drive(s) holding what a failed write left -- not put "
                "back", g_leftover.size());
    g_leftover.clear();
    g_noted.clear();
    g_brought.clear();
    g_broughtPending.clear();
    {
        std::lock_guard<std::mutex> lk(g_enrolledMu);
        g_enrolled.clear();
    }
    g_asm.Clear();
    g_applying = false;
    g_counts = Counts{};
    for (auto& t : g_nextRefusalSay) t = {};
}

Counts LaneCounts() { return g_counts; }

bool DevClaimRow(void* actor) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || IsHost(s) || !actor) return false;
    const coop::element::ElementId eid = coop::element::Registry::Get().EidForActor(actor);
    SD::Row row;
    if (eid == coop::element::kInvalidId || !DC::ReadDriveRow(actor, row)) return false;
    return SendBytes(s, static_cast<uint32_t>(eid), Bytes(row), 0);
}

}  // namespace coop::drive_payload_sync
