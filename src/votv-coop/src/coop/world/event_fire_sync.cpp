// coop/world/event_fire_sync.cpp -- see coop/world/event_fire_sync.h. The bytecode facts this
// module stands on: the save slot's settime iterates allEvents, skips rows in passEvents, and
// on a clock-cross fire calls the eventer's runEvent for the row and appends it to passEvents,
// so a runEvent whose caller is settime is exactly a scheduler fire, and an empty allEvents
// kills the walk (the client suppression seam); runEvent is the only function of that name,
// called from settime, from the game's own event menu and through our reflected dispatch; the
// gamemode's boot marks the rows before a new game's start day passed without firing them, and
// rebuilds allEvents from the events table on every world load, so the zeroed count self-heals
// and a client-written save cannot be poisoned; and the only special the table uses is the
// prank roll, which summonArirPrank resolves through an internal runSpecialEvent call -- so the
// watch sits on BOTH eventer verbs and the wire carries the name each call itself took (the
// rolled case, not the prank verb).

#include "coop/world/event_fire_sync.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/world/time_sync.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/world/world_singleton.h"
#include "ue_wrap/world/daynightcycle.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <string>
#include <unordered_set>

namespace coop::event_fire_sync {
namespace {

namespace R  = ue_wrap::reflection;
namespace GT = ue_wrap::game_thread;
namespace sg = ue_wrap::script_gate;

std::atomic<coop::net::Session*> g_session{nullptr};

// Resolution, on demand, game thread: the three classes come from the object index, which answers at
// once whether or not they are loaded; their members resolve once all three are, on the first pass or
// never (a renamed symbol on a future game version), so a failed pass latches loudly.
int32_t g_offSaveSlot = -1;           // mainGamemode.saveSlot (UsaveSlot_C*)
int32_t g_offEventer = -1;            // mainGamemode.eventer  (Atrigger_eventer_C*)
int32_t g_offPassEvents = -1;         // saveSlot.passEvents (TArray<FName>)
int32_t g_offAllEvents = -1;          // saveSlot.allEvents  (TArray<FName>)
void* g_runEventFn = nullptr;         // runEvent(FName event, FName special)
void* g_runSpecialEventFn = nullptr;  // runSpecialEvent(FName eventName1) -> bool
void* g_summonArirPrankFn = nullptr;  // summonArirPrank(), runSpecialEvent's prank-roll caller
void* g_settimeFn = nullptr;          // saveSlot.settime, the scheduler's walk
int32_t g_offEventParam = -1;         // runEvent's `event` in its parameter frame
int32_t g_offSpecialParam = -1;       // runSpecialEvent's `eventName1` in its parameter frame
bool g_resolved = false;
bool g_resolveFailed = false;

// The host's watches on both eventer verbs, registered once per process. One emit per
// committed call: the watches are the ONLY broadcast source (a dev HostFire's reflected call
// reaches them like any other), and the sequence number makes repeats of a replayable name
// (a special is repeatable) tellable apart -- the dedupe key is the occurrence, never the name.
constexpr int kTagEventFire = 0x45564652;  // 'EVFR'
constexpr const wchar_t* kRunEvent = L"runEvent";  // one pointer: the gate matches a name watch by it
constexpr const wchar_t* kRunSpecialEvent = L"runSpecialEvent";
bool g_watchInstalled = false;
bool g_watchLive = false;
bool g_specialWatchInstalled = false;
bool g_specialWatchLive = false;
unsigned g_hostFireSeq = 0;  // committed host fires across both verbs (game thread)

// Client suppression and replay state, game thread.
int g_zeroedAllEventsNum = 0;         // what we zeroed (restore on disconnect); 0 = nothing zeroed
void* g_zeroedSaveSlot = nullptr;
int32_t g_zeroedSaveSlotIdx = -1;
struct PendingFire {
    FireKind kind;
    std::string name;
    // The active override: the host registry says this row is in flight, so bypass the
    // passEvents dedupe (a mid-event joiner's blob carries the row as completed history while
    // the event is still running).
    bool activeOverride = false;
    bool attempted = false;    // one replay try ran (paces the next)
    int failedDispatches = 0;  // the eventer was up but refused the native call
    bool failLogged = false;   // the once-per-row retry line
    std::chrono::steady_clock::time_point lastAttempt{};
};
std::deque<PendingFire> g_pending;    // replays waiting for the eventer (join window)
// EventFire is pre-world-sendable, so a joiner can queue fires for its whole load window. The
// cap is the table plus specials plus margin; duplicates are skipped at queue time, so it is
// effectively unreachable.
constexpr size_t kMaxPending = 128;
// A refused dispatch retries on a real-time cadence (the per-tick install pump drives the
// drain, so a stalled row never waits for the next fire) and is bounded, so a permanently
// refused row cannot wedge the FIFO behind it.
constexpr auto kReplayRetrySpacing = std::chrono::seconds(1);
constexpr int kMaxReplayDispatches = 10;
std::unordered_set<std::string> g_replayed;  // rows replayed this session (dedupe)
std::atomic<unsigned> g_replays{0};          // fires replayed natively (ReplayCount)

// The UE4 array header; 8-byte FName elements for the two arrays touched.
struct RawArray {
    R::FName* Data;
    int32_t Num;
    int32_t Max;
};

bool MembersMissing() {
    return g_offSaveSlot < 0 || g_offEventer < 0 || g_offPassEvents < 0 || g_offAllEvents < 0 ||
           !g_runEventFn || !g_runSpecialEventFn || !g_settimeFn || g_offEventParam < 0 ||
           g_offSpecialParam < 0;
}

bool ResolvePass() {
    if (g_resolved) return true;
    if (g_resolveFailed) return false;
    namespace OI = ue_wrap::object_index;
    void* gmCls = OI::ClassByName(L"mainGamemode_C");
    void* ssCls = OI::ClassByName(L"saveSlot_C");
    void* evCls = OI::ClassByName(L"trigger_eventer_C");
    if (!gmCls || !ssCls || !evCls) return false;  // the world has not loaded them yet
    g_offSaveSlot = R::FindPropertyOffset(gmCls, L"saveSlot");
    g_offEventer = R::FindPropertyOffset(gmCls, L"eventer");
    g_offPassEvents = R::FindPropertyOffset(ssCls, L"passEvents");
    if (g_offAllEvents < 0) g_offAllEvents = R::FindPropertyOffset(ssCls, L"allEvents");
    g_runEventFn = R::FindFunction(evCls, L"runEvent");
    g_runSpecialEventFn = R::FindFunction(evCls, L"runSpecialEvent");
    // The prank roll's frame is an origin label, not a requirement: its absence costs only
    // the "prank-roll" tag in the fire log, so it stays out of the latching check.
    g_summonArirPrankFn = R::FindFunction(evCls, L"summonArirPrank");
    g_settimeFn = R::FindFunction(ssCls, L"settime");
    g_offEventParam = g_runEventFn ? R::FindParamOffset(g_runEventFn, L"event") : -1;
    g_offSpecialParam = g_runSpecialEventFn ? R::FindParamOffset(g_runSpecialEventFn, L"eventName1") : -1;
    if (MembersMissing()) {
        g_resolveFailed = true;
        UE_LOGW("event_fire: resolution INCOMPLETE on loaded classes (saveSlot=0x%X eventer=0x%X "
                "passEvents=0x%X allEvents=0x%X runEvent=%s(event=0x%X) "
                "runSpecialEvent=%s(eventName1=0x%X) settime=%s) -- "
                "latched OFF; game version mismatch?",
                g_offSaveSlot, g_offEventer, g_offPassEvents, g_offAllEvents, g_runEventFn ? "yes" : "NO",
                g_offEventParam, g_runSpecialEventFn ? "yes" : "NO", g_offSpecialParam,
                g_settimeFn ? "yes" : "NO");
        return false;
    }
    g_resolved = true;
    UE_LOGI("event_fire: resolved (saveSlot=0x%X passEvents=0x%X allEvents=0x%X eventer=0x%X "
            "runEvent=yes runSpecialEvent=yes summonArirPrank=%s settime=yes)",
            g_offSaveSlot, g_offPassEvents, g_offAllEvents, g_offEventer,
            g_summonArirPrankFn ? "yes" : "NO");
    return true;
}

void* SaveSlotOf(void* gm) {
    if (!gm || g_offSaveSlot < 0) return nullptr;
    void* ss = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(gm) + g_offSaveSlot);
    return (ss && R::IsLive(ss)) ? ss : nullptr;
}

void* EventerOf(void* gm) {
    if (!gm || g_offEventer < 0) return nullptr;
    void* ev = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(gm) + g_offEventer);
    return (ev && R::IsLive(ev)) ? ev : nullptr;
}

