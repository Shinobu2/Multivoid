// coop/world/spawn_authority.cpp -- see the header. The rows rest on these bytecode facts: the
// mushroom master arms one looping spawn timer at begin-play, and its spawn mints spawner children
// with a lifespan, so refused children are reaped by the engine independently of the refused event;
// the mushroom spawner's own looping timer materialises the food cap and self-destroys, and with
// spawn refused the lifespan reaps it; the yellow-wisp ticker spawns at a navmesh random-walk point,
// not around the player, and its product is host-mirrored, so the client must not run its own
// spawner; the sky-wisp ticker spawns the sky wisps at absolute map coordinates, so the host rolls
// and clients mirror through the source-gated catch and the variant allowlist; the roach master's
// summon and its three looping timer entries fire independently of its tick. The tick rows: the
// insomniac and fossilhound tickers roll inside their tick with no delay chains or reap duties, so
// refusing the tick stops the roll and the product, and the products are host-mirrored; the roach
// master's tick drives roach movement, the food-eat mutation and crush traces, which a client running
// it would diverge, so it and its summoner are refused while the roach sync drives the client
// population. The jellyfish path's spawn makes seven fish inside its graph, and the host's are
// mirrored through the source-gated catch, so a client's own, its 18:00 roll's, is refused. The
// event creatures (the vent crawler, the gray pack, the eggs and the tentacle balls) are the same
// shape pushed one level down: on a client they exist only as host mirrors, so their AI, timers,
// overlaps and despawn verbs are refused while the AnimBP and the pose stream stay; and the
// bodies that mint them inside the gray controller, the balls follower, the super egger and the
// eventer's own verbs are refused on the client, because their BeginDeferred calls are
// bytecode-internal and no spawn interceptor ever sees them.

#include "coop/world/spawn_authority.h"

#include "coop/net/session.h"
#include "coop/world/event_fire_sync.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/script_gate.h"

#include <atomic>
#include <cstdint>
#include <iterator>

