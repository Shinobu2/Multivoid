// coop/permissions/permission_host.cpp -- see coop/permissions/permission_host.h.

#include "coop/permissions/permission_host.h"

#include "coop/moderation/moderation.h"
#include "coop/net/peer_identity.h"
#include "coop/permissions/permission_files.h"
#include "coop/permissions/resolution.h"

#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"

#include <atomic>
#include <ctime>
#include <filesystem>
#include <fstream>
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
    // The store had a problem: `model` is empty and every check but the console's is refused.
    bool broken = false;
    // The server folder OnHostStart was given; empty when EnsureHosted failed.
    std::wstring serverDir;
};

// The hand-off slot: the pointer under the mutex, the flag set with release after it.
std::mutex g_handoffMutex;
std::unique_ptr<Published> g_handoff;
std::atomic<bool> g_published{false};

// The game thread's live model. Until a host start publishes one, the empty store answers (the
// `default` group only), so a check is never made over nothing. It is replaced by an adoption, an
// edit or a reload.
Model g_model;
ContextSet g_subject;
bool g_broken = false;
std::wstring g_serverDir;
std::optional<Checker> g_checker;
// Counted at each replacement of the live model; Revision() hands it out.
uint64_t g_revision = 0;

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
            g_serverDir = std::move(adopted->serverDir);
            g_checker.emplace(g_model);  // its cache belongs to the old model
            ++g_revision;
        }
    }
    if (!g_checker) g_checker.emplace(g_model);
    return *g_checker;
}

// Makes `m` the live model: the checker's cache belongs to the old one, the grants tick re-projects
// at the new revision, and a store that now loads is not broken.
void ReplaceLive(Model m) {
    g_model = std::move(m);
    g_checker.emplace(g_model);
    ++g_revision;
    g_broken = false;
}

// The store's folder under the server folder; the caller has checked that g_serverDir is set.
std::filesystem::path StoreDir() { return std::filesystem::path(g_serverDir) / L"permissions"; }

ApplyResult Refused(std::vector<std::string> lines) {
    UE_LOGI("permissions: refused: %s", lines.front().c_str());
    ApplyResult out;
    out.outcome = ApplyOutcome::Refused;
    out.replies = std::move(lines);
    return out;
}

// A header line, then the loader's problems as the host answers them.
std::vector<std::string> WithProblems(const char* header, const std::vector<std::string>& problems) {
    std::vector<std::string> lines{header};
    for (std::string& line : ProblemLines(problems)) lines.push_back(std::move(line));
    return lines;
}

// One line to the action log. The change stands when the append fails: the log is a record a person
// reads, and a torn last line is what a crash mid-append costs.
void AppendActionLog(const std::filesystem::path& dir, const Action& action) {
    const std::string line = ActionJson(action, NowSeconds()) + "\n";
    bool appended = false;
    {
        std::ofstream f(dir / L"actions.jsonl", std::ios::binary | std::ios::app);
        if (f) {
            f.write(line.data(), static_cast<std::streamsize>(line.size()));
            f.flush();
            appended = static_cast<bool>(f);
        }
    }
    if (!appended) UE_LOGW("permissions: could not append to actions.jsonl -- the change stands");
}

}  // namespace

int64_t NowSeconds() { return static_cast<int64_t>(::time(nullptr)); }

uint64_t Revision() {
    UE_ASSERT_GAME_THREAD("permission_host::Revision");
    LiveChecker();  // adopts a pending publish, as the first check would
    return g_revision;
}

int64_t NextExpiry(const std::string& playerId) {
    UE_ASSERT_GAME_THREAD("permission_host::NextExpiry");
    Checker& checker = LiveChecker();
    if (g_broken) return 0;  // nothing is loaded, so nothing expires
    return checker.Get(playerId, g_subject, NowSeconds())->validUntil;
}