RawArray* ArrayAt(void* obj, int32_t off) {
    if (!obj || off < 0) return nullptr;
    return reinterpret_cast<RawArray*>(reinterpret_cast<uint8_t*>(obj) + off);
}

std::string NarrowName(const R::FName& n) {
    const std::wstring w = R::ToString(n);
    std::string s;
    s.reserve(w.size());
    for (wchar_t c : w) s.push_back((c > 0 && c < 128) ? static_cast<char>(c) : '?');
    return s;
}

// The replay bypass, thread-local and exact: spawn_authority refuses the eventer's verbs on a
// client, and only the call this scope marks -- that eventer object, that function -- may pass.
// The gate's fromOurCode flag is the wider flag it replaces: every synchronous coop dispatch
// inherits it, so any other lane reaching a verb through a reflected call would walk in under it.
thread_local void* t_replayObject = nullptr;
thread_local void* t_replayFunction = nullptr;
// The scoped call's commit proof: the verb's name-watch POST runs only after the body really
// ran -- a script-gate Cancel skips every POST -- and stamps this only for the exact pair.
thread_local bool t_replayCommitted = false;

struct ReplayScope {
    void* prevObj;
    void* prevFn;
    ReplayScope(void* o, void* f) : prevObj(t_replayObject), prevFn(t_replayFunction) {
        t_replayObject = o;
        t_replayFunction = f;
    }
    ~ReplayScope() { t_replayObject = prevObj; t_replayFunction = prevFn; }
};

