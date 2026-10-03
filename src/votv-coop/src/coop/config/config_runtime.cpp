// coop/config/config_runtime.cpp -- a config row's value while the game runs: the runtime layer
// every Resolve reads first (config.cpp, PickRawLayered), SetValue and ResetValue (hold or drop,
// write or remove its ini line, notify) and the per-row subscribers. MTA's CMainConfig::SetSetting
// is the shape: refuse, assign, save, call the row's callback. The divergence: in the game the
// callback is posted to the game thread through the notifier the boot code wires, because a set
// comes from the render thread; a process that wires none calls it inside the setter, as MTA does.

// Two rules a later caller must keep. The lock order is g_setMutex, then g_layerMutex or
// IniMutex(), never the reverse; g_layerMutex is never held across a call out. A subscriber never
// sets a row: one made on its own stack is refused (t_notifying); one it defers or hands to
// another thread loops inside one drain (game_thread.cpp:277-286) and is not guarded.

// Also unlike the precedents: the ini is written on every accepted set (MTA's server: only when
// bSavable && bSave, CMainConfig.cpp:1479-1483; its client: every apply, CSettings.cpp:4790); a
// live set beats the launch pin so a change holds for the run (MTA: command line wins,
// CMainConfig.cpp:1050-1081); Source clamps an out-of-range value and calls back only on a change
// (convar.cpp:794-798, 843-853), we refuse it and notify every set and reset (CMainConfig.cpp:1485).

#include "coop/config/config.h"

#include "config_internal.h"
#include "coop/config/config_registry.h"
#include "ue_wrap/core/log.h"

#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace coop::config {

namespace {

using Row = config_registry::Row;

// Held by SetValueAt across its put and its ini write, so two sets cannot interleave them, by
// ResetValueAt across its drop and its ini write, and by the keep-line across its rewrite and its
// drop (SetMutex).
std::mutex g_setMutex;
// The layer map, the subscriber list and the notifier pointer; also the session layer's map
// (config_session.cpp, through internal::LayerMutex).
std::mutex g_layerMutex;
std::unordered_map<const Row*, std::string> g_layer;
std::vector<std::pair<const Row*, void (*)()>> g_subscribers;
void (*g_post)(std::function<void()> task) = nullptr;

// True on a thread while it runs a row's subscribers: SetValueAt refuses a set from there, and
// ResetValueAt a reset.
thread_local bool t_notifying = false;

// Sets t_notifying for its scope and restores the previous value, so an early return or a throw
// cannot leave it set.
struct NotifyScope {
    const bool prev;
    NotifyScope() : prev(t_notifying) { t_notifying = true; }
    ~NotifyScope() { t_notifying = prev; }
    NotifyScope(const NotifyScope&) = delete;
    NotifyScope& operator=(const NotifyScope&) = delete;
};

void AddSubscriber(const Row* row, void (*onChange)()) {
    if (!row) {
        UE_LOGE("config: Subscribe REFUSED -- row is null");
        return;
    }
    if (!onChange) {
        UE_LOGE("config: Subscribe %s REFUSED -- onChange is null", row->key);
        return;
    }
    std::lock_guard<std::mutex> lk(g_layerMutex);
    for (const auto& s : g_subscribers)
        if (s.first == row && s.second == onChange) return;
    g_subscribers.emplace_back(row, onChange);
}

}  // namespace

