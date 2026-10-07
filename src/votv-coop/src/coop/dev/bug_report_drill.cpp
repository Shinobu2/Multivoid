// coop/dev/bug_report_drill.cpp -- see coop/dev/bug_report_drill.h.

#include "coop/dev/bug_report_drill.h"

#include "coop/bug_report/report_bundle.h"
#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/net/peer_identity.h"
#include "coop/net/session.h"
#include "coop/player/roster.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"  // HasAnnouncedWorldReady

#include "ue_wrap/core/log.h"

#include "miniz.h"
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace coop::dev::bug_report_drill {
namespace {

namespace BR = coop::bug_report;

enum class Mode : uint8_t { Off, On, Red };

Mode ModeNow() {
    static const Mode mode = [] {
        const std::string v =
            coop::config::ResolveEnum(::coop::config_registry::rows::bug_report_drill);
        return v == "on" ? Mode::On : v == "red" ? Mode::Red : Mode::Off;
    }();
    return mode;
}

// The values planted in the log and the form: one of every class the redactor rewrites.
constexpr const char* kAddr = "203.0.113.7";
constexpr const char* kId = "5eedf00d5eedf00d5eedf00d5eedf00d";
constexpr const char* kHappened = "[report-drill] the drill describes what happened here";
constexpr int kWaitS = 60;

enum class Phase : uint8_t { Plant, Ask, Wait, Done };

Phase g_phase = Phase::Plant;
bool g_wasPaired = false;
std::string g_profileForm;  // the planted folder, as the redactor knows it
std::string g_happened;     // the form's first field, as asked
std::chrono::steady_clock::time_point g_askedAt;

void Fail(const std::string& why) {
    UE_LOGE("[REPORT-DRILL] FAIL: %s", why.c_str());
    g_phase = Phase::Done;
}

// An abort is not a measurement: the drill could not reach its verdict.
void Abort(const char* why) {
    UE_LOGW("[REPORT-DRILL] ABORT: %s", why);
    g_phase = Phase::Done;
}

bool ClientReady(coop::net::Session& s) {
    return s.connected() && coop::net_pump::HasAnnouncedWorldReady() &&
           coop::join_progress::CurrentPhase() == coop::join_progress::Phase::Idle;
}

bool ReadWhole(const std::wstring& path, std::string& out) {
    FILE* f = nullptr;
    if (::_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return false;
    bool ok = std::fseek(f, 0, SEEK_END) == 0;
    const long size = ok ? std::ftell(f) : -1;
    ok = ok && size >= 0 && std::fseek(f, 0, SEEK_SET) == 0;
    if (ok) {
        out.resize(static_cast<size_t>(size));
        ok = std::fread(out.data(), 1, out.size(), f) == out.size();
    }
    std::fclose(f);
    return ok;
}

struct Item {
    std::string name, body;
};

const Item* Find(const std::vector<Item>& items, const char* name) {
    for (const Item& i : items)
        if (i.name == name) return &i;
    return nullptr;
}

// Opens the finished zip and judges it: prints PASS or the first failing check.
void Judge(bool host) {
    const BR::Status st = BR::GetStatus();
    std::string zip;
    if (!ReadWhole(st.zipPath, zip)) return Fail("the zip cannot be read");
    mz_zip_archive z;
    mz_zip_zero_struct(&z);
    if (!mz_zip_reader_init_mem(&z, zip.data(), zip.size(), 0)) return Fail("the zip does not open");
    std::vector<Item> items;
    const mz_uint count = mz_zip_reader_get_num_files(&z);
    for (mz_uint k = 0; k < count; ++k) {
        char name[260] = {};
        mz_zip_reader_get_filename(&z, k, name, sizeof(name));
        size_t size = 0;
        void* data = mz_zip_reader_extract_to_heap(&z, k, &size, 0);
        if (!data) {
            mz_zip_reader_end(&z);
            return Fail(std::string(name) + " cannot be extracted");
        }
        items.push_back({name, std::string(static_cast<const char*>(data), size)});
        mz_free(data);
    }
    mz_zip_reader_end(&z);

    for (const char* need : {"report.txt", "meta.json", "multivoid.log"})
        if (!Find(items, need)) return Fail(std::string(need) + " is missing");
    const nlohmann::json meta = nlohmann::json::parse(Find(items, "meta.json")->body, nullptr, false);
    if (meta.is_discarded() || !meta.is_object()) return Fail("meta.json does not parse");
    // An install with no multivoid.ini leaves it out and says so; any other absence is a failure.
    if (!Find(items, "multivoid.ini")) {
        bool notPresent = false;
        const auto left = meta.find("left_out");
        if (left != meta.end() && left->is_object()) {
            const auto why = left->find("multivoid.ini");
            notPresent = why != left->end() && why->is_string() && why->get<std::string>() == "not present";
        }
        if (!notPresent) return Fail("multivoid.ini is missing");
    }
    if (Find(items, "report.txt")->body.find(g_happened) == std::string::npos)
        return Fail("report.txt lacks the form's text");
    const std::string role = meta.value("role", std::string());
    if (role != (host ? "host" : "client")) return Fail("meta.json names the role '" + role + "'");

    struct Needle {
        const char* what;
        std::string text;
    };
    const Needle needles[] = {{"address", kAddr},
                              {"id", kId},
                              {"key", std::string(64, 'a')},
                              {"path", g_profileForm},
                              {"own id", coop::net::peer_identity::LocalGuid()}};
    for (const Item& item : items)
        for (const Needle& n : needles)
            if (!n.text.empty() && item.body.find(n.text) != std::string::npos)
                return Fail(std::string("planted ") + n.what + " in " + item.name);
    const std::string& log = Find(items, "multivoid.log")->body;
    for (const char* token : {"addr#", "player#", "key#", "%USERPROFILE%"})
        if (log.find(token) == std::string::npos)
            return Fail(std::string("multivoid.log holds no ") + token);

    UE_LOGI("[REPORT-DRILL] PASS (%u entries, %zu bytes)", static_cast<unsigned>(count), zip.size());
    g_phase = Phase::Done;
}

}  // namespace

void Tick(coop::net::Session* session) {
    if (ModeNow() == Mode::Off) return;
    if (!session || !session->running()) {
        // A session that ended re-arms the host's pairing edge for the next one.
        g_wasPaired = false;
        return;
    }
    const bool host = session->role() == coop::net::Role::Host;
    bool paired = true;
    if (host) {
        // A host judges after a client has joined, and again each time one joins anew: a joiner
        // the rig relaunched re-marks the host's log, and the verdict must come after the mark.
        coop::roster::Snapshot roster;
        coop::roster::GetSnapshot(roster);
        paired = roster.count >= 2;
        if (paired && !g_wasPaired && g_phase != Phase::Plant) g_phase = Phase::Plant;
        g_wasPaired = paired;
    }
    switch (g_phase) {
    case Phase::Plant: {
        if (host ? !paired : !ClientReady(*session)) return;
        // A request still running is not this drill's to refuse: the next one waits for it.
        if (BR::GetStatus().phase == BR::Phase::Building) return;
        UE_LOGI("[REPORT-DRILL] plant address %s", ue_wrap::log::Addr(kAddr).c_str());
        UE_LOGI("[REPORT-DRILL] plant id %s", kId);
        UE_LOGI("[REPORT-DRILL] plant key gen:%s", std::string(64, 'a').c_str());
        const BR::RedactContext machine = BR::ReadThisMachine(std::string(), std::string());
        if (machine.profileForms.empty()) return Fail("no profile folder");
        g_profileForm = machine.profileForms[0];
        UE_LOGI("[REPORT-DRILL] plant path %s\\report-drill", g_profileForm.c_str());
        g_phase = Phase::Ask;
        return;
    }
    case Phase::Ask: {
        // The red puts the address in the player's own words, which are never marked: the zip's
        // report.txt then holds it and the judge must fail.
        g_happened = ModeNow() == Mode::Red ? std::string(kHappened) + " at " + kAddr : kHappened;
        BR::Form form;
        form.happened = g_happened;
        form.expected = "nothing";
        form.contact = "drill";
        if (!BR::Request(std::move(form))) return Fail("request refused");
        g_askedAt = std::chrono::steady_clock::now();
        g_phase = Phase::Wait;
        return;
    }
    case Phase::Wait: {
        const BR::Status st = BR::GetStatus();
        if (st.phase == BR::Phase::Done) return Judge(host);
        if (st.phase == BR::Phase::Failed) return Fail("bundle failed: " + st.error);
        if (std::chrono::steady_clock::now() - g_askedAt > std::chrono::seconds(kWaitS))
            return Abort("the bundle not done in 60 s");
        return;
    }
    case Phase::Done:
        return;
    }
}

}  // namespace coop::dev::bug_report_drill