// One dispatch attempt's result. Call answers "no dispatch fault", which a gate Cancel ALSO
// satisfies, so Committed -- the verb's own POST observed inside the scope -- is the only
// success. Cancelled is a refusal: it counts on the row's retry bound exactly like a Faulted.
enum class FireOutcome { Faulted, Cancelled, Committed };

FireOutcome FireOnce(void* eventer, ue_wrap::ParamFrame& f, bool observable) {
    t_replayCommitted = false;
    bool dispatched;
    {
        const ReplayScope replay(eventer, f.function());
        dispatched = ue_wrap::Call(eventer, f);
    }
    if (!dispatched) return FireOutcome::Faulted;
    return (!observable || t_replayCommitted) ? FireOutcome::Committed : FireOutcome::Cancelled;
}

// The native fire, game thread. True iff the verb's body committed (FireOnce): callers gate
// the broadcast and the replayed set on it, since a refused host fire must not make clients
// replay an event the authority never executed, and a refused replay must not permanently
// consume the row. quietFail silences the per-call refusal warn for a queue's spaced
// retries -- the first refusal is loud and the row's retry line is said once, in NoteAttempt.
bool NativeFire(FireKind kind, const std::wstring& eventName, const std::wstring& specialName,
                bool quietFail = false) {
    void* eventer = EventerOf(ue_wrap::world_singleton::Gamemode());
    if (!eventer) {
        UE_LOGW("event_fire: no live trigger_eventer -- native fire dropped ('%ls')", eventName.c_str());
        return false;
    }
    const char* verb = kind == FireKind::SpecialEvent ? "runSpecialEvent" : "runEvent";
    void* fn = kind == FireKind::SpecialEvent ? g_runSpecialEventFn : g_runEventFn;
    if (!fn) { UE_LOGW("event_fire: %s unresolved", verb); return false; }
    ue_wrap::ParamFrame f(fn);
    if (!f.valid()) return false;
    if (kind == FireKind::SpecialEvent) {
        f.Set<R::FName>(L"eventName1", ue_wrap::fname_utils::StringToFName(eventName));
    } else {
        f.Set<R::FName>(L"event", ue_wrap::fname_utils::StringToFName(eventName));
        f.Set<R::FName>(L"special", ue_wrap::fname_utils::StringToFName(specialName));
    }
    // The commit check needs an observer: the verb's watch live AND the gate held (a solo run
    // or a join-window replay before the hold observes nothing, and takes the dispatch's word).
    const bool observable = sg::IsEnabled() &&
        (kind == FireKind::SpecialEvent ? g_specialWatchLive : g_watchLive);
    const FireOutcome out = FireOnce(eventer, f, observable);
    if (out == FireOutcome::Committed) {
        if (kind == FireKind::SpecialEvent)
            UE_LOGI("event_fire: runSpecialEvent('%ls') dispatched", eventName.c_str());
        else
            UE_LOGI("event_fire: runEvent('%ls', special='%ls') dispatched",
                    eventName.c_str(), specialName.c_str());
        return true;
    }
    if (!quietFail) {
        if (out == FireOutcome::Cancelled)
            UE_LOGW("event_fire: %s('%ls') dispatch returned but the body was CANCELLED at the "
                    "script gate -- counted as refused", verb, eventName.c_str());
        else
            UE_LOGW("event_fire: %s('%ls') dispatch FAILED", verb, eventName.c_str());
    }
    return false;
}

