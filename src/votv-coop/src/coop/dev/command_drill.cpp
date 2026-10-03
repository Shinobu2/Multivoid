// coop/dev/command_drill.cpp -- see coop/dev/command_drill.h.

#include "coop/dev/command_drill.h"

#include "coop/commands/command_sync.h"
#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/net/session.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"  // HasAnnouncedWorldReady

#include "ue_wrap/core/log.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace coop::dev::command_drill {
namespace {

namespace CS = coop::command_sync;

enum class Mode : uint8_t { Off, On, Red };

Mode ModeNow() {
    static const Mode mode = [] {
        const std::string v = coop::config::ResolveEnum(::coop::config_registry::rows::command_drill);
        return v == "on" ? Mode::On : v == "red" ? Mode::Red : Mode::Off;
    }();
    return mode;
}

enum class Phase : uint8_t { Ready, WaitA, SendB, WaitB, Done };

Phase g_phase = Phase::Ready;
bool g_isHost = false;
int g_replies = 0;   // phase A lines seen
int g_accepted = 0;  // phase B unknown-command lines seen before the rate notice

constexpr const char* kHelpHeader = "Commands you can use:";
constexpr const char* kHelpHeaderRed = "Commands you may use:";
constexpr const char* kHelpLine =
    "/help [command...] -- Lists the commands you can use, or shows how to use one.";
constexpr const char* kUnknownLine = "Unknown command '/nosuchcommand'. Type /help for the commands.";
constexpr const char* kRateNotice = "Too many commands -- wait a moment.";
constexpr int kBurstLines = 6;

void Finish() {
    g_phase = Phase::Done;
    CS::SetReplyObserver(nullptr);
}

void Fail(int n, std::string_view got, const char* want) {
    UE_LOGE("[CMD-DRILL] FAIL: reply %d was '%s', expected '%s'", n, std::string(got).c_str(), want);
    Finish();
}

void OnPhaseA(std::string_view line) {
    const char* want[3] = {ModeNow() == Mode::Red ? kHelpHeaderRed : kHelpHeader, kHelpLine, kUnknownLine};
    const int i = g_replies;
    if (line != want[i]) { Fail(i + 1, line, want[i]); return; }
    if (++g_replies < 3) return;
    if (g_isHost) {
        UE_LOGI("[CMD-DRILL] host DONE");
        Finish();
    } else {
        g_phase = Phase::SendB;
    }
}

void OnPhaseB(std::string_view line) {
    if (line == kUnknownLine) { ++g_accepted; return; }
    if (line != kRateNotice) { Fail(3 + g_accepted + 1, line, kRateNotice); return; }
    if (g_accepted < 1 || g_accepted > kBurstLines - 1) {
        UE_LOGE("[CMD-DRILL] FAIL: %d of %d lines were answered before the rate notice, expected 1 to %d",
                g_accepted, kBurstLines, kBurstLines - 1);
        Finish();
        return;
    }
    UE_LOGI("[CMD-DRILL] client DONE (accepted %d of %d, told once)", g_accepted, kBurstLines);
    Finish();
}

// Every delivered reply line of this peer, in order.
void Observe(std::string_view line) {
    if (g_phase == Phase::WaitA) OnPhaseA(line);
    else if (g_phase == Phase::WaitB) OnPhaseB(line);
}

bool ClientReady(coop::net::Session* s) {
    return s->connected() && coop::net_pump::HasAnnouncedWorldReady() &&
           coop::join_progress::CurrentPhase() == coop::join_progress::Phase::Idle;
}

}  // namespace

void Tick(coop::net::Session* session) {
    if (ModeNow() == Mode::Off || !session || !session->running()) return;
    switch (g_phase) {
    case Phase::Ready:
        g_isHost = session->role() == coop::net::Role::Host;
        if (!g_isHost && !ClientReady(session)) return;
        g_replies = 0;
        g_accepted = 0;
        CS::SetReplyObserver(&Observe);
        g_phase = Phase::WaitA;
        CS::Submit("help");
        CS::Submit("nosuchcommand");
        UE_LOGI("[CMD-DRILL] %s sent phase A", g_isHost ? "host" : "client");
        return;
    case Phase::SendB:
        g_phase = Phase::WaitB;
        for (int i = 0; i < kBurstLines; ++i) CS::Submit("nosuchcommand");
        UE_LOGI("[CMD-DRILL] client sent phase B (%d lines)", kBurstLines);
        return;
    case Phase::WaitA:
    case Phase::WaitB:
    case Phase::Done:
        return;
    }
}

void OnDisconnect() {
    CS::SetReplyObserver(nullptr);
    g_phase = Phase::Ready;
    g_replies = 0;
    g_accepted = 0;
}

}  // namespace coop::dev::command_drill
