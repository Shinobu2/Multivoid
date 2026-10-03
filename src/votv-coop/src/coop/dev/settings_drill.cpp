// coop/dev/settings_drill.cpp -- see coop/dev/settings_drill.h.

#include "coop/dev/settings_drill.h"

#include "coop/config/config.h"
#include "coop/comms/peer_action_feed.h"
#include "coop/config/config_registry.h"
#include "coop/net/session.h"
#include "coop/player/nameplate.h"
#include "coop/player/nick_color.h"
#include "coop/voice/voice_chat.h"

#include "ui/fonts.h"
#include "ui/hud.h"
#include "ui/net_stats_panel.h"
#include "ui/scale.h"

#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"

#include <cmath>
#include <cstdint>
#include <string>

namespace coop::dev::settings_drill {
namespace {

constexpr int kSlot = 1;  // the pair's client

// The step the row names, parsed once at the first Tick. Only Off, Nameplate, NickColor, Flags,
// Scale, Font, VoiceMode, VoiceVolume and Red are parsed from the row; the other enumerators are
// not produced yet.
enum class Token {
    Off, Nameplate, NickColor, Flags, Scale, Font, VoiceMode, VoiceVolume, Server, ServerJoin,
    ServerRed, Red
};

// 0 idle; 1 SET done, waiting (the probe posted, or for a counter token the counter to move);
// 2 RESET done, waiting likewise; 3 DONE.
int      g_phase = 0;
Token    g_token = Token::Off;
// A counter token's counter, read immediately before the SET and again before the RESET.
uint32_t g_baseline = 0;

Token ParseToken(const std::string& mode) {
    if (mode == "nameplate") return Token::Nameplate;
    if (mode == "nickcolor") return Token::NickColor;
    if (mode == "flags")     return Token::Flags;
    if (mode == "scale")     return Token::Scale;
    if (mode == "font")      return Token::Font;
    if (mode == "voicemode") return Token::VoiceMode;
    if (mode == "voicevolume") return Token::VoiceVolume;
    if (mode == "red")       return Token::Red;
    return Token::Off;
}

// The render thread applies a scale or a font row at a later drawn frame, and the voice tick
// reopens the devices for a mode row at a later game tick, so these three tokens wait on the
// module's own counter instead of a posted probe.
bool IsCounterToken() {
    return g_token == Token::Scale || g_token == Token::Font || g_token == Token::VoiceMode;
}

uint32_t Counter() {
    if (g_token == Token::Scale) return ui::scale::RowApplies();
    if (g_token == Token::Font) return ui::fonts::RowsApplies();
    return coop::voice_chat::Reopens();
}

// The counter's name in the step's lines: a scale or font row is applied, a mode row reopens.
const char* CounterName() { return g_token == Token::VoiceMode ? "reopens" : "applies"; }

// The role's row: the first font role is ui.font.menu.
const coop::config_registry::EnumRow& FontRow() { return coop::config_registry::FontRoleRow(0); }

const char* CounterKey() {
    if (g_token == Token::Scale) return ::coop::config_registry::rows::ui_scale.row->key;
    if (g_token == Token::Font) return FontRow().row->key;
    return ::coop::config_registry::rows::voice_mode.row->key;
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
    if (g_token == Token::Flags) {
        const bool peerActions = coop::peer_action_feed::Enabled();
        const bool netstats    = ui::net_stats_panel::Enabled();
        if (peerActions && !netstats)
            UE_LOGI("[SETTINGS-DRILL] host: flags followed back (peer_actions=1 netstats=0)");
        else
            UE_LOGW("[SETTINGS-DRILL] FAIL: flags did not follow back (peer_actions=%d netstats=%d)",
                    peerActions ? 1 : 0, netstats ? 1 : 0);
    } else if (g_token == Token::NickColor) {
        const uint32_t packed = coop::nick_color::LocalPacked();
        if (packed == coop::nick_color::Pack(255, 255, 255))
            UE_LOGI("[SETTINGS-DRILL] host: nick_color followed back (packed=%08X)", packed);
        else
            UE_LOGW("[SETTINGS-DRILL] FAIL: nick_color did not follow back (packed=%08X)", packed);
    } else if (g_token == Token::VoiceVolume) {
        const float def = ::coop::config_registry::rows::voice_volume.row->defF;
        const float vol = coop::voice_chat::MasterVolume();
        if (std::fabs(vol - def) < 0.001f)
            UE_LOGI("[SETTINGS-DRILL] host: voice.volume followed back (volume=%.2f)", vol);
        else
            UE_LOGW("[SETTINGS-DRILL] FAIL: voice.volume did not follow back (volume=%.2f)", vol);
    } else {
        const bool v = coop::nameplate::LocalVisible();
        if (v) UE_LOGI("[SETTINGS-DRILL] host: nameplate followed back (visible=1)");
        else   UE_LOGW("[SETTINGS-DRILL] FAIL: nameplate did not follow back (visible=0)");
    }
    Done();
}

// A counter token's RESET: no probe is posted; Tick waits for the counter to move.
void ResetCounterRow() {
    const char* key = CounterKey();
    g_baseline = Counter();
    UE_LOGI("[SETTINGS-DRILL] host: ResetValue %s", key);
    coop::config::SetResult r;
    if (g_token == Token::Scale)
        r = coop::config::ResetValue(::coop::config_registry::rows::ui_scale);
    else if (g_token == Token::Font)
        r = coop::config::ResetValue(FontRow());
    else
        r = coop::config::ResetValue(::coop::config_registry::rows::voice_mode);
    UE_LOGI("[SETTINGS-DRILL] host: ResetValue %s returned %s", key, SetResultName(r));
    g_phase = 2;
}

void Reset() {
    if (IsCounterToken()) {
        ResetCounterRow();
        return;
    }
    if (g_token == Token::Flags) {
        UE_LOGI("[SETTINGS-DRILL] host: ResetValue ui.chat.peer_actions");
        const coop::config::SetResult r1 =
            coop::config::ResetValue(::coop::config_registry::rows::ui_chat_peer_actions);
        UE_LOGI("[SETTINGS-DRILL] host: ResetValue ui.chat.peer_actions returned %s",
                SetResultName(r1));
        UE_LOGI("[SETTINGS-DRILL] host: ResetValue ui.netstats");
        const coop::config::SetResult r2 =
            coop::config::ResetValue(::coop::config_registry::rows::ui_netstats);
        UE_LOGI("[SETTINGS-DRILL] host: ResetValue ui.netstats returned %s", SetResultName(r2));
    } else if (g_token == Token::NickColor) {
        UE_LOGI("[SETTINGS-DRILL] host: ResetValue nick_color");
        const coop::config::SetResult r =
            coop::config::ResetValue(::coop::config_registry::rows::nick_color);
        UE_LOGI("[SETTINGS-DRILL] host: ResetValue nick_color returned %s", SetResultName(r));
    } else if (g_token == Token::VoiceVolume) {
        UE_LOGI("[SETTINGS-DRILL] host: ResetValue voice.volume");
        const coop::config::SetResult r =
            coop::config::ResetValue(::coop::config_registry::rows::voice_volume);
        UE_LOGI("[SETTINGS-DRILL] host: ResetValue voice.volume returned %s", SetResultName(r));
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
    if (g_token == Token::Flags) {
        const bool peerActions = coop::peer_action_feed::Enabled();
        const bool netstats    = ui::net_stats_panel::Enabled();
        followed = !peerActions && netstats;
        if (followed) UE_LOGI("[SETTINGS-DRILL] host: flags followed (peer_actions=0 netstats=1)");
        else UE_LOGW("[SETTINGS-DRILL] FAIL: flags did not follow (peer_actions=%d netstats=%d)",
                     peerActions ? 1 : 0, netstats ? 1 : 0);
    } else if (g_token == Token::NickColor) {
        const uint32_t packed = coop::nick_color::LocalPacked();
        followed = (packed == 0);
        if (followed) UE_LOGI("[SETTINGS-DRILL] host: nick_color followed (packed=%08X)", packed);
        else UE_LOGW("[SETTINGS-DRILL] FAIL: nick_color did not follow (packed=%08X)", packed);
    } else if (g_token == Token::VoiceVolume) {
        const float vol = coop::voice_chat::MasterVolume();
        followed = std::fabs(vol - 0.5f) < 0.001f;
        if (followed) UE_LOGI("[SETTINGS-DRILL] host: voice.volume followed (volume=%.2f)", vol);
        else UE_LOGW("[SETTINGS-DRILL] FAIL: voice.volume did not follow (volume=%.2f)", vol);
    } else {
        const bool v = coop::nameplate::LocalVisible();
        followed = !v;
        if (followed) UE_LOGI("[SETTINGS-DRILL] host: nameplate followed (visible=0)");
        else UE_LOGW("[SETTINGS-DRILL] FAIL: nameplate did not follow (visible=1)");
    }
    if (followed) Reset();
    else Done();
}

// The first token of the font row's list that is not the family it holds now.
std::string OtherFontToken() {
    const std::string cur = coop::config::ResolveEnum(FontRow());
    const std::string list = FontRow().row->tokens;
    size_t pos = 0;
    while (pos <= list.size()) {
        size_t bar = list.find('|', pos);
        if (bar == std::string::npos) bar = list.size();
        std::string tok = list.substr(pos, bar - pos);
        if (tok != cur) return tok;
        pos = bar + 1;
    }
    return std::string();
}

// A counter token's SET. The baseline is read before the set; the drawn-frame line (the scale and
// font tokens) says whether the HUD keeps the overlay drawing, because the render thread applies
// only on a drawn frame.
void SetCounterRow() {
    const char* key = CounterKey();
    std::string value;
    if (g_token == Token::Scale) value = "1.50";
    else if (g_token == Token::Font) value = OtherFontToken();
    else value = "activation";
    g_baseline = Counter();
    UE_LOGI("[SETTINGS-DRILL] host: SetValue %s=%s", key, value.c_str());
    coop::config::SetResult r;
    if (g_token == Token::Scale)
        r = coop::config::SetValue(::coop::config_registry::rows::ui_scale, value.c_str());
    else if (g_token == Token::Font)
        r = coop::config::SetValue(FontRow(), value.c_str());
    else
        r = coop::config::SetValue(::coop::config_registry::rows::voice_mode, value.c_str());
    UE_LOGI("[SETTINGS-DRILL] host: SetValue %s=%s returned %s", key, value.c_str(),
            SetResultName(r));
    if (g_token != Token::VoiceMode)
        UE_LOGI("[SETTINGS-DRILL] host: waiting for a drawn frame (hud=%d)",
                ui::hud::IsActive() ? 1 : 0);
    g_phase = 1;
}

// Game thread, each Tick while a counter token waits.
void PollCounter() {
    const uint32_t now = Counter();
    if (now == g_baseline) return;
    if (g_phase == 1) {
        UE_LOGI("[SETTINGS-DRILL] host: %s followed (%s=%u)", CounterKey(), CounterName(), now);
        Reset();
    } else {
        UE_LOGI("[SETTINGS-DRILL] host: %s followed back (%s=%u)", CounterKey(), CounterName(),
                now);
        Done();
    }
}

// The red arm is the nameplate step with its one call skipped.
void Set() {
    if (IsCounterToken()) {
        SetCounterRow();
        return;
    }
    const bool skipSet = (g_token == Token::Red);
    if (g_token == Token::Flags) {
        UE_LOGI("[SETTINGS-DRILL] host: SetValue ui.chat.peer_actions=0");
        const coop::config::SetResult r1 =
            coop::config::SetValue(::coop::config_registry::rows::ui_chat_peer_actions, "0");
        UE_LOGI("[SETTINGS-DRILL] host: SetValue ui.chat.peer_actions=0 returned %s",
                SetResultName(r1));
        UE_LOGI("[SETTINGS-DRILL] host: SetValue ui.netstats=1");
        const coop::config::SetResult r2 =
            coop::config::SetValue(::coop::config_registry::rows::ui_netstats, "1");
        UE_LOGI("[SETTINGS-DRILL] host: SetValue ui.netstats=1 returned %s", SetResultName(r2));
    } else if (g_token == Token::NickColor) {
        UE_LOGI("[SETTINGS-DRILL] host: SetValue nick_color= (per-surface default)");
        const coop::config::SetResult r = coop::config::SetValue(
            ::coop::config_registry::rows::nick_color, coop::nick_color::IniTextFor(0).c_str());
        UE_LOGI("[SETTINGS-DRILL] host: SetValue nick_color= (per-surface default) returned %s",
                SetResultName(r));
    } else if (g_token == Token::VoiceVolume) {
        UE_LOGI("[SETTINGS-DRILL] host: SetValue voice.volume=0.50");
        const coop::config::SetResult r =
            coop::config::SetValue(::coop::config_registry::rows::voice_volume, "0.50");
        UE_LOGI("[SETTINGS-DRILL] host: SetValue voice.volume=0.50 returned %s", SetResultName(r));
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
    if (IsCounterToken() && (g_phase == 1 || g_phase == 2)) {
        PollCounter();
        return;
    }
    if (g_phase != 0 || !session->IsSlotWorldReady(kSlot)) return;
    Set();
}

void OnDisconnect() {
    g_phase = 0;
}

}  // namespace coop::dev::settings_drill