void Broadcast(FireKind kind, const std::string& name) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || s->role() != coop::net::Role::Host) return;
    coop::net::EventFirePayload p{};  // zero-init -> name[] pre-NUL-bound
    p.dispatch = static_cast<uint8_t>(kind);
    const size_t n = name.size() < sizeof(p.name) - 1 ? name.size() : sizeof(p.name) - 1;
    std::memcpy(p.name, name.c_str(), n);
    s->SendReliable(coop::net::ReliableKind::EventFire, &p, sizeof(p));
    UE_LOGI("event_fire: broadcast %s '%s'",
            kind == FireKind::SpecialEvent ? "runSpecialEvent" : "runEvent", name.c_str());
}

// The one emit point for host fires, at the body commit: runEvent entered from settime is a
// scheduler fire, a call with no Blueprint caller and fromOurCode is our own HostFire dispatch,
// and any other caller is the game's own menus -- every one reached clients before only if the
// host broadcast it, which is now this watch's job alone. POST so a cancelled body can never
// announce a fire that did not run; a client replay's own call early-outs on the role gate.
void OnRunEventPost(const sg::Call& call) {
    // Reached only when the scoped call's body really ran: the replay's commit proof.
    if (call.object == t_replayObject && call.function == t_replayFunction) t_replayCommitted = true;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || s->role() != coop::net::Role::Host) return;
    if (!ResolvePass() || call.function != g_runEventFn) return;
    const std::string name = NarrowName(*reinterpret_cast<const R::FName*>(call.locals + g_offEventParam));
    const char* origin = call.callerFunction == g_settimeFn ? "scheduler" :
                         call.fromOurCode ? "dev-call" : "native";
    ++g_hostFireSeq;
    UE_LOGI("event_fire: host fire #%u runEvent('%s') origin=%s -- broadcasting",
            g_hostFireSeq, name.c_str(), origin);
    Broadcast(FireKind::RunEvent, name);
}

// HOST: a runSpecialEvent body just completed. The natural entry is a scheduled ariralPrank
// row: runEvent -> summonArirPrank (a rep-tier Array_Random pick, removed from the pool on the
// way out) -> runSpecialEvent, so THIS call is where the rolled outcome becomes known -- the
// row's own broadcast carries only the arirInteraction name. Dev fires (reflected, no caller)
// and the game's cheat menu reach it the same way and emit through here exactly once.
void OnRunSpecialEventPost(const sg::Call& call) {
    // Reached only when the scoped call's body really ran: the replay's commit proof.
    if (call.object == t_replayObject && call.function == t_replayFunction) t_replayCommitted = true;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || s->role() != coop::net::Role::Host) return;
    if (!ResolvePass() || call.function != g_runSpecialEventFn) return;
    const std::string name =
        NarrowName(*reinterpret_cast<const R::FName*>(call.locals + g_offSpecialParam));
    const char* origin =
        (g_summonArirPrankFn && call.callerFunction == g_summonArirPrankFn) ? "prank-roll" :
        call.fromOurCode ? "dev-call" : "native";
    const bool returned = call.result ? *static_cast<const bool*>(call.result) : true;
    ++g_hostFireSeq;
    UE_LOGI("event_fire: host fire #%u runSpecialEvent('%s') origin=%s returned=%d -- broadcasting",
            g_hostFireSeq, name.c_str(), origin, returned ? 1 : 0);
    Broadcast(FireKind::SpecialEvent, name);
}

