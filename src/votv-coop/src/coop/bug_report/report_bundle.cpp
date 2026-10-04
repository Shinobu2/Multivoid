// coop/bug_report/report_bundle.cpp -- the request, the status and the worker; see
// coop/bug_report/report_bundle.h. The files and the chunked reader are report_files.cpp.
//
// Thread map: Request, GetStatus and ListEntries run on any thread; the capture closure on the
// game thread (the role, slot, player count, game mode and ids live there); the worker on its own
// detached thread, touching no UObject. The order at every worker exit: publish the Status, then
// clear the in-flight flag, so a Request that sees the flag clear sees the final status.

#include "coop/bug_report/report_bundle.h"

#include "coop/config/config_report.h"
#include "coop/net/peer_identity.h"
#include "coop/net/protocol.h"
#include "coop/player/players_registry.h"
#include "coop/player/roster.h"
#include "coop/session/shutdown.h"
#include "coop/version.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/paths.h"
#include "ue_wrap/world/game_rules.h"

#include "miniz.h"
#include <nlohmann/json.hpp>

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <system_error>
#include <thread>

namespace coop::bug_report {
namespace {

namespace fs = std::filesystem;
using Json = nlohmann::json;

std::mutex g_statusMu;
Status g_status;
std::atomic<bool> g_inFlight{false};

void Publish(Status s) {
    std::lock_guard<std::mutex> lk(g_statusMu);
    g_status = std::move(s);
}

Status FailedStatus(const char* why) {
    Status s;
    s.phase = Phase::Failed;
    s.error = why;
    return s;
}

// What the game thread knows, read before the worker starts.
struct Capture {
    std::string role = "solo";  // host | client | solo
    int slot = -1;              // -1 = none (solo, or not yet assigned)
    int players = 0;
    std::string gameMode, selfId, selfKey;
    std::string utc, stamp;     // ISO 8601 Z, and yyyymmdd-hhmmss for the zip's name
};

Capture CaptureOnGameThread() {
    Capture c;
    coop::roster::Snapshot snap;
    coop::roster::GetSnapshot(snap);
    if (coop::roster::LocalIsHost()) c.role = "host";
    else if (snap.inSession) c.role = "client";
    if (c.role != "solo") {
        const uint8_t id = coop::players::Registry::Get().LocalPeerId();
        if (id != coop::players::kPeerIdUnknown) c.slot = id;
    }
    c.players = snap.count;
    ue_wrap::game_rules::Snapshot rules;
    if (ue_wrap::game_rules::ReadLocal(rules)) c.gameMode = rules.gamemodeName;
    c.selfId = coop::net::peer_identity::LocalGuid();
    c.selfKey = coop::net::peer_identity::LocalIdentityString();
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
    ::gmtime_s(&tm, &now);
    char buf[32] = {};
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    c.utc = buf;
    std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &tm);
    c.stamp = buf;
    return c;
}

std::wstring Widen(const char* ascii) { return std::wstring(ascii, ascii + std::strlen(ascii)); }

// _wfopen as the tree spells it (_wfopen_s); null when it fails.
FILE* OpenFile(const std::wstring& path, const wchar_t* mode) {
    FILE* f = nullptr;
    return ::_wfopen_s(&f, path.c_str(), mode) == 0 ? f : nullptr;
}

// Removes what a run leaves in the work folder, and the half-written zip unless it was renamed.
// Declared before anything that holds a file open, so it runs after those close.
struct Cleanup {
    std::wstring tmpDir, part;
    ~Cleanup() {
        std::error_code ec;
        fs::remove(fs::path(part), ec);
        for (fs::directory_iterator it(fs::path(tmpDir), ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code rm;
            fs::remove(it->path(), rm);
        }
    }
};

// A crashed run's leftovers: every file in the work folder and every half-written zip.
void RemoveLeftovers(const fs::path& reports, const fs::path& tmpDir) {
    std::error_code ec;
    for (fs::directory_iterator it(tmpDir, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code rm;
        fs::remove(it->path(), rm);
    }
    ec.clear();
    for (fs::directory_iterator it(reports, ec), end; !ec && it != end; it.increment(ec)) {
        const std::wstring name = it->path().filename().wstring();
        if (name.size() > 9 && name.compare(name.size() - 9, 9, L".zip.part") == 0) {
            std::error_code rm;
            fs::remove(it->path(), rm);
        }
    }
}

// The zip being written, on a FILE the writer owns; closed on every path out.
class Zip {
public:
    Zip() { mz_zip_zero_struct(&zip_); }
    ~Zip() { Close(); }
    Zip(const Zip&) = delete;
    Zip& operator=(const Zip&) = delete;

    bool Open(const std::wstring& part) {
        file_ = OpenFile(part, L"wb");
        if (!file_) return false;
        if (!mz_zip_writer_init_cfile(&zip_, file_, 0)) {
            Close();
            return false;
        }
        open_ = true;
        return true;
    }
    bool AddFile(const char* name, FILE* src, uint64_t size) {
        return mz_zip_writer_add_cfile(&zip_, name, src, size, nullptr, nullptr, 0, MZ_DEFAULT_LEVEL,
                                       nullptr, 0, nullptr, 0) != 0;
    }
    bool AddMem(const char* name, const std::string& data) {
        return mz_zip_writer_add_mem(&zip_, name, data.data(), data.size(), MZ_DEFAULT_LEVEL) != 0;
    }
    // The central directory written, the writer ended, the file closed.
    bool Finish() {
        bool ok = open_ && mz_zip_writer_finalize_archive(&zip_) != 0;
        Close();
        return ok;
    }

private:
    void Close() {
        if (open_) {
            mz_zip_writer_end(&zip_);
            open_ = false;
        }
        if (file_) {
            std::fclose(file_);
            file_ = nullptr;
        }
    }
    mz_zip_archive zip_;
    FILE* file_ = nullptr;
    bool open_ = false;
};

enum class AddResult { Ok, ReadFailed, WriteFailed };
using Feed = std::function<bool(const detail::LineFn&)>;

// One entry: the lines `feed` produces go through the Redactor into a temporary file, which
// miniz deflates into the zip in its own buffered loop; the temporary file is deleted.
AddResult AddRedacted(Zip& zip, Redactor& redactor, const std::wstring& tmpDir, const char* name,
                      const Feed& feed) {
    const std::wstring tmp = tmpDir + L"\\" + Widen(name);
    FILE* out = OpenFile(tmp, L"wb");
    if (!out) return AddResult::WriteFailed;
    bool writeOk = true;
    uint64_t written = 0;
    const bool read = feed([&](std::string_view line) {
        if (!writeOk) return;
        const std::string redacted = redactor.Apply(line);
        if (!redacted.empty() && std::fwrite(redacted.data(), 1, redacted.size(), out) != redacted.size())
            writeOk = false;
        else if (std::fputc('\n', out) == EOF)
            writeOk = false;
        else
            written += redacted.size() + 1;
    });
    const bool closed = std::fclose(out) == 0;
    AddResult result = AddResult::Ok;
    if (!read) {
        result = AddResult::ReadFailed;
    } else if (!writeOk || !closed) {
        result = AddResult::WriteFailed;
    } else if (FILE* in = OpenFile(tmp, L"rb")) {
        if (!zip.AddFile(name, in, written)) result = AddResult::WriteFailed;
        std::fclose(in);
    } else {
        result = AddResult::WriteFailed;
    }
    std::error_code ec;
    fs::remove(fs::path(tmp), ec);
    return result;
}

// A text's lines, each without its '\n'; a last line with no '\n' counts.
bool FeedText(const std::string& text, const detail::LineFn& fn) {
    std::string_view v(text);
    while (!v.empty()) {
        const size_t nl = v.find('\n');
        if (nl == std::string_view::npos) {
            fn(v);
            break;
        }
        fn(v.substr(0, nl));
        v.remove_prefix(nl + 1);
    }
    return true;
}

detail::ReadPlan PlanFor(const Entry& e) {
    detail::ReadPlan plan;
    plan.tailOnly = e.tailOnly;
    plan.completeLinesOnly = std::strcmp(e.name, "multivoid.log") == 0;  // the log still being written
    return plan;
}

Status Build(const Form& form, const Capture& cap) {
    constexpr const char* kWriteFailed = "Could not write the report file.";
    constexpr const char* kClosing = "The game is closing.";
    const std::wstring exeDir = ue_wrap::paths::ExeDir();
    if (exeDir.empty()) return FailedStatus(kWriteFailed);
    const std::wstring reports = exeDir + L"\\multivoid_reports";
    const std::wstring tmpDir = reports + L"\\.tmp";
    const std::wstring zipPath = reports + L"\\report-" + Widen(cap.stamp.c_str()) + L".zip";
    Cleanup cleanup{tmpDir, zipPath + L".part"};

    std::error_code ec;
    fs::create_directories(fs::path(tmpDir), ec);
    if (ec) return FailedStatus(kWriteFailed);
    RemoveLeftovers(fs::path(reports), fs::path(tmpDir));

    RedactContext ctx = ReadThisMachine(cap.selfId, cap.selfKey);
    std::string iniText;
    bool iniOk = coop::config::IniTextForReport(iniText);
    std::vector<Entry> entries = ListEntries();
    if (!entries[0].leftOut.empty()) return FailedStatus("Could not find the log.");
    Redactor redactor(std::move(ctx));
    const std::string reportText = ReportText(form);

    // Pass 1: every player id of every entry, and the bytes of each that pass 2 reads.
    FeedText(reportText, [&](std::string_view line) { redactor.Learn(line); });
    std::vector<detail::Span> spans(entries.size());
    for (size_t i = 0; i < entries.size(); ++i) {
        if (!entries[i].leftOut.empty()) continue;
        if (coop::shutdown::IsShuttingDown()) return FailedStatus(kClosing);
        if (!detail::StreamLines(entries[i].path, PlanFor(entries[i]), false, spans[i],
                                 [&](std::string_view line) { redactor.Learn(line); })) {
            if (i == 0) return FailedStatus("Could not read the log.");
            entries[i].leftOut = "could not be read";
        }
    }
    if (iniOk) FeedText(iniText, [&](std::string_view line) { redactor.Learn(line); });

    // Pass 2, in zip order.
    Zip zip;
    if (!zip.Open(cleanup.part)) return FailedStatus(kWriteFailed);
    const auto addText = [&](const char* name, const std::string& text) {
        return AddRedacted(zip, redactor, tmpDir, name,
                           [&](const detail::LineFn& fn) { return FeedText(text, fn); });
    };
    const auto addEntry = [&](size_t i) -> const char* {
        Entry& e = entries[i];
        if (!e.leftOut.empty()) return nullptr;
        if (coop::shutdown::IsShuttingDown()) return kClosing;
        const detail::ReadPlan plan = PlanFor(e);
        const AddResult r = AddRedacted(zip, redactor, tmpDir, e.name, [&](const detail::LineFn& fn) {
            return detail::StreamLines(e.path, plan, true, spans[i], fn);
        });
        if (r == AddResult::WriteFailed) return kWriteFailed;
        if (r == AddResult::ReadFailed) {
            if (i == 0) return "Could not read the log.";
            e.leftOut = "could not be read";
        }
        return nullptr;
    };

    if (addText(kMadeEntries[0], reportText) != AddResult::Ok) return FailedStatus(kWriteFailed);
    for (const size_t i : {size_t(0), size_t(1), size_t(2)})
        if (const char* why = addEntry(i)) return FailedStatus(why);
    if (iniOk) {
        if (coop::shutdown::IsShuttingDown()) return FailedStatus(kClosing);
        if (addText(kMadeEntries[1], iniText) != AddResult::Ok) return FailedStatus(kWriteFailed);
    }
    if (const char* why = addEntry(3)) return FailedStatus(why);

    Json meta;
    meta["format"] = 1;
    meta["game_target"] = coop::version::kGameTarget;
    meta["build"] = coop::net::kProtocolVersion;
    meta["role"] = cap.role;
    meta["slot"] = cap.slot < 0 ? Json(nullptr) : Json(cap.slot);
    meta["players"] = cap.players;
    meta["game_mode"] = cap.gameMode;
    meta["utc"] = cap.utc;
    meta["reporter"] = "player#self";
    const RedactCounts& counts = redactor.Counts();
    meta["redactions"] = {{"profile", counts.profile}, {"players", counts.players},
                          {"keys", counts.keys}, {"addresses", counts.addresses}};
    Json leftOut = Json::object();
    for (const Entry& e : entries)
        if (!e.leftOut.empty()) leftOut[e.name] = e.leftOut;
    if (!iniOk) leftOut[kMadeEntries[1]] = "could not be read";
    meta["left_out"] = leftOut;
    if (!zip.AddMem(kMadeEntries[2], meta.dump(2, ' ', false, Json::error_handler_t::replace)))
        return FailedStatus(kWriteFailed);
    if (!zip.Finish()) return FailedStatus(kWriteFailed);
    if (!::MoveFileExW(cleanup.part.c_str(), zipPath.c_str(), 0)) return FailedStatus(kWriteFailed);

    Status done;
    done.phase = Phase::Done;
    done.zipPath = zipPath;
    done.zipBytes = fs::file_size(fs::path(zipPath), ec);
    if (ec) done.zipBytes = 0;
    done.counts = counts;
    return done;
}

void FailToStart() {
    Publish(FailedStatus("Could not start the report."));
    g_inFlight.store(false, std::memory_order_release);
}

// The worker thread's whole body. An exception escaping a detached thread is std::terminate, so
// nothing leaves it; the flag is cleared outside the inner try, as session_manager's workers do.
void WorkerMain(const Form& form, const Capture& cap) {
    try {
        Status result;
        try {
            result = Build(form, cap);
        } catch (...) {
            result = FailedStatus("The report could not be made.");
        }
        Publish(std::move(result));
    } catch (...) {
        // Publish itself failed: the status stays Building and the flag below is all that can be done.
    }
    g_inFlight.store(false, std::memory_order_release);
}

}  // namespace

bool Request(Form form) {
    if (ValidateForm(form) != nullptr) return false;
    if (g_inFlight.exchange(true, std::memory_order_acq_rel)) return false;
    try {
        Status building;
        building.phase = Phase::Building;
        Publish(std::move(building));
        ue_wrap::game_thread::Post([form = std::move(form)]() mutable {
            try {
                Capture cap = CaptureOnGameThread();
                ue_wrap::log::Flush();
                std::thread([form = std::move(form), cap = std::move(cap)] { WorkerMain(form, cap); })
                    .detach();
            } catch (...) {
                FailToStart();
            }
        });
    } catch (...) {
        FailToStart();
    }
    return true;
}

Status GetStatus() {
    std::lock_guard<std::mutex> lk(g_statusMu);
    return g_status;
}

}  // namespace coop::bug_report