namespace internal {

std::mutex& SetMutex() { return g_setMutex; }

std::mutex& LayerMutex() { return g_layerMutex; }

bool RuntimeLayerGet(const Row* row, std::string& raw) {
    std::lock_guard<std::mutex> lk(g_layerMutex);
    const auto it = g_layer.find(row);
    if (it == g_layer.end()) return false;
    raw = it->second;
    return true;
}

void RuntimeLayerPut(const Row* row, const std::string& raw) {
    std::lock_guard<std::mutex> lk(g_layerMutex);
    g_layer[row] = raw;
}

void RuntimeLayerDrop(const Row* row) {
    std::lock_guard<std::mutex> lk(g_layerMutex);
    g_layer.erase(row);
}

bool HasSubscriber(const Row* row) {
    std::lock_guard<std::mutex> lk(g_layerMutex);
    for (const auto& s : g_subscribers)
        if (s.first == row) return true;
    return false;
}

void NotifySubscribers(const Row* row) {
    // Copied under the lock and called after it is released: a subscriber re-resolves, which
    // takes g_layerMutex again.
    std::vector<void (*)()> fns;
    {
        std::lock_guard<std::mutex> lk(g_layerMutex);
        for (const auto& s : g_subscribers)
            if (s.first == row) fns.push_back(s.second);
    }
    const NotifyScope scope;
    for (void (*fn)() : fns) fn();
}

void PostNotify(const Row* row) {
    if (HasSubscriber(row)) {
        void (*post)(std::function<void()> task) = nullptr;
        {
            std::lock_guard<std::mutex> lk(g_layerMutex);
            post = g_post;
        }
        if (post) post([row] { NotifySubscribers(row); });
        else NotifySubscribers(row);
    }
}

SetResult SetValueAt(const std::wstring& iniPath, const Row* row, const char* value) {
    if (t_notifying) {
        UE_LOGW("config: SET %s REFUSED -- called from inside a change notification; a subscriber "
                "never sets a row", row->key);
        return SetResult::Refused;
    }
    const std::string v =
        NormalizeValue(value, row->kind != config_registry::Kind::String);
    if (!ValueValidForKey(row->key, v, nullptr)) {
        UE_LOGW("config: SET %s REFUSED", row->key);
        return SetResult::Refused;
    }
    bool saved = false;
    {
        std::lock_guard<std::mutex> setLock(g_setMutex);
        RuntimeLayerPut(row, v);
        saved = WriteIniKeyAtPath(iniPath, row->key, v.c_str());
    }
    // The value is shown as the ini writer shows it: a credential is never printed.
    const char* shown = config_registry::IsCredentialKey(row->key) ? "<set>" : v.c_str();
    std::string over;
    if (row->envVar && !ReadEnv(row->envVar).empty()) over = std::string(", over ") + row->envVar;
    UE_LOGI("config: SET %s=%s (runtime%s%s)", row->key, shown, saved ? "" : ", not saved",
            over.c_str());
    PostNotify(row);
    return saved ? SetResult::Saved : SetResult::HeldNotSaved;
}

// ResetValue's precedents: MTA's client "Load defaults" re-applies a copy of the default literals
// (CSettings.cpp:2835-2861); Source's ConVar::Revert is a set to the default
// (convar.cpp:1076-1081). Ours removes the stored line instead, so a later change of the default,
// or an environment twin, answers rather than a frozen copy. It notifies even when no line was
// stored: MTA's "Load defaults" re-applies every value unconditionally (CSettings.cpp:2837-2858)
// and every MTA Set bumps a revision counter that consumers poll (CClientVariables.h:78-112,
// CGUI.cpp:303-305, CChat.cpp:161-164) -- the same always-notify shape, read by polling where ours
// is pushed; Source's Revert skips the callbacks when the value is unchanged (convar.cpp:843-853).
// Ours always notifies: it compares no values, and a subscriber re-resolves the row rather than
// trusting what it saw before.
SetResult ResetValueAt(const std::wstring& iniPath, const Row* row) {
    if (t_notifying) {
        UE_LOGW("config: RESET %s REFUSED -- called from inside a change notification; a "
                "subscriber never sets a row", row->key);
        return SetResult::Refused;
    }
    int removed = 0;
    bool wrote = false;
    {
        std::lock_guard<std::mutex> setLock(g_setMutex);
        RuntimeLayerDrop(row);
        wrote = RemoveIniKeyAtPath(iniPath, row->key, removed);
    }
    if (wrote) UE_LOGI("config: RESET %s (runtime, ini lines removed: %d)", row->key, removed);
    else UE_LOGI("config: RESET %s (runtime, ini not rewritten)", row->key);
    PostNotify(row);
    return wrote ? SetResult::Saved : SetResult::HeldNotSaved;
}

}  // namespace internal

SetResult SetValue(const config_registry::FlagRow& row, const char* value) {
    return internal::SetValueAt(internal::LiveIniPath(), row.row, value);
}
SetResult SetValue(const config_registry::IntRow& row, const char* value) {
    return internal::SetValueAt(internal::LiveIniPath(), row.row, value);
}
SetResult SetValue(const config_registry::FloatRow& row, const char* value) {
    return internal::SetValueAt(internal::LiveIniPath(), row.row, value);
}
SetResult SetValue(const config_registry::EnumRow& row, const char* value) {
    return internal::SetValueAt(internal::LiveIniPath(), row.row, value);
}
SetResult SetValue(const config_registry::StringRow& row, const char* value) {
    return internal::SetValueAt(internal::LiveIniPath(), row.row, value);
}

SetResult ResetValue(const config_registry::FlagRow& row) {
    return internal::ResetValueAt(internal::LiveIniPath(), row.row);
}
SetResult ResetValue(const config_registry::IntRow& row) {
    return internal::ResetValueAt(internal::LiveIniPath(), row.row);
}
SetResult ResetValue(const config_registry::FloatRow& row) {
    return internal::ResetValueAt(internal::LiveIniPath(), row.row);
}
SetResult ResetValue(const config_registry::EnumRow& row) {
    return internal::ResetValueAt(internal::LiveIniPath(), row.row);
}
SetResult ResetValue(const config_registry::StringRow& row) {
    return internal::ResetValueAt(internal::LiveIniPath(), row.row);
}

void Subscribe(const config_registry::FlagRow& row, void (*onChange)()) {
    AddSubscriber(row.row, onChange);
}
void Subscribe(const config_registry::IntRow& row, void (*onChange)()) {
    AddSubscriber(row.row, onChange);
}
void Subscribe(const config_registry::FloatRow& row, void (*onChange)()) {
    AddSubscriber(row.row, onChange);
}
void Subscribe(const config_registry::EnumRow& row, void (*onChange)()) {
    AddSubscriber(row.row, onChange);
}
void Subscribe(const config_registry::StringRow& row, void (*onChange)()) {
    AddSubscriber(row.row, onChange);
}

void SubscribeServerScope(void (*onChange)()) {
    size_t count = 0;
    const Row* rows = config_registry::Rows(count);
    for (size_t i = 0; i < count; ++i)
        if (config_registry::IsReplicated(&rows[i])) AddSubscriber(&rows[i], onChange);
}

void SetNotifier(void (*post)(std::function<void()> task)) {
    std::lock_guard<std::mutex> lk(g_layerMutex);
    g_post = post;
}

}  // namespace coop::config