// True iff the client's own passEvents already contains the row (the transferred save
// carried this fire; its world effects are already in the loaded state).
bool InClientPassEvents(const std::string& name) {
    RawArray* pass = ArrayAt(SaveSlotOf(ue_wrap::world_singleton::Gamemode()), g_offPassEvents);
    if (!pass || !pass->Data || pass->Num <= 0 || pass->Num > 100000) return false;
    std::wstring w(name.begin(), name.end());
    const R::FName want = ue_wrap::fname_utils::StringToFName(w);
    for (int32_t i = 0; i < pass->Num; ++i) {
        const R::FName& e = pass->Data[i];
        if (e.ComparisonIndex == want.ComparisonIndex && e.Number == want.Number) return true;
    }
    return false;
}

// The replay step's result, game thread: Done pops the row (dispatched or deliberately
// skipped), NotReady waits it uncounted (the eventer is simply not up yet), Failed counts a
// real dispatch refusal against the row's retry bound.
enum class ReplayStep { Done, NotReady, Failed };

// The client replay executor, game thread.
ReplayStep TryReplay(const PendingFire& pf) {
    if (!ResolvePass() || !EventerOf(ue_wrap::world_singleton::Gamemode())) return ReplayStep::NotReady;
    // Dedupe applies to one-shot scheduled rows only (the game's own passEvents semantics);
    // specials (graffiti, pranks the menu re-fires) are repeatable by design.
    if (pf.kind == FireKind::RunEvent) {
        if (g_replayed.count(pf.name)) {
            UE_LOGI("event_fire: '%s' already replayed this session -- skipping", pf.name.c_str());
            return ReplayStep::Done;
        }
        // A passEvents hit marks nothing: the skip must stay re-decidable, since marking here would
        // let a history-skipped fire permanently block a later in-flight override for the same row
        // (a fire landing between the joiner's connect and its blob capture rides both the wire and
        // the blob). A duplicate just rescans the array; passEvents never shrinks mid-session.
        if (!pf.activeOverride && InClientPassEvents(pf.name)) {
            UE_LOGI("event_fire: '%s' already in local passEvents (save carried it) -- skipping",
                    pf.name.c_str());
            return ReplayStep::Done;
        }
    }
    const std::wstring w(pf.name.begin(), pf.name.end());
    UE_LOGI("event_fire: client REPLAY %s '%s'%s",
            pf.kind == FireKind::SpecialEvent ? "runSpecialEvent" : "runEvent", pf.name.c_str(),
            pf.activeOverride ? " (in-flight active-override)" : "");
    // The special is always None: the only native special is the prank roll, whose chosen case
    // the host's runSpecialEvent watch sends on its own. A refused dispatch is NOT a consume:
    // the row stays queued and retries on a bound (NoteAttempt); the repeated refusals stay
    // quiet so the once-per-row line stands out.
    if (NativeFire(pf.kind, w, L"None", /*quietFail=*/pf.failedDispatches > 0)) {
        g_replays.fetch_add(1, std::memory_order_relaxed);
        if (pf.kind == FireKind::RunEvent) g_replayed.insert(pf.name);
        return ReplayStep::Done;
    }
    return ReplayStep::Failed;
}

// One attempt's outcome on a queued row: stamp the pacing, and on a real refusal count it
// against the bound and say so once -- a NotReady row just waits for its eventer.
void NoteAttempt(PendingFire& pf, ReplayStep step) {
    pf.attempted = true;
    pf.lastAttempt = std::chrono::steady_clock::now();
    if (step != ReplayStep::Failed) return;
    ++pf.failedDispatches;
    if (pf.failLogged) return;
    pf.failLogged = true;
    UE_LOGW("event_fire: replay of '%s' refused by the native dispatch -- stays queued, "
            "retrying every %ds (dropped after %d refusals)",
            pf.name.c_str(), static_cast<int>(kReplayRetrySpacing.count()), kMaxReplayDispatches);
}

