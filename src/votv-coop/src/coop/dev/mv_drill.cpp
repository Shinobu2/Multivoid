// coop/dev/mv_drill.cpp -- see coop/dev/mv_drill.h.

#include "coop/dev/mv_drill.h"

#include "coop/commands/command_sync.h"
#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/dev/command_drill.h"
#include "coop/net/peer_identity.h"
#include "coop/net/session.h"
#include "coop/permissions/grants_core.h"
#include "coop/player/players_registry.h"
#include "coop/player/roster.h"
#include "coop/player/roster_ledger.h"
#include "coop/server_profile/server_profile.h"
#include "coop/session/local_grants.h"

#include "ue_wrap/core/log.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace coop::dev::mv_drill {
namespace {

namespace CS = coop::command_sync;
namespace G = coop::permissions::grants;
namespace LG = coop::session::local_grants;

enum class Arm : uint8_t { Off, On, Red };

Arm ArmNow() {
    static const Arm arm = [] {
        const std::string v = coop::config::ResolveEnum(::coop::config_registry::rows::mv_drill);
        return v == "on" ? Arm::On : v == "red" ? Arm::Red : Arm::Off;
    }();
    return arm;
}

// The rig's ids: the two installs' identity keys (the same names the grants fixtures carry), and
// the offline player the seeded store names.
#define MVD_HOST "7f990136fefd61696706ff16d74362df"
#define MVD_CLIENT "9af578e3dea27e43f722b3a2c7af8b1f"
#define MVD_OWNER "000000000000000000000000000c4d01"
constexpr const char* kHostId = MVD_HOST;
constexpr const char* kClientId = MVD_CLIENT;

// Budgets: a hang guard that fails the drill, never a pacing. A Send waits for this peer's own
// answer; a WaitNotice or WaitBit for the other peer's action.
constexpr std::chrono::seconds kSendBudget{30};
constexpr std::chrono::seconds kWaitBudget{60};

enum class Kind : uint8_t { Send, WaitNotice, WriteFile, RemoveFile, ExpectFile, WaitBit };

// Send: a = the line, b = the expected first reply. WaitNotice: a = the text the notice contains.
// WriteFile: a = the path under the permissions folder, b = the text. RemoveFile / ExpectFile: a =
// the path, flag = present (ExpectFile). WaitBit: flag = the hud value.
struct Step {
    Kind kind;
    const char* a;
    const char* b;
    bool flag;
};

// One numbered entry of a script: its steps are `<item>.1`, `<item>.2`, ...
struct Item {
    const Step* steps;
    size_t count;
};

constexpr const char* kSetHud = "mv user " MVD_CLIENT " permission set multivoid.dev.local.hud true";
constexpr const char* kDoneSetHud = "Done: /mv user " MVD_CLIENT " permission set multivoid.dev.local.hud true";
constexpr const char* kNotLoad = "The permission files do not load:";
constexpr const char* kStaffInfo = "mv group staff info";
constexpr const char* kStaffNoWeight = "staff: no weight";
constexpr const char* kBadGroup = "groups\\Bad Name.json";
constexpr const char* kBadUser = "users\\Bad Name.json";

// The host's script (the numbers are the items the verdicts name).
constexpr Step kH1[] = {{Kind::Send, kSetHud, kNotLoad, false}};
constexpr Step kH2[] = {{Kind::Send, "mv listgroups",
                         "The permission files did not load at the host start: fix them, then /mv reload.", false}};
constexpr Step kH3[] = {{Kind::RemoveFile, kBadGroup, nullptr, false},
                        {Kind::Send, "mv reload", "Reloaded: 1 groups, 2 users.", false}};
constexpr Step kH4[] = {{Kind::Send, kSetHud, kDoneSetHud, false}};
constexpr Step kH5[] = {{Kind::Send, kSetHud, "No change.", false}};
constexpr Step kH5Red[] = {{Kind::Send, kSetHud, kDoneSetHud, false}};
constexpr Step kH6[] = {{Kind::Send, "mv listgroups", "default", false},
                        {Kind::Send, kStaffInfo, kStaffNoWeight, false},
                        {Kind::WaitNotice, "mvdrill.delegate", nullptr, false}};
constexpr Step kH7[] = {{Kind::WriteFile, kBadUser, "{}", false},
                        {Kind::Send, kSetHud, kNotLoad, false},
                        {Kind::Send, kStaffInfo, kStaffNoWeight, false},
                        {Kind::RemoveFile, kBadUser, nullptr, false}};
constexpr Step kH8[] = {{Kind::Send, "mv deletegroup staff",
                         "That change would leave the permission files broken:", false}};
constexpr Step kH9[] = {{Kind::WriteFile, "groups\\hand.json", "{\"permissions\": [\"hand.node\"]}", false},
                        {Kind::Send, "mv group hand permission set hand.second true",
                         "Done: /mv group hand permission set hand.second true", false}};
constexpr Step kH10[] = {{Kind::Send, "mv creategroup mvtmp", "Done: /mv creategroup mvtmp", false},
                         {Kind::ExpectFile, "groups\\mvtmp.json", nullptr, true},
                         {Kind::Send, "mv deletegroup mvtmp", "Done: /mv deletegroup mvtmp", false},
                         {Kind::ExpectFile, "groups\\mvtmp.json", nullptr, false}};
constexpr Step kH11[] = {{Kind::Send, "mv user " MVD_OWNER " permission unset mvdrill.only",
                          "Done: /mv user " MVD_OWNER " permission unset mvdrill.only", false},
                         {Kind::ExpectFile, "users\\" MVD_OWNER ".json", nullptr, false}};

#define MVD_ITEM(arr) Item{arr, std::size(arr)}
constexpr Item kHostItems[] = {MVD_ITEM(kH1), MVD_ITEM(kH2), MVD_ITEM(kH3), MVD_ITEM(kH4),
                               MVD_ITEM(kH5), MVD_ITEM(kH6), MVD_ITEM(kH7), MVD_ITEM(kH8),
                               MVD_ITEM(kH9), MVD_ITEM(kH10), MVD_ITEM(kH11)};
constexpr size_t kRedItem = 4;  // the host's item 5

// The client's script: one item per entry.
constexpr Step kC1[] = {{Kind::WaitBit, nullptr, nullptr, true}};
constexpr Step kC2[] = {{Kind::WaitNotice, "multivoid.dev.local.hud", nullptr, false}};
constexpr Step kC3[] = {{Kind::Send, "mv user " MVD_HOST " permission set multivoid.kick false",
                         "That would take multivoid.kick away from the host.", false}};
constexpr Step kC4[] = {{Kind::Send, "mv user " MVD_HOST " permission set mvdrill.delegate true",
                         "Done: /mv user " MVD_HOST " permission set mvdrill.delegate true", false}};
constexpr Step kC5[] = {{Kind::Send, "mv group staff permission set x.y true",
                         "You do not have permission for /mv group permission set "
                         "(multivoid.mv.group.permission.set).", false}};
constexpr Item kClientItems[] = {MVD_ITEM(kC1), MVD_ITEM(kC2), MVD_ITEM(kC3), MVD_ITEM(kC4), MVD_ITEM(kC5)};

enum class Phase : uint8_t { Init, Waiting, Running, Done };

Phase g_phase = Phase::Init;
bool g_isHost = false;
bool g_installed = false;  // this drill holds the reply observer
char g_role[12] = "host";

// Every delivered line of this peer, in arrival order: notices (`[mv] ...`) and replies.
std::vector<std::string> g_notices;
std::vector<std::string> g_replies;

Item g_hostRun[std::size(kHostItems)];
const Item* g_items = nullptr;
size_t g_itemCount = 0;
size_t g_item = 0;     // the item being run
size_t g_k = 0;        // the step within it
bool g_begun = false;  // the step has started (a Send is submitted)
size_t g_mark = 0;     // the replies list's size when the Send was submitted
std::chrono::steady_clock::time_point g_stepStart{};
std::filesystem::path g_dir;  // the host's permissions folder

bool StartsWith(std::string_view s, std::string_view prefix) {
    return s.substr(0, prefix.size()) == prefix;
}

void Observe(std::string_view line) {
    (StartsWith(line, "[mv] ") ? g_notices : g_replies).emplace_back(line);
}

void Finish() {
    g_phase = Phase::Done;
    if (g_installed) CS::SetReplyObserver(nullptr);
    g_installed = false;
}

void SetRole(bool host) {
    g_isHost = host;
    const uint8_t slot = coop::players::Registry::Get().LocalPeerId();
    if (host) std::snprintf(g_role, sizeof(g_role), "host");
    else if (slot < coop::players::kMaxPeers) std::snprintf(g_role, sizeof(g_role), "c%u", static_cast<unsigned>(slot));
    else std::snprintf(g_role, sizeof(g_role), "client");
}

void Pass() {
    UE_LOGI("[MV-DRILL] %s PASS", g_role);
    Finish();
}

// A FAIL line carries `step <item>.<k>:` (1-based) for a step, or the bare text.
void FailStep(const char* what) {
    UE_LOGE("[MV-DRILL] %s FAIL: step %zu.%zu: %s", g_role, g_item + 1, g_k + 1, what);
    Finish();
}

void FailPlain(const std::string& what) {
    UE_LOGE("[MV-DRILL] %s FAIL: %s", g_role, what.c_str());
    Finish();
}

std::filesystem::path PathOf(const char* rel) {
    std::wstring w;
    for (const char* c = rel; *c; ++c) w.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*c)));
    return g_dir / w;
}

