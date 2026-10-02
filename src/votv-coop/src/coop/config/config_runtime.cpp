// coop/config/config_runtime.cpp -- a config row's value while the game runs: the runtime layer
// every Resolve reads first (config.cpp, PickRawLayered), SetValue (normalise, refuse, hold, write
// the ini, log, notify) and the per-row subscribers. MTA's CMainConfig::SetSetting is the shape:
// refuse a bad value, assign, save, call the row's change callback. The divergence: in the game the
// callback is posted to the game thread through the notifier the boot code wires, because a set
// comes from the render thread; a process that wires none calls it inside the setter, as MTA does.
//
// Two rules a later caller must keep. The lock order is g_setMutex, then g_layerMutex or
// IniMutex(), never the reverse, and g_layerMutex is never held across a call out (a subscriber,
// the notifier, the logger). And a subscriber never calls SetValue.

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

// Held by SetValueAt across its put and its ini write, so two sets cannot interleave them.
std::mutex g_setMutex;
// The layer map, the subscriber list and the notifier pointer.
std::mutex g_layerMutex;
std::unordered_map<const Row*, std::string> g_layer;
std::vector<std::pair<const Row*, void (*)()>> g_subscribers;
void (*g_post)(std::function<void()> task) = nullptr;

void AddSubscriber(const Row* row, void (*onChange)()) {
    if (!row || !onChange) return;
    std::lock_guard<std::mutex> lk(g_layerMutex);
    for (const auto& s : g_subscribers)
        if (s.first == row && s.second == onChange) return;
    g_subscribers.emplace_back(row, onChange);
}

}  // namespace

namespace internal {

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
    for (void (*fn)() : fns) fn();
}

SetResult SetValueAt(const std::wstring& iniPath, const Row* row, const char* value) {
    const std::string v = NormalizeValue(value);
    if (!ValueValidForKey(row->key, v, nullptr)) {
        UE_LOGW("config: SET %s REFUSED -- the value would be rejected on read (registry "
                "kind/range/tokens); nothing changed", row->key);
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
    if (HasSubscriber(row)) {
        void (*post)(std::function<void()> task) = nullptr;
        {
            std::lock_guard<std::mutex> lk(g_layerMutex);
            post = g_post;
        }
        if (post) post([row] { NotifySubscribers(row); });
        else NotifySubscribers(row);
    }
    return saved ? SetResult::Saved : SetResult::HeldNotSaved;
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

void SetNotifier(void (*post)(std::function<void()> task)) {
    std::lock_guard<std::mutex> lk(g_layerMutex);
    g_post = post;
}

}  // namespace coop::config