// CLIENT: replay the queued fires in arrival order, stopping at the first the eventer cannot take
// yet; a front row that already tried waits out its spacing first. Run before a new fire is
// handled, at the client's world-ready announce, and every install tick (the lasting retry).
void DrainPending() {
    const auto now = std::chrono::steady_clock::now();
    while (!g_pending.empty()) {
        PendingFire& front = g_pending.front();
        if (front.attempted && now - front.lastAttempt < kReplayRetrySpacing) return;
        const ReplayStep step = TryReplay(front);
        if (step == ReplayStep::Done) {
            g_pending.pop_front();
            continue;
        }
        NoteAttempt(front, step);
        if (front.failedDispatches >= kMaxReplayDispatches) {
            UE_LOGE("event_fire: replay of '%s' DROPPED after %d refused dispatches -- "
                    "the row's outputs are lost this session", front.name.c_str(),
                    front.failedDispatches);
            g_pending.pop_front();
        }
        return;
    }
}

// CLIENT: the cycle is about to tick, and its settime walks the save's list of events on any clock
// change: hold the list empty before the tick's body runs, so no row is ever due on a client -- the
// first tick of a new world included, where the clock lane's pre-observer writes the host's newest
// sample into the same tick. Held on exactly the cycles the clock lane parks, by its own predicate.
bool g_tickObserved = false;  // the pre-observer is registered (once per process)
bool g_holdUnresolvable = false;  // the save slot's class has no allEvents; said once

void OnCycleTickPre(void* self, void* /*function*/, void* /*params*/) {
    if (!GT::IsGameThread() || !coop::time_sync::HoldsCycle(self)) return;
    void* ss = ue_wrap::daynightcycle::SaveSlotOfCycle(self);
    if (!ss) return;
    // The list's offset from the live slot's own class, a property lookup with no object-array walk,
    // so the first tick of a world is held even before the other members have resolved.
    if (g_offAllEvents < 0) {
        if (g_holdUnresolvable) return;
        g_offAllEvents = R::FindPropertyOffset(R::ClassOf(ss), L"allEvents");
        if (g_offAllEvents < 0) {
            g_holdUnresolvable = true;
            UE_LOGW("event_fire: the save slot has no allEvents -- a client's event walk cannot be held");
            return;
        }
    }
    RawArray* all = ArrayAt(ss, g_offAllEvents);
    if (!all || all->Num <= 0 || all->Num > 100000) return;  // 0 = already suppressed
    // A legal array state (empty with slack): data and capacity untouched, and the engine frees
    // the same allocation later. The gamemode's boot rebuilds allEvents from the table on every
    // world load, so this re-asserts after any reload and can never poison a save.
    g_zeroedAllEventsNum = all->Num;
    g_zeroedSaveSlot = ss;
    g_zeroedSaveSlotIdx = R::InternalIndexOf(ss);
    all->Num = 0;
    UE_LOGI("event_fire: client scheduler SUPPRESSED at the cycle's tick (allEvents %d -> 0; host is the only "
            "firer; restored on disconnect)", g_zeroedAllEventsNum);
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    // Called every pump tick by the install fanout, which is also the retry until the cycle class
    // loads and until the gate has resolved the watch's name.
    if (!g_watchInstalled)
        g_watchInstalled = sg::WatchName(kRunEvent, kTagEventFire, nullptr, &OnRunEventPost);
    if (!g_specialWatchInstalled)
        g_specialWatchInstalled =
            sg::WatchName(kRunSpecialEvent, kTagEventFire, nullptr, &OnRunSpecialEventPost);
    if ((g_watchInstalled && !g_watchLive) || (g_specialWatchInstalled && !g_specialWatchLive))
        sg::ResolvePendingNames();
    if (!g_watchLive && sg::NameWatchLive(kRunEvent, kTagEventFire)) {
        g_watchLive = true;
        UE_LOGI("event_fire: the host's fires are seen at runEvent (a script-gate watch)");
    }
    if (!g_specialWatchLive && sg::NameWatchLive(kRunSpecialEvent, kTagEventFire)) {
        g_specialWatchLive = true;
        UE_LOGI("event_fire: the host's special picks are seen at runSpecialEvent (a script-gate watch)");
    }
    // The client replay queue's lasting retry: a fire whose eventer was not up, or whose dispatch
    // was refused, no longer waits for the next fire to arrive -- this pump paces it instead.
    if (!g_pending.empty()) DrainPending();
    namespace DNC = ue_wrap::daynightcycle;
    if (g_tickObserved || !DNC::EnsureResolved()) return;
    void* fn = DNC::TickFunction();
    if (!fn) {
        UE_LOGW("event_fire: daynightCycle_C::ReceiveTick not found -- a client's event walk cannot be held");
        g_tickObserved = true;
        return;
    }
    if (!GT::RegisterPreObserver(fn, &OnCycleTickPre)) {
        static bool s_said = false;  // retried every pump tick; said once
        if (!s_said) UE_LOGW("event_fire: the cycle tick's pre-observer did not register (table full?) -- retrying");
        s_said = true;
        return;
    }
    g_tickObserved = true;
    UE_LOGI("event_fire: a client's event walk is held at the cycle's own tick (pre-observer on ReceiveTick)");
}