bool Expired(std::chrono::seconds budget) {
    return std::chrono::steady_clock::now() - g_stepStart >= budget;
}

void BeginStep() {
    g_begun = true;
    g_stepStart = std::chrono::steady_clock::now();
}

// One step. True when it is done and the next may run now; false when it still waits or the drill
// ended (the phase says which).
bool RunStep(const Step& st) {
    switch (st.kind) {
    case Kind::Send: {
        if (!g_begun) {
            BeginStep();
            g_mark = g_replies.size();
            CS::Submit(st.a);
            return false;
        }
        if (g_replies.size() > g_mark) {
            const std::string& got = g_replies[g_mark];
            if (got == st.b) return true;
            char what[512];
            std::snprintf(what, sizeof(what), "expected '%s' got '%s'", st.b, got.c_str());
            FailStep(what);
            return false;
        }
        if (Expired(kSendBudget)) FailStep("no answer in 30 s");
        return false;
    }
    case Kind::WaitNotice: {
        if (!g_begun) BeginStep();
        for (const std::string& n : g_notices)
            if (n.find(st.a) != std::string::npos) return true;
        if (Expired(kWaitBudget)) FailStep("no answer in 60 s");
        return false;
    }
    case Kind::WaitBit: {
        if (!g_begun) BeginStep();
        if (LG::Has(G::Projected::Hud) == st.flag) return true;
        if (Expired(kWaitBudget)) FailStep("no answer in 60 s");
        return false;
    }
    case Kind::WriteFile: {
        std::ofstream out(PathOf(st.a), std::ios::binary | std::ios::trunc);
        if (out) out << st.b;
        if (!out || !out.good()) {
            FailStep((std::string("could not write ") + st.a).c_str());
            return false;
        }
        return true;
    }
    case Kind::RemoveFile: {
        std::error_code ec;
        std::filesystem::remove(PathOf(st.a), ec);
        if (ec) {
            FailStep((std::string("could not remove ") + st.a).c_str());
            return false;
        }
        return true;
    }
    case Kind::ExpectFile: {
        std::error_code ec;
        const bool present = std::filesystem::exists(PathOf(st.a), ec);
        if (present == st.flag && !ec) return true;
        FailStep((std::string(st.a) + " is " + (present ? "present" : "absent")).c_str());
        return false;
    }
    }
    return false;
}

