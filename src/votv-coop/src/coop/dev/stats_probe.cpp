// coop/dev/stats_probe.cpp -- see coop/dev/stats_probe.h.

#include "coop/dev/stats_probe.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/dev/command_drill.h"
#include "coop/net/session.h"

#include "ue_wrap/actors/vitals.h"
#include "ue_wrap/core/log.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>

namespace coop::dev::stats_probe {
namespace {

namespace V = ue_wrap::vitals;

enum class Arm : uint8_t { Off, On, Red };

Arm ArmNow() {
    static const Arm arm = [] {
        const std::string v = coop::config::ResolveEnum(::coop::config_registry::rows::stats_probe);
        return v == "on" ? Arm::On : v == "red" ? Arm::Red : Arm::Off;
    }();
    return arm;
}

constexpr float kTolerance = 0.001f;
constexpr float kTestStep = 7.f;    // a writable float row is written its value plus this
constexpr float kSnapshotBase = 1000.f;  // snapshot row i is written 1000 + i

bool g_done = false;
int g_bad = 0;  // MISMATCH and NOT FOUND lines
int g_ok = 0;

bool Near(float a, float b) { return std::fabs(a - b) <= kTolerance; }

void Good() { ++g_ok; }
void Bad() { ++g_bad; }

void NotFound(const char* what) {
    UE_LOGE("[STATS-PROBE] %s NOT FOUND", what);
    Bad();
}

// A writable row: read, write, read back, restore, read again. A refused or unreadable step is a
// NOT FOUND. Every attempted write is followed by a restore, since a write can store its value and
// still report failure (Strength and Agility, when updateStrAgl's call fails).
void ProbeWritable(V::Field f, bool red) {
    const V::Row& row = V::RowOf(f);
    float orig = 0.f, back = 0.f, restored = 0.f;
    if (!V::Read(f, &orig)) return NotFound(row.name);
    const float test = row.type == V::Type::Bool ? 1.f - orig : orig + kTestStep;
    const float expected = (red && f == V::Field::Health) ? orig + kTestStep + 1.f : test;
    const bool wrote = V::Write(f, test);
    const bool readBack = wrote && V::Read(f, &back);
    V::Write(f, orig);
    if (!wrote) return NotFound(row.name);
    const bool readRestored = V::Read(f, &restored);
    if (!readBack || !readRestored) return NotFound(row.name);
    const bool ok = Near(back, expected) && Near(restored, orig);
    if (ok) UE_LOGI("[STATS-PROBE] %s read=%.3f wrote=%.3f back=%.3f restored=%.3f OK", row.name, orig, test, back, restored);
    else UE_LOGE("[STATS-PROBE] %s read=%.3f wrote=%.3f back=%.3f restored=%.3f MISMATCH", row.name, orig, test, back, restored);
    ok ? Good() : Bad();
}

void ProbeReadOnly(V::Field f) {
    const V::Row& row = V::RowOf(f);
    float v = 0.f;
    if (!V::Read(f, &v)) return NotFound(row.name);
    UE_LOGI("[STATS-PROBE] %s read=%.3f OK", row.name, v);
    Good();
}

// The snapshot's nine members against their rows: each row is written a distinct value, the
// snapshot is read, and each member must hold its own row's value. The pairing is written out here
// on purpose: it is the instrument for SnapshotField's switch, so a swapped pair reads back crossed.
void ProbeSnapshot() {
    constexpr size_t kRows = 9;
    float orig[kRows];
    for (size_t i = 0; i < kRows; ++i)
        if (!V::Read(static_cast<V::Field>(i), &orig[i])) return NotFound("snapshot");
    bool written[kRows] = {};
    for (size_t i = 0; i < kRows; ++i)
        written[i] = V::Write(static_cast<V::Field>(i), kSnapshotBase + static_cast<float>(i));
    V::Snapshot s;
    const bool haveSnapshot = V::ReadSnapshot(s);
    for (size_t i = 0; i < kRows; ++i)
        V::Write(static_cast<V::Field>(i), orig[i]);  // a refused write stored nothing; a failed one may have
    bool allWritten = true;
    for (size_t i = 0; i < kRows; ++i) allWritten = allWritten && written[i];
    if (!haveSnapshot || !allWritten) return NotFound("snapshot");

    struct Pair { const char* member; float held; V::Field row; };
    const Pair pairs[kRows] = {
        {"health", s.health, V::Field::Health},
        {"maxHealth", s.maxHealth, V::Field::MaxHealth},
        {"food", s.food, V::Field::Food},
        {"sleep", s.sleep, V::Field::Sleep},
        {"battery", s.battery, V::Field::Battery},
        {"coffeePower", s.coffeePower, V::Field::CoffeePower},
        {"gasolinepilled", s.gasolinepilled, V::Field::GasolinePilled},
        {"strength", s.strength, V::Field::Strength},
        {"agility", s.agility, V::Field::Agility},
    };
    for (size_t i = 0; i < kRows; ++i) {
        const float wrote = kSnapshotBase + static_cast<float>(static_cast<size_t>(pairs[i].row));
        if (Near(pairs[i].held, wrote)) continue;
        UE_LOGE("[STATS-PROBE] snapshot MISMATCH: %s holds %.3f, its row %s wrote %.3f", pairs[i].member,
                pairs[i].held, V::RowOf(pairs[i].row).name, wrote);
        return Bad();
    }
    UE_LOGI("[STATS-PROBE] snapshot OK");
    Good();
}

// What the table refuses: a write to a read-only row, a non-finite write, an id past the last row.
void ProbeRefusals() {
    const char* which = nullptr;
    float before = 0.f, after = 0.f;
    V::Field id = V::Field::Health;
    if (V::Write(V::Field::Dead, 1.f)) which = "a write to the read-only Dead was accepted";
    else if (!V::Read(V::Field::Health, &before)) which = "Health did not read";
    else if (V::Write(V::Field::Health, std::nanf(""))) which = "a NaN write to Health was accepted";
    else if (!V::Read(V::Field::Health, &after) || !Near(before, after)) which = "a refused NaN write changed Health";
    else if (V::FieldFromId(static_cast<uint8_t>(V::Field::Count), &id)) which = "id 22 was accepted";
    else if (!V::FieldFromId(static_cast<uint8_t>(V::Field::Glasses), &id)) which = "id 21 was refused";
    if (!which) {
        UE_LOGI("[STATS-PROBE] refusals OK");
        return Good();
    }
    UE_LOGE("[STATS-PROBE] refusals MISMATCH: %s", which);
    Bad();
}

// The save class's own defaults, read through the rows' offsets off its class default object.
void ProbeDefaults() {
    V::Snapshot d;
    if (!V::ReadDefaults(d)) return NotFound("defaults");
    const bool ok = std::isfinite(d.health) && d.health > 0.f && std::isfinite(d.maxHealth) && d.maxHealth > 0.f &&
                    std::isfinite(d.food) && std::isfinite(d.sleep);
    if (ok) UE_LOGI("[STATS-PROBE] defaults health=%.3f maxhealth=%.3f food=%.3f sleep=%.3f OK", d.health, d.maxHealth, d.food, d.sleep);
    else UE_LOGE("[STATS-PROBE] defaults health=%.3f maxhealth=%.3f food=%.3f sleep=%.3f MISMATCH", d.health, d.maxHealth, d.food, d.sleep);
    ok ? Good() : Bad();
}

// The host's readiness: one row of each owner reads.
bool HostReady() {
    float v = 0.f;
    return V::Read(V::Field::Health, &v) && V::Read(V::Field::Air, &v) && V::Read(V::Field::Dreaming, &v);
}

}  // namespace

void Tick(coop::net::Session* session) {
    if (ArmNow() == Arm::Off || g_done || !session || !session->running()) return;
    const bool host = session->role() == coop::net::Role::Host;
    if (host ? !HostReady() : !coop::dev::command_drill::ClientReady(session)) return;
    g_done = true;
    g_bad = 0;
    g_ok = 0;
    const bool red = ArmNow() == Arm::Red;
    for (uint8_t i = 0; i < static_cast<uint8_t>(V::Field::Count); ++i) {
        const V::Field f = static_cast<V::Field>(i);
        if (V::RowOf(f).write == V::WriteRule::ReadOnly) ProbeReadOnly(f);
        else ProbeWritable(f, red);
    }
    ProbeSnapshot();
    ProbeRefusals();
    ProbeDefaults();
    UE_LOGI("[STATS-PROBE] DONE bad=%d ok=%d", g_bad, g_ok);
}

void OnDisconnect() { g_done = false; }

}  // namespace coop::dev::stats_probe