bool HostFire(FireKind kind, const std::wstring& eventName, const std::wstring& specialName) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (s && s->connected() && s->role() != coop::net::Role::Host) {
        UE_LOGW("event_fire: HostFire refused -- connected as a client (host is authoritative)");
        return false;
    }
    const std::wstring ev = eventName;
    const std::wstring sp = specialName.empty() ? L"None" : specialName;
    GT::Post([kind, ev, sp] {
        // Resolve here, not before the post: a solo host's dev menu has no session. The native fire
        // warns loudly if the world or the eventer is not up.
        if (!ResolvePass()) {
            UE_LOGW("event_fire: HostFire('%ls') -- the event classes are not loaded", ev.c_str());
            return;
        }
        // No send of its own: the runEvent/runSpecialEvent watches see this reflected call like
        // any other host fire and emit it once (a dev fire is the 'dev-call' origin). A refused
        // dispatch commits nothing, so nothing reaches the wire.
        NativeFire(kind, ev, sp);
    });
    return true;
}

void OnReliable(const coop::net::EventFirePayload& payload) {
    if (!GT::IsGameThread()) { UE_LOGW("event_fire: OnReliable off-game-thread -- dropping"); return; }
    // NUL-bound the name (the payload crosses the trust boundary; the dispatcher length-checked
    // it).
    char buf[sizeof(payload.name) + 1] = {};
    std::memcpy(buf, payload.name, sizeof(payload.name));
    const std::string name(buf);
    if (name.empty() || payload.dispatch > static_cast<uint8_t>(FireKind::SpecialEvent)) {
        UE_LOGW("event_fire: OnReliable malformed (dispatch=%u, name='%s') -- dropping",
                payload.dispatch, name.c_str());
        return;
    }
    const FireKind kind = static_cast<FireKind>(payload.dispatch);
    const char* lane = nullptr;
    const int verdict = ReplayVerdict(name, &lane);
    if (verdict == 0) {
        UE_LOGI("event_fire: '%s' NOT replayed -- %s owns the outputs", name.c_str(), lane);
        return;
    }
    if (verdict < 0) {
        UE_LOGW("event_fire: '%s' not in the replay policy -- default NO-replay (newer host? "
                "add the row's verdict)", name.c_str());
        return;
    }
    DrainPending();
    PendingFire pf{ kind, name };
    ReplayStep step = ReplayStep::NotReady;  // a non-empty queue tries it in FIFO order later
    if (g_pending.empty()) {
        step = TryReplay(pf);
        if (step == ReplayStep::Done) return;
        NoteAttempt(pf, step);  // the queue must remember the try: it paces the retry
    }
    // One-shot rows dedupe at queue time too (a scheduler re-fire of a dev-fired row during the
    // same load window would otherwise queue twice; the replay would catch it later, but a
    // duplicate-free queue keeps the cap honest).
    if (kind == FireKind::RunEvent) {
        for (const auto& q : g_pending)
            if (q.kind == kind && q.name == name) return;
    }
    if (g_pending.size() >= kMaxPending) {
        UE_LOGW("event_fire: pending replay queue full (%zu) -- dropping '%s'",
                g_pending.size(), name.c_str());
        return;
    }
    UE_LOGI("event_fire: queued '%s' (%zu pending; %s)", name.c_str(), g_pending.size() + 1,
            step == ReplayStep::Failed ? "first dispatch refused" : "eventer not up yet");
    g_pending.push_back(std::move(pf));
}