void RunScript() {
    while (g_phase == Phase::Running) {
        if (g_item >= g_itemCount) { Pass(); return; }
        const Item& item = g_items[g_item];
        if (g_k >= item.count) {
            ++g_item;
            g_k = 0;
            g_begun = false;
            continue;
        }
        if (!RunStep(item.steps[g_k])) return;
        ++g_k;
        g_begun = false;
    }
}

void StartScript() {
    g_item = 0;
    g_k = 0;
    g_begun = false;
    if (g_isHost) {
        std::copy(std::begin(kHostItems), std::end(kHostItems), g_hostRun);
        if (ArmNow() == Arm::Red) g_hostRun[kRedItem] = MVD_ITEM(kH5Red);
        g_items = g_hostRun;
        g_itemCount = std::size(g_hostRun);
    } else {
        g_items = kClientItems;
        g_itemCount = std::size(kClientItems);
    }
    g_phase = Phase::Running;
    UE_LOGI("[MV-DRILL] %s script start", g_role);
}

// The host starts when a client is seated, proved and world-ready, and the rig's ids are the ones
// the seeded store names.
void WaitHost(coop::net::Session* s) {
    coop::roster::Snapshot r;
    coop::roster::GetSnapshot(r);
    int slot = -1;
    for (int i = 0; i < r.count; ++i)
        if (r.rows[i].slot >= 1 && r.rows[i].connected) { slot = r.rows[i].slot; break; }
    if (slot < 0) return;
    const std::string id = s->ProvedGuidForSlotWithToken(slot, coop::roster_ledger::Get(slot).bornGeneration);
    if (id.empty()) return;
    if (coop::net::peer_identity::LocalGuid() != kHostId) {
        FailPlain("the host's id is not the rig's");
        return;
    }
    if (id != kClientId) {
        FailPlain("the client's id is " + id + ", not the rig's");
        return;
    }
    if (!s->IsSlotWorldReady(slot)) return;
    const std::string server = coop::config::ResolveString(::coop::config_registry::rows::net_server);
    if (server.empty()) {
        FailPlain("net.server is empty");
        return;
    }
    g_dir = std::filesystem::path(coop::server_profile::ServersDir()) /
            std::wstring(server.begin(), server.end()) / L"permissions";
    StartScript();
}