namespace coop::spawn_authority {
namespace {

namespace SG = ue_wrap::script_gate;

std::atomic<coop::net::Session*> g_session{nullptr};

// Refuse only while an active client session exists: the running flag flips true in the session
// start and false in the stop, which every disconnect path reaches; a bare role gate would bleed the
// refusal into single player after the session.
bool IsActiveClientSession() {
    auto* s = g_session.load(std::memory_order_acquire);
    return s && s->running() && s->role() == coop::net::Role::Client;
}

// One row: the body a client must not run, named by its class and its function. The function is
// spelled as the live header dump spells it; names compare without case. `replayBypass` marks the
// rows the event replay legitimately invokes on a client: event_fire_sync marks its own reflected
// Call with a thread-local scope naming the exact eventer object and UFunction, and only that
// call passes -- the gate's fromOurCode flag is the wider flag it replaces, since every
// synchronous coop dispatch on the thread inherits it. Every creature and event-spawner row
// stays false -- a mirror materialised through our own Call must still never arm its gameplay.
struct Row {
    const wchar_t* cls;
    const wchar_t* fn;
    const char* tag;
    bool replayBypass;
};
constexpr Row kRows[] = {
    {L"mushroomMaster_C",            L"Spawn",          "mushroomMaster.Spawn",          false},
    {L"mushroomSpawner_C",           L"Spawn",          "mushroomSpawner.Spawn",         false},
    // A late-game class: keyed by name, the watch holds from the class's first body, whenever it loads.
    {L"ticker_yellowWispSpawner_C",  L"ReceiveTick",    "yellowWispSpawner.ReceiveTick", false},
    // Sky wisps: world-anchored, so the host rolls; the source-gated catch and the variant
    // allowlist mirror the products.
    {L"ticker_wispSpawner_C",        L"ReceiveTick",    "wispSpawner.ReceiveTick",       false},
    // The space jellyfish: the host's run is mirrored through the source-gated catch.
    {L"jellyfishPath_C",             L"spawn",          "jellyfishPath.spawn",           false},
    // The roach sim's entries: the ticker's cross-object call and the three looping timer
    // delegates. The roach sync drives the client population instead.
    {L"cockroachMaster_C",           L"summonRoach",    "cockroachMaster.summonRoach",   false},
    {L"cockroachMaster_C",           L"addRoachTimer",  "cockroachMaster.addRoachTimer", false},
    {L"cockroachMaster_C",           L"spawnNestTimer", "cockroachMaster.spawnNestTimer",false},
    {L"cockroachMaster_C",           L"CustomEvent",    "cockroachMaster.CustomEvent",   false},
    // The ticks.
    {L"ticker_insomniacSpawner_C",   L"ReceiveTick",    "insomniacSpawner.ReceiveTick",  false},
    {L"ticker_fossilhoundSpawner_C", L"ReceiveTick",    "fossilhoundSpawner.ReceiveTick",false},
    {L"cockroachMaster_C",           L"ReceiveTick",    "cockroachMaster.ReceiveTick",   false},
    {L"ticker_roachSummoner_C",      L"ReceiveTick",    "roachSummoner.ReceiveTick",     false},

    // The story-event creatures: on a client every instance is a host mirror -- the local
    // spawn paths are suppressed through the allowlist or refused below -- so the gameplay
    // entries are refused class-wide. Animation and presentation stay: nothing here touches
    // the AnimBP, the timelines simply never arm without their owning entries, and the host's
    // pose stream owns the transform.
    // ventCrawler_C: the whole scene is the BeginPlay body -- it binds the prop_vent_C ref,
    // plays the moveTL timeline (which itself SetActorLocations the crawl), arms the vent-bang
    // and footstep events, and ends in the door writes, the setEvent flag and K2_DestroyActor
    // (ventCrawler_C UG @2669). The box overlap is the end-of-crawl contact: a client pawn
    // touching the mirror must not open the doors locally (@2363).
    {L"ventCrawler_C",               L"ReceiveBeginPlay","ventCrawler.ReceiveBeginPlay", false},
    {L"ventCrawler_C",               L"BndEvt__ventCrawler_Box_K2Node_ComponentBoundEvent_0_ComponentBeginOverlapSignature__DelegateSignature",
                                                            "ventCrawler.BoxBeginOverlap",false},
    // grayTest_C: BeginPlay re-attaches the mesh, gathers the pack and arms the wander MoveTo
    // plus the two looping 3 s timers and the 5-10 s despawn timer (@1117). The sphere overlap
    // is the player-catch: on a mainPlayer touch it calls grayController->despawn() and
    // deacCams() (@1508). deacCams/disableCams deactivate the client's own cameras, rdrone and
    // kerfur -- the damage write, not animation.
    {L"grayTest_C",                  L"ReceiveBeginPlay","grayTest.ReceiveBeginPlay",    false},
    {L"grayTest_C",                  L"BndEvt__grayTest_Sphere_K2Node_ComponentBoundEvent_0_ComponentBeginOverlapSignature__DelegateSignature",
                                                            "grayTest.SphereBeginOverlap",false},
    {L"grayTest_C",                  L"deacCams",       "grayTest.deacCams",             false},
    {L"grayTest_C",                  L"disableCams",    "grayTest.disableCams",          false},
    // eg_C: BeginPlay arms the wander (@1466); the tick bobs the mesh but also rolls the
    // camera-distance check into retrieve() and the rendered/setEvent writes (@15) -- refusing
    // it costs the bob, while letting it run lets a client egg hop on its own. retrieve() is
    // the fly-away despawn: collision off, gravity off, a 10 s delay into gamemode->eg=null
    // and setEvent (@1476). OnLanded continues the move chain (@1869).
    {L"eg_C",                        L"ReceiveBeginPlay","eg.ReceiveBeginPlay",          false},
    {L"eg_C",                        L"ReceiveTick",    "eg.ReceiveTick",                false},
    {L"eg_C",                        L"OnLanded",       "eg.OnLanded",                   false},
    {L"eg_C",                        L"retrieve",       "eg.retrieve",                   false},
    // tentacleBall_C: BeginPlay resolves the anim instance, starts Timeline_0 and the ambient
    // loop, and arms the 15-30 s timer and the first move (@7603). The tick is the brain:
    // proximity scans, the pindown/chase decision, the stare updates (@8872). move() is the
    // MoveTo verb it reaches (@10046); the sphere hit is the melee contact (@10314). The
    // montage-notify talk/light callbacks and step() stay -- sound and light presentation.
    {L"tentacleBall_C",              L"ReceiveBeginPlay","tentacleBall.ReceiveBeginPlay",false},
    {L"tentacleBall_C",              L"ReceiveTick",    "tentacleBall.ReceiveTick",      false},
    {L"tentacleBall_C",              L"move",           "tentacleBall.move",             false},
    {L"tentacleBall_C",              L"BndEvt__tentacleBall_Sphere_K2Node_ComponentBoundEvent_0_ComponentHitSignature__DelegateSignature",
                                                            "tentacleBall.SphereHit",    false},

    // The event spawners themselves: a client's copies must never mint children. Every
    // BeginDeferred inside these graphs is bytecode-internal, so the spawn interceptor never
    // sees them; refusing the bodies that contain them is the only suppression that holds.
    // grayEventController_C (level-placed on both sides): spawn() is the only body with the
    // grayTest BeginDeferred (@2617, the marker loop) -- it also plays the Audio, writes
    // isRaining=false and runs the deac-cams SphereOverlap. begin() (@3064) is the eventer's
    // entry -- the player punch, the transformer break, the car zap -- and turnedon (@3388)
    // and the triggerbox overlap (@3309) are its local activations.
    {L"grayEventController_C",       L"begin",          "grayEventController.begin",     false},
    {L"grayEventController_C",       L"turnedon",       "grayEventController.turnedon",  false},
    {L"grayEventController_C",       L"spawn",          "grayEventController.spawn",     false},
    {L"grayEventController_C",       L"BndEvt__grayEventController_triggerbox_K2Node_ComponentBoundEvent_0_ComponentBeginOverlapSignature__DelegateSignature",
                                                            "grayEventController.triggerboxOverlap",false},
    // tentacleBallsFollower_C (level-placed on both sides): runTrigger is the five-ball spawn
    // loop plus the setBuddies pass and the cleanup timers (@3432). The tick moves the follow
    // marker along the spline and writes setEvent at the resume and end points (@2012); the
    // client's copy has an empty balls array and nothing to guide.
    {L"tentacleBallsFollower_C",     L"runTrigger",     "tentacleBallsFollower.runTrigger",false},
    {L"tentacleBallsFollower_C",     L"ReceiveTick",    "tentacleBallsFollower.ReceiveTick",false},
    // superEgger_C: a client's copy is a world-actor mirror, but FinishSpawning runs its
    // BeginPlay, which builds the spawn grid and chains spwn() -- the only body with the eg_C
    // BeginDeferred (@1399, @1316). Refusing both keeps the mirror a husk.
    {L"superEgger_C",                L"ReceiveBeginPlay","superEgger.ReceiveBeginPlay",  false},
    {L"superEgger_C",                L"spwn",           "superEgger.spwn",               false},

    // The dispatcher: a client's trigger_eventer_C must never fire an event natively. The
    // scheduler is dormant (event_fire_sync zeroes allEvents) and the creature rows are held
    // out of the replay, but a level trigger chain, a save-restore call or the game's own
    // event and cheat menus can reach the verbs' bodies directly -- ui_cheatMenu calls
    // runSpecialEvent outright -- and every spawn site inside is bytecode-internal, so a
    // native special would roll its own prank and write its own scene. runEvent and
    // runSpecialEvent pass only inside the replay's marked Call (the exact object and
    // function); summonArirPrank is the prank roll itself and gets no bypass: the wire
    // carries the already-rolled case, so a client's copy must never run it and shrink its
    // local pool. A replayed runEvent never reaches it either -- the replay passes
    // special=None, and ariralPrank is the roll's only caller path.
    {L"trigger_eventer_C",           L"runEvent",        "eventer.runEvent",        true},
    {L"trigger_eventer_C",           L"runSpecialEvent", "eventer.runSpecialEvent", true},
    {L"trigger_eventer_C",           L"summonArirPrank", "eventer.summonArirPrank", false},
};
constexpr int kRowCount = static_cast<int>(std::size(kRows));
constexpr int kTagBase = 0x53410000;   // 'SA', then the row

// Refusals per row, for the log: the first and then each power of two, so a refused tick proves itself
// without a line a second. Game thread, as the gate's callbacks are.
std::uint64_t g_refused[kRowCount] = {};

SG::Verdict OnRowPre(const SG::Call& call) {
    if (!IsActiveClientSession()) return SG::Verdict::Run;
    const int row = call.tag - kTagBase;
    if (row < 0 || row >= kRowCount) return SG::Verdict::Run;
    // The replay rows pass only for the replay's own call: the scope names the exact eventer
    // object and UFunction the reflected dispatch marked, which is the narrow truth the old
    // fromOurCode pass approximated -- and let through for every other coop call that reached
    // the verb on the same thread. A refused replay would delete the event the wire asked for;
    // the creature rows keep no bypass, so a mirror born inside our own Call is still inert.
    if (kRows[row].replayBypass &&
        coop::event_fire_sync::InReplayScope(call.object, call.function)) {
        return SG::Verdict::Run;
    }
    const std::uint64_t n = ++g_refused[row];
    if ((n & (n - 1)) == 0)
        UE_LOGI("spawn_authority[%s]: client-refuse %p (call #%llu)", kRows[row].tag, call.object,
                static_cast<unsigned long long>(n));
    return SG::Verdict::Cancel;
}

std::atomic<bool> g_watched{false};

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (g_watched.exchange(true, std::memory_order_acq_rel)) return;
    int watched = 0;
    for (int i = 0; i < kRowCount; ++i) {
        if (SG::WatchClassName(kRows[i].cls, kRows[i].fn, kTagBase + i, &OnRowPre, nullptr)) {
            ++watched;
        } else {
            UE_LOGE("spawn_authority: the watch on %ls::%ls did not register -- a client runs it",
                    kRows[i].cls, kRows[i].fn);
        }
    }
    UE_LOGI("spawn_authority: %d/%d spawner watches registered by class and name, each live once the game "
            "thread resolves its names; refused only on an active client session", watched, kRowCount);
}

}  // namespace coop::spawn_authority