void ReplayInFlightRow(const std::string& rowName) {
    if (!GT::IsGameThread()) { UE_LOGW("event_fire: ReplayInFlightRow off-game-thread -- dropping"); return; }
    if (rowName.empty()) return;
    const char* lane = nullptr;
    const int verdict = ReplayVerdict(rowName, &lane);
    if (verdict == 0) {
        UE_LOGI("event_fire: in-flight '%s' NOT replayed -- %s owns the outputs (its join "
                "snapshot delivers current state)", rowName.c_str(), lane);
        return;
    }
    if (verdict < 0) {
        UE_LOGW("event_fire: in-flight '%s' not in the replay policy -- default NO-replay "
                "(add the row's verdict)", rowName.c_str());
        return;
    }
    DrainPending();
    PendingFire pf{ FireKind::RunEvent, rowName, /*activeOverride=*/true };
    if (g_pending.empty()) {
        const ReplayStep step = TryReplay(pf);
        if (step == ReplayStep::Done) return;
        NoteAttempt(pf, step);  // the queue must remember the try: it paces the retry
    }
    // The eventer is not up yet. If the row is already queued (a fire copy landed in the pre-world
    // window), upgrade it in place: two entries would double-dispatch, and the plain copy alone
    // could history-skip the in-flight replay.
    for (auto& q : g_pending) {
        if (q.kind == FireKind::RunEvent && q.name == rowName) {
            q.activeOverride = true;
            UE_LOGI("event_fire: in-flight '%s' already queued -- upgraded to active-override",
                    rowName.c_str());
            return;
        }
    }
    if (g_pending.size() >= kMaxPending) {
        UE_LOGW("event_fire: pending replay queue full (%zu) -- dropping in-flight '%s'",
                g_pending.size(), rowName.c_str());
        return;
    }
    UE_LOGI("event_fire: queued in-flight '%s' (%zu pending)",
            rowName.c_str(), g_pending.size() + 1);
    g_pending.push_back(std::move(pf));
}

void OnClientWorldReady() {
    if (g_pending.empty()) return;
    const size_t before = g_pending.size();
    DrainPending();
    if (g_pending.empty())
        UE_LOGI("event_fire: world ready -- replayed the %zu queued fire(s)", before);
    else
        UE_LOGW("event_fire: world ready with %zu of %zu queued fire(s) still pending -- "
                "the install pump retries them", g_pending.size(), before);
}

void OnDisconnect() {
    // Restore the client's scheduler only if we zeroed this exact live save slot and nothing
    // repopulated it since (a boot rebuild leaves the count positive, and then the restore must
    // not run).
    if (g_zeroedAllEventsNum > 0 && g_zeroedSaveSlot &&
        R::IsLiveByIndex(g_zeroedSaveSlot, g_zeroedSaveSlotIdx)) {
        RawArray* all = ArrayAt(g_zeroedSaveSlot, g_offAllEvents);
        if (all && all->Num == 0 && all->Max >= g_zeroedAllEventsNum) {
            all->Num = g_zeroedAllEventsNum;
            UE_LOGI("event_fire: allEvents restored (0 -> %d) -- local scheduler resumes",
                    g_zeroedAllEventsNum);
        }
    }
    g_zeroedAllEventsNum = 0;
    g_zeroedSaveSlot = nullptr;
    g_zeroedSaveSlotIdx = -1;
    g_pending.clear();
    g_replayed.clear();
    g_session.store(nullptr, std::memory_order_release);
}

bool InReplayScope(void* object, void* function) {
    return t_replayObject && t_replayObject == object && t_replayFunction == function;
}

unsigned ReplayCount() { return g_replays.load(std::memory_order_relaxed); }

}  // namespace coop::event_fire_sync
