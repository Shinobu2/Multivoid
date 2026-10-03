// coop/permissions/permission_host.cpp -- see coop/permissions/permission_host.h.

#include "coop/permissions/permission_host.h"

#include "coop/permissions/permission_files.h"
#include "coop/permissions/resolution.h"

#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"

#include <atomic>
#include <ctime>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

namespace coop::permissions::host {
namespace {

// What OnHostStart builds and the game thread adopts.
struct Published {
    Model model;
    ContextSet subject;
    bool broken = false;  // the store had a problem: `model` is empty and every check is refused
};

// The hand-off slot: the pointer under the mutex, the flag set with release after it.
std::mutex g_handoffMutex;
std::unique_ptr<Published> g_handoff;
std::atomic<bool> g_published{false};

// The game thread's live model. Until a host start publishes one, the empty store answers (the
// `default` group only), so a check is never made over nothing.
Model g_model;
ContextSet g_subject;
bool g_broken = false;
std::optional<Checker> g_checker;

Checker& LiveChecker() {
    if (g_published.exchange(false, std::memory_order_acq_rel)) {
        std::unique_ptr<Published> adopted;
        {
            std::lock_guard<std::mutex> lock(g_handoffMutex);
            adopted.swap(g_handoff);
        }
        if (adopted) {
            g_model = std::move(adopted->model);
            g_subject = std::move(adopted->subject);
            g_broken = adopted->broken;
            g_checker.emplace(g_model);  // its cache belongs to the old model
        }
    }
    if (!g_checker) g_checker.emplace(g_model);
    return *g_checker;
}

int64_t NowSeconds() { return static_cast<int64_t>(::time(nullptr)); }

}  // namespace

void OnHostStart(const std::wstring& serverDir, std::string serverId) {
    auto fresh = std::make_unique<Published>();
    fresh->subject.Add("server", serverId);
    fresh->subject.Add("mode", "listen");
    if (serverDir.empty()) {
        UE_LOGI("permissions: no server folder this session; only the defaults apply");
    } else {
        const std::filesystem::path dir = std::filesystem::path(serverDir) / L"permissions";
        const LoadReport report = LoadStore(dir, fresh->model);
        for (const std::string& problem : report.problems) UE_LOGW("permissions: %s", problem.c_str());
        if (ShouldLoad(report)) {
            UE_LOGI("permissions: loaded %d group(s), %d user(s) from %ls", report.groups, report.users,
                    dir.c_str());
        } else {
            fresh->model = Model();
            fresh->broken = true;
            UE_LOGW("permissions: the store has %d problem(s); none of it is loaded -- every command but the "
                    "host's is refused until it is fixed",
                    static_cast<int>(report.problems.size()));
        }
    }
    {
        std::lock_guard<std::mutex> lock(g_handoffMutex);
        g_handoff = std::move(fresh);
    }
    g_published.store(true, std::memory_order_release);
}

bool Allows(std::string_view playerId, std::string_view node, bool defaultGranted, bool owner) {
    UE_ASSERT_GAME_THREAD("permission_host::Allows");
    Checker& checker = LiveChecker();
    if (g_broken) return owner;  // the console keeps everything; no other caller, default-granted nodes included
    const std::shared_ptr<const Resolved> r = checker.Get(playerId, g_subject, NowSeconds());
    switch (Evaluate(*r, node, owner).value) {
        case Tristate::True: return true;
        case Tristate::False: return false;
        case Tristate::Undefined: break;
    }
    return defaultGranted;
}

bool HoldsExplicitly(std::string_view playerId, std::string_view node) {
    UE_ASSERT_GAME_THREAD("permission_host::HoldsExplicitly");
    Checker& checker = LiveChecker();
    if (g_broken) return false;
    const std::shared_ptr<const Resolved> r = checker.Get(playerId, g_subject, NowSeconds());
    return IsSetExplicitly(*r, node);
}

}  // namespace coop::permissions::host
