// coop/dev/settings_drill.cpp -- see coop/dev/settings_drill.h.

#include "coop/dev/settings_drill.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/net/session.h"
#include "coop/player/nameplate.h"
#include "coop/player/nick_color.h"

#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"

#include <string>

namespace coop::dev::settings_drill {
namespace {

constexpr int kSlot = 1;  // the pair's client

// The step the row names, parsed once at the first Tick. Only Off, Nameplate, NickColor and Red
// are parsed from the row; the other enumerators are not produced yet.
enum class Token {
    Off, Nameplate, NickColor, Flags, Scale, Font, VoiceMode, VoiceVolume, Server, ServerJoin,
    ServerRed, Red
};

// 0 idle; 1 SET done, the probe posted; 2 RESET done, the probe posted; 3 DONE.
int   g_phase = 0;
Token g_token = Token::Off;

Token ParseToken(const std::string& mode) {
    if (mode == "nameplate") return Token::Nameplate;
    if (mode == "nickcolor") return Token::NickColor;
    if (mode == "red")       return Token::Red;
    return Token::Off;
}

const char* SetResultName(coop::config::SetResult r) {
    switch (r) {
    case coop::config::SetResult::Saved:        return "Saved";
    case coop::config::SetResult::HeldNotSaved: return "HeldNotSaved";
    case coop::config::SetResult::Refused:      return "Refused";
    }
    return "Refused";
}

void Done() {
    g_phase = 3;
    UE_LOGI("[SETTINGS-DRILL] host DONE");
}

// Game thread, after the subscriber of the reset (FIFO).
void ProbeAfterReset() {
    if (g_token == Token::NickColor) {
        const uint32_t packed = coop::nick_color::LocalPacked();
        if (packed == coop::nick_color::Pack(255, 255, 255))
            UE_LOGI("[SETTINGS-DRILL] host: nick_color followed back (packed=%08X)", packed);
        else
            UE_LOGW("[SETTINGS-DRILL] FAIL: nick_color did not follow back (packed=%08X)", packed);
    } else {
        const bool v = coop::nameplate::LocalVisible();
        if (v) UE_LOGI("[SETTINGS-DRILL] host: nameplate followed back (visible=1)");
        else   UE_LOGW("[SETTINGS-DRILL] FAIL: nameplate did not follow back (visible=0)");
    }
    Done();
}

void Reset() {
    if (g_token == Token::NickColor) {
        UE_LOGI("[SETTINGS-DRILL] host: ResetValue nick_color");
        const coop::config::SetResult r =
            coop::config::ResetValue(::coop::config_registry::rows::nick_color);
        UE_LOGI("[SETTINGS-DRILL] host: ResetValue nick_color returned %s", SetResultName(r));
    } else {
        UE_LOGI("[SETTINGS-DRILL] host: ResetValue nameplate");
        const coop::config::SetResult r =
            coop::config::ResetValue(::coop::config_registry::rows::nameplate);
        UE_LOGI("[SETTINGS-DRILL] host: ResetValue nameplate returned %s", SetResultName(r));
    }
    ue_wrap::game_thread::Post(&ProbeAfterReset);
    g_phase = 2;
}

// Game thread, after the subscriber of the set (FIFO). A failed probe ends the step.
void ProbeAfterSet() {
    bool followed = false;
    if (g_token == Token::NickColor) {
        const uint32_t packed = coop::nick_color::LocalPacked();
        followed = (packed == 0);
        if (followed) UE_LOGI("[SETTINGS-DRILL] host: nick_color followed (packed=%08X)", packed);
        else UE_LOGW("[SETTINGS-DRILL] FAIL: nick_color did not follow (packed=%08X)", packed);
    } else {
        const bool v = coop::nameplate::LocalVisible();
        followed = !v;
        if (followed) UE_LOGI("[SETTINGS-DRILL] host: nameplate followed (visible=0)");
        else UE_LOGW("[SETTINGS-DRILL] FAIL: nameplate did not follow (visible=1)");
    }
    if (followed) Reset();
    else Done();
}

// The red arm is the nameplate step with its one call skipped.
void Set() {
    const bool skipSet = (g_token == Token::Red);
    if (g_token == Token::NickColor) {
        UE_LOGI("[SETTINGS-DRILL] host: SetValue nick_color= (per-surface default)");
        const coop::config::SetResult r = coop::config::SetValue(
            ::coop::config_registry::rows::nick_color, coop::nick_color::IniTextFor(0).c_str());
        UE_LOGI("[SETTINGS-DRILL] host: SetValue nick_color= (per-surface default) returned %s",
                SetResultName(r));
    } else {
        UE_LOGI("[SETTINGS-DRILL] host: SetValue nameplate=0");
        if (!skipSet) {
            const coop::config::SetResult r =
                coop::config::SetValue(::coop::config_registry::rows::nameplate, "0");
            UE_LOGI("[SETTINGS-DRILL] host: SetValue nameplate=0 returned %s", SetResultName(r));
        }
    }
    ue_wrap::game_thread::Post(&ProbeAfterSet);
    g_phase = 1;
}

}  // namespace

void Tick(coop::net::Session* session) {
    static const bool s_parsed = [] {
        g_token = ParseToken(
            coop::config::ResolveEnum(::coop::config_registry::rows::settings_drill));
        return true;
    }();
    (void)s_parsed;
    if (g_token == Token::Off) return;
    if (!session || !session->running() || session->role() != coop::net::Role::Host) return;
    if (g_phase != 0 || !session->IsSlotWorldReady(kSlot)) return;
    Set();
}

void OnDisconnect() {
    g_phase = 0;
}

}  // namespace coop::dev::settings_drill