bool OtherDrillOn() {
    return coop::config::ResolveEnum(::coop::config_registry::rows::command_drill) != "off" ||
           coop::config::ResolveEnum(::coop::config_registry::rows::settings_drill) != "off";
}

void ClearLists() {
    g_notices.clear();
    g_replies.clear();
}

#undef MVD_ITEM
#undef MVD_OWNER
#undef MVD_CLIENT
#undef MVD_HOST

}  // namespace

void Tick(coop::net::Session* session) {
    if (ArmNow() == Arm::Off || !session || !session->running()) return;
    switch (g_phase) {
    case Phase::Init:
        SetRole(session->role() == coop::net::Role::Host);
        if (OtherDrillOn()) {
            FailPlain("another drill holds the reply observer");
            return;
        }
        ClearLists();
        CS::SetReplyObserver(&Observe);
        g_installed = true;
        g_phase = Phase::Waiting;
        return;
    case Phase::Waiting:
        if (g_isHost) {
            WaitHost(session);
            return;
        }
        if (!coop::dev::command_drill::ClientReady(session)) return;
        SetRole(false);
        StartScript();
        return;
    case Phase::Running:
        RunScript();
        return;
    case Phase::Done:
        return;
    }
}

void OnDisconnect() {
    if (g_installed) CS::SetReplyObserver(nullptr);
    g_installed = false;
    g_phase = Phase::Init;
    ClearLists();
    g_item = 0;
    g_k = 0;
    g_begun = false;
}

}  // namespace coop::dev::mv_drill
