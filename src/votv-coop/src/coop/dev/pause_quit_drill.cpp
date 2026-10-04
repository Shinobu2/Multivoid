// coop/dev/pause_quit_drill.cpp -- see coop/dev/pause_quit_drill.h.

#include "coop/dev/pause_quit_drill.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/run_end_travel.h"
#include "coop/props/prop_snapshot.h"
#include "coop/save/join_window_baseline.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"  // HasAnnouncedWorldReady
#include "ui/multiplayer_menu.h"    // IsPauseMenuOpen

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace coop::dev::pause_quit_drill {
namespace {

namespace R = ue_wrap::reflection;
namespace P = ue_wrap::profile;
using Clock = std::chrono::steady_clock;

enum class Arm : uint8_t { Off, Client, Host };

Arm ArmNow() {
    static const Arm arm = [] {
        const std::string v = coop::config::ResolveEnum(::coop::config_registry::rows::pause_quit_drill);
        return v == "client" ? Arm::Client : v == "host" ? Arm::Host : Arm::Off;
    }();
    return arm;
}

// Armed: waiting for the session to settle. WaitMenu: Escape sent, waiting for the pause menu.
// Done: the click was made, or the drill gave up. The latch is per PROCESS: the quit ends the
// session, and a drill that started over with the next one would quit again.
enum class Phase : uint8_t { Armed, WaitMenu, Done };
Phase g_phase = Phase::Armed;
Clock::time_point g_escapeAt{};
Clock::time_point g_lastFind{};

// A fail-safe bound on a wait that is otherwise on readiness (the menu opening), not a clock the
// drill's verdict rests on.
constexpr auto kAbortAfter = std::chrono::seconds(30);
// The walk that finds the menu instance is a one-shot-rate call.
constexpr auto kFindEvery = std::chrono::seconds(1);

const char* Me() { return ArmNow() == Arm::Host ? "host" : "client"; }

void Abort(const char* why) {
    UE_LOGW("[PAUSE-QUIT] %s ABORT: %s", Me(), why);
    g_phase = Phase::Done;
}

// What `rig: READY joined` stands on, the settled test the chat drill's client uses.
bool ClientReady(coop::net::Session& s) {
    return s.connected() && coop::net_pump::HasAnnouncedWorldReady() &&
           coop::join_progress::CurrentPhase() == coop::join_progress::Phase::Idle;
}

// Some slot's join traffic is DELIVERED (the settled-client test of the ban drill), so the host
// quits a joined peer's session, not one mid-join.
bool HostReady(coop::net::Session& s) {
    for (int slot = 1; slot < coop::players::kMaxPeers; ++slot) {
        if (s.IsSlotWorldReady(slot) && coop::prop_snapshot::IsBracketClosed(slot) &&
            !coop::join_window_baseline::IsLateWindowOpen(slot) && s.SlotReliableIdle(slot))
            return true;
    }
    return false;
}

// A zeroed parameter frame for `fn`, as the reflected input-event calls take.
std::vector<uint8_t> ZeroFrame(void* fn) {
    const int32_t size = R::FunctionFrameSize(fn);
    return std::vector<uint8_t>(size > 0 ? static_cast<size_t>(size) : 0, 0u);
}

// The player's Escape event, which opens the pause menu unless the player is dreaming, asleep, out
// of bounds, dead or ragdolled. The local player is the registry's, never a lookup by class: a
// puppet is a mainPlayer_C too.
void SendEscape() {
    void* player = coop::players::Registry::Get().Local();
    if (!player) return;  // not yet; the next tick asks again
    void* fn = R::FindFunction(R::ClassOf(player), P::name::MainPlayerEscapeFn);
    if (!fn) return Abort("the Escape event was not found on mainPlayer_C");
    std::vector<uint8_t> frame = ZeroFrame(fn);
    R::CallFunction(player, fn, frame.empty() ? nullptr : frame.data());
    UE_LOGI("[PAUSE-QUIT] %s: Escape sent", Me());
    g_escapeAt = Clock::now();
    g_lastFind = Clock::time_point{};
    g_phase = Phase::WaitMenu;
}

// A ui_menu_C reads isPause true while it is the in-game pause menu. The property is read on the
// class (the function walks a class's property chain), then the bit from the instance.
bool IsPauseInstance(void* menu) {
    static int32_t off = -1;
    static uint8_t mask = 0;
    if (off < 0 && !R::FindBoolProperty(R::ClassOf(menu), P::name::UiMenuIsPauseProp, off, mask))
        return false;
    return (*(static_cast<const uint8_t*>(menu) + off) & mask) != 0;
}

// The pause menu: the live ui_menu_C whose isPause reads true. Not a world filter (the widget is
// owned by the GameInstance, so it has no world), and FindObjectsByClass can return a departed
// widget, hence the liveness test. Null while none is found; sets `ambiguous` for two or more.
void* FindPauseMenu(bool& ambiguous) {
    void* found = nullptr;
    for (void* obj : R::FindObjectsByClass(P::name::UiMenuClass)) {
        if (!R::IsLive(obj) || !IsPauseInstance(obj)) continue;
        if (found) { ambiguous = true; return nullptr; }
        found = obj;
    }
    return found;
}

// The click handler of the "Main menu" button. It is synchronous: its script calls
// lib_C::loadLevel("menu") unless an event is active or the camera is out of bounds, and the
// run-ending watch counts that call before this returns, so an unchanged count is a refusal.
void PressMainMenu(void* menu) {
    void* fn = R::FindFunction(R::ClassOf(menu), P::name::PauseMenuQuitClickFn);
    if (!fn) return Abort("the Main menu click handler was not found on ui_menu_C");
    const unsigned long long before = coop::player::run_end_travel::MenuTravelsSeen();
    std::vector<uint8_t> frame = ZeroFrame(fn);
    R::CallFunction(menu, fn, frame.empty() ? nullptr : frame.data());
    g_phase = Phase::Done;
    if (coop::player::run_end_travel::MenuTravelsSeen() == before) {
        UE_LOGW("[PAUSE-QUIT] %s FAIL: the click did not reach loadLevel (an event is active, or out "
                "of bounds)", Me());
        return;
    }
    UE_LOGI("[PAUSE-QUIT] %s: Main menu pressed", Me());
}

void WaitForMenu() {
    if (Clock::now() - g_escapeAt > kAbortAfter)
        return Abort("the pause menu did not open (in bed, sitting, an interface open?)");
    if (!coop::multiplayer_menu::IsPauseMenuOpen()) return;
    if (g_lastFind.time_since_epoch().count() != 0 && Clock::now() - g_lastFind < kFindEvery) return;
    g_lastFind = Clock::now();
    bool ambiguous = false;
    void* menu = FindPauseMenu(ambiguous);
    if (ambiguous) return Abort("two pause menus");
    if (menu) PressMainMenu(menu);
}

}  // namespace

void Tick(coop::net::Session* s) {
    const Arm arm = ArmNow();
    if (arm == Arm::Off || g_phase == Phase::Done || !s || !s->running()) return;
    const bool host = s->role() == coop::net::Role::Host;
    if (host != (arm == Arm::Host)) return;
    if (g_phase == Phase::Armed) {
        if (host ? HostReady(*s) : ClientReady(*s)) SendEscape();
        return;
    }
    WaitForMenu();
}

void OnDisconnect() {
    if (g_phase == Phase::WaitMenu) g_phase = Phase::Done;
}

}  // namespace coop::dev::pause_quit_drill