void OnHostStart(const std::wstring& serverDir, std::string serverId) {
    auto fresh = std::make_unique<Published>();
    fresh->serverDir = serverDir;
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

ApplyResult Apply(const HolderKey& key, const std::function<bool(Model& copy, std::string* why)>& change,
                  bool callerIsOwner, const std::vector<std::string>& nodes, const Action& action) {
    UE_ASSERT_GAME_THREAD("permission_host::Apply");
    ApplyResult out;
    if (!coop::moderation::HostedSessionRunning()) {
        out.outcome = ApplyOutcome::NoSession;
        return out;
    }
    LiveChecker();  // adopts a pending publish: OnHostStart publishes only before the session starts
    if (g_serverDir.empty()) return Refused({"The server folder is not available."});
    const std::filesystem::path dir = StoreDir();

    // The disk is read at every edit, so a hand edit to any file is part of what is planned over
    // and published. Only an edit saved by hand to the SAME holder's file between this read and the
    // write below is lost: the milliseconds of one command, accepted.
    std::vector<HolderText> texts;
    LoadReport read;
    if (!ReadStoreTexts(dir, &texts, &read)) return Refused({"Could not read the permission files."});
    if (!read.problems.empty()) return Refused(WithProblems("The permission files do not load:", read.problems));

    EditPlan plan = PlanEdit(texts, key, change, callerIsOwner, coop::net::peer_identity::LocalGuid(), g_subject,
                             NowSeconds(), nodes);
    switch (plan.result) {
        case EditResult::NoChange:
            // `candidate` is what the disk holds: a hand edit to another file goes live here too.
            ReplaceLive(std::move(plan.candidate));
            out.outcome = ApplyOutcome::NoChange;
            out.replies.push_back("No change.");
            return out;
        case EditResult::Refused:
            if (plan.refusedBy == RefusedBy::Store)
                return Refused(WithProblems("The permission files do not load:", plan.problems));
            if (plan.refusedBy == RefusedBy::Candidate)
                return Refused(WithProblems("That change would leave the permission files broken:", plan.problems));
            return Refused({plan.why});
        case EditResult::OwnerLoses:
            return Refused({"That would take " + plan.why + " away from the host."});
        case EditResult::Changed:
            break;
    }

    // Written before it is published: a failed write leaves the live model as the disk still is.
    const atomic_file::Result wrote = WriteHolderFile(dir, key.kind == HolderKind::Group, key.name,
                                                      plan.deleteFile ? nullptr : &plan.text);
    if (!wrote.ok()) {
        UE_LOGW("permissions: could not write the %s file (step %d, error %lu)",
                key.kind == HolderKind::Group ? "group" : "user", static_cast<int>(wrote.failedAt), wrote.error);
        return Refused({"Could not write the permission file."});
    }
    ReplaceLive(std::move(plan.candidate));
    UE_LOGI("permissions: %s (by %.8s)", action.description.c_str(), action.sourceId.c_str());
    AppendActionLog(dir, action);
    out.outcome = ApplyOutcome::Changed;
    out.replies.push_back("Done: " + action.description);
    return out;
}

std::vector<std::string> Reload(const std::string& actorId) {
    UE_ASSERT_GAME_THREAD("permission_host::Reload");
    if (!coop::moderation::HostedSessionRunning()) return {};
    LiveChecker();
    const auto refuse = [&](std::vector<std::string> lines, const std::string& first) {
        UE_LOGI("permissions: reload by %.8s refused: %s", actorId.c_str(), first.c_str());
        return lines;
    };
    if (g_serverDir.empty()) {
        return refuse({"The server folder is not available."}, "no server folder");
    }
    const std::filesystem::path dir = StoreDir();
    std::vector<HolderText> texts;
    LoadReport report;
    if (!ReadStoreTexts(dir, &texts, &report)) {
        return refuse({"Could not read the permission files."}, "could not read the folder");
    }
    Model fresh;
    LoadReport loaded = LoadHolders(texts, fresh);
    for (std::string& problem : loaded.problems) report.problems.push_back(std::move(problem));
    if (!ShouldLoad(report))
        return refuse(WithProblems("The permission files do not load:", report.problems), report.problems.front());
    ReplaceLive(std::move(fresh));
    UE_LOGI("permissions: reloaded by %.8s (%d groups, %d users)", actorId.c_str(), loaded.groups, loaded.users);
    return {"Reloaded: " + std::to_string(loaded.groups) + " groups, " + std::to_string(loaded.users) + " users."};
}

const Model& Live() {
    UE_ASSERT_GAME_THREAD("permission_host::Live");
    LiveChecker();
    return g_model;
}

bool StoreBroken() {
    UE_ASSERT_GAME_THREAD("permission_host::StoreBroken");
    LiveChecker();
    return g_broken;
}

}  // namespace coop::permissions::host
