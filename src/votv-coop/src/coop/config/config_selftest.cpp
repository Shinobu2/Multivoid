// coop/config/config_selftest.cpp -- the dev selftest seams of the config
// corpus instrument (probes are RULE-2-exempt). Path-parameterized twins of
// the two readers + the raw line list + the injected-failure scan + the typed
// resolver twins, so the env-gated autotest (harness/autotest/autotest_config.cpp) can
// run the REAL lexer over corpus ini files and prove the tri-state branches
// and the arc-3 default/sentinel semantics. The runtime layer's selftest also lives here:
// SelftestRuntimeLayer drives SetValue's own code against a scratch ini. Not for product use:
// product code reads only the module-dir ini via the public config.h API.
//
// Extracted from config.cpp (arc 3, soft-cap discipline -- the C5a twins
// pushed it past 800 LOC; the instrument seams are their own concept). All
// semantics live in config.cpp's internal:: cores (*FromRaw, the path readers)
// -- this TU is glue only, so product and instrument cannot diverge.

#include "coop/config/config.h"

#include "config_internal.h"
#include "coop/config/config_registry.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/paths.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <string>
#include <vector>

namespace coop::config {

using IniScan = internal::IniScan;

IniSelftestRead SelftestReadValue(const std::wstring& path, const char* key) {
    IniScan st = IniScan::Ok;
    IniFault fault = IniFault::None;
    IniSelftestRead r;
    const std::string sentinel = "\x01<absent>";
    r.value = internal::ReadIniValueAtPath(path, key, sentinel.c_str(), &st, &fault);
    r.found = (r.value != sentinel);
    if (!r.found) r.value.clear();
    r.scan = static_cast<int>(st);
    r.fault = fault;
    return r;
}

int SelftestFlagTriState(const std::wstring& path, const char* key) {
    return internal::LookupTriStateAtPath(path, key);
}

int SelftestListLines(const std::wstring& path, std::vector<std::string>& out) {
    const IniScan st =
        internal::ScanIniFile(path, [&](const std::string& line) { out.push_back(line); });
    return static_cast<int>(st);
}

int SelftestScanWithFailure(int failAfterLines) {
    return internal::ScanWithInjectedFailure(failAfterLines);
}

// ---- typed-resolver twins (arc 3 C5; see config.h) --------------------------

namespace {

// The twins' ini pick: same authoritative-line read as the live layer, over
// `path` instead of the module ini; NO env layer (the env layer is drilled by
// its own live-resolver control -- see config.h).
bool PickIniAt(const std::wstring& path, const config_registry::Row* row, std::string& raw) {
    static const char* kAbsent = "\x01<absent>";
    IniScan st = IniScan::Ok;
    const std::string v = internal::ReadIniValueAtPath(path, row->key, kAbsent, &st);
    if (v == kAbsent) return false;
    raw = v;
    return true;
}

}  // namespace

bool SelftestResolveFlagAt(const std::wstring& path, const config_registry::FlagRow& h) {
    std::string raw;
    const bool have = PickIniAt(path, h.row, raw);
    return internal::FlagFromRaw(h.row, have, raw);
}
long SelftestResolveIntAt(const std::wstring& path, const config_registry::IntRow& h) {
    std::string raw;
    const bool have = PickIniAt(path, h.row, raw);
    return internal::IntFromRaw(h.row, have, raw);
}
float SelftestResolveFloatAt(const std::wstring& path, const config_registry::FloatRow& h) {
    std::string raw;
    const bool have = PickIniAt(path, h.row, raw);
    return internal::FloatFromRaw(h.row, have, raw);
}
std::string SelftestResolveEnumAt(const std::wstring& path, const config_registry::EnumRow& h) {
    std::string raw;
    const bool have = PickIniAt(path, h.row, raw);
    return internal::EnumFromRaw(h.row, have, raw);
}
std::string SelftestResolveStringAt(const std::wstring& path, const config_registry::StringRow& h) {
    std::string raw;
    if (!PickIniAt(path, h.row, raw)) return h.row->defS;
    return raw;
}
FailClosedRead SelftestResolveFailClosedAt(const std::wstring& path,
                                           const config_registry::FailClosedEnumRow& h,
                                           std::string& out, IniFault* faultOut) {
    static const char* kAbsent = "\x01<absent>";
    IniScan st = IniScan::Ok;
    IniFault fault = IniFault::None;
    const std::string v = internal::ReadIniValueAtPath(path, h.row->key, kAbsent, &st, &fault);
    const bool have = v != kAbsent;
    return internal::FailClosedFromPick(h.row, have, have ? v : std::string(), /*fromEnv*/ false,
                                        st, fault, out, nullptr, nullptr, faultOut);
}

// ---- the runtime layer (config_runtime.cpp) ----------------------------------

namespace {

// The probe row's subscriber: counts its calls and records whether it ran on the thread the
// notifications are meant for.
std::atomic<int> g_subscriberCalls{0};
std::atomic<bool> g_subscriberOnNotifyThread{false};
bool (*g_onNotifyThread)() = nullptr;

void ProbeSubscriber() {
    g_subscriberCalls.fetch_add(1);
    g_subscriberOnNotifyThread.store(g_onNotifyThread && g_onNotifyThread());
}

}  // namespace

int SelftestRuntimeLayer(void (*drain)(), bool (*onNotifyThread)()) {
    namespace reg = config_registry;
    const reg::IntRow& P = reg::rows::selftest_runtime_probe;
    const std::wstring scratch =
        ue_wrap::paths::ExeDir() + L"\\multivoid.selftest-runtime.ini";
    const char* const kEnv = "VOTVCOOP_SELFTEST_RUNTIME_PROBE";
    int fail = 0;
    auto expect = [&](const char* what, bool ok) {
        if (ok) UE_LOGI("config-selftest: runtime layer %s ok", what);
        else { UE_LOGW("config-selftest: FAIL runtime layer %s", what); ++fail; }
    };

    // The environment twin is cleared so check 0 sees the row's default whatever the launch pinned,
    // and restored after check 7.
    char old[256] = {};
    const DWORD oldLen = ::GetEnvironmentVariableA(kEnv, old, sizeof(old));
    ::SetEnvironmentVariableA(kEnv, nullptr);
    {
        FILE* f = nullptr;
        if (_wfopen_s(&f, scratch.c_str(), L"w") == 0 && f) std::fclose(f);
    }
    g_onNotifyThread = onNotifyThread;

    // 0: a peer whose ini holds the probe row fails here, so no later check can pass or fail for
    // that reason.
    expect("clean probe", ResolveInt(P) == 0);

    // 1: the red knob skips the set, so every check that reads the value it would have set fails.
    {
        bool setOk = true;
        if (!ResolveFlag(reg::rows::selftest_break_runtime_layer))
            setOk = internal::SetValueAt(scratch, P.row, "7") == SetResult::Saved;
        expect("set -> resolve", setOk && ResolveInt(P) == 7);
    }
    // 2: the ini holds what was set.
    {
        const IniSelftestRead r = SelftestReadValue(scratch, P.row->key);
        expect("written", r.found && r.value == "7");
    }
    // 3: the layer answers above the environment twin.
    ::SetEnvironmentVariableA(kEnv, "3");
    expect("layer above env", ResolveInt(P) == 7);
    // 4: a refused value changes nothing.
    {
        const SetResult r = internal::SetValueAt(scratch, P.row, "banana");
        expect("refused value", r == SetResult::Refused && ResolveInt(P) == 7);
    }
    // 5: the value is held as the writer normalises it.
    {
        const SetResult r = internal::SetValueAt(scratch, P.row, " 5\r\n");
        std::string raw;
        const bool got = internal::RuntimeLayerGet(P.row, raw);
        expect("normalised", r == SetResult::Saved && got && raw == "5");
    }
    // 6: one call per set however often the pair was registered, on the notification thread.
    {
        g_subscriberCalls.store(0);
        g_subscriberOnNotifyThread.store(false);
        Subscribe(P, &ProbeSubscriber);
        Subscribe(P, &ProbeSubscriber);
        internal::SetValueAt(scratch, P.row, "6");
        drain();
        expect("subscriber on the game thread",
               g_subscriberCalls.load() == 1 && g_subscriberOnNotifyThread.load());
    }
    // 7: with the layer entry dropped, the environment answers again.
    internal::RuntimeLayerDrop(P.row);
    expect("drop -> env answers", ResolveInt(P) == 3);

    ::SetEnvironmentVariableA(kEnv, (oldLen > 0 && oldLen < sizeof(old)) ? old : nullptr);
    ::DeleteFileW(scratch.c_str());
    ::DeleteFileW((scratch + L".new").c_str());
    return fail;
}

}  // namespace coop::config
