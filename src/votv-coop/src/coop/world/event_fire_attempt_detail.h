// The replay attempt's engine-free core: the admission permit, frame binding, commit stamp
// and outcome classification that event_fire_sync.cpp wires to the script gate. Kept free of
// engine types so offline checks can exercise the production logic directly.
#pragma once

#include <cstdint>

namespace coop::event_fire_sync::detail {

// The replay attempt, thread-local and stack-owned: one reflected Call's contract with the
// gate. Its permit is the ONE admission the Call may produce, consumed by the first matching
// query. It binds its own invocation's
// frame and takes its commit only from that frame's POST.
struct ReplayAttempt {
    void* object;                   // the eventer the dispatch targets
    void* function;                 // the event verb's UFunction
    std::int32_t eventIndex = 0;   // FName.ComparisonIndex of the expected name argument
    std::int32_t eventNumber = 0;  // FName.Number
    void* stack = nullptr;          // the bound invocation's frame
    bool committed = false;         // the bound invocation's POST ran
    bool permitOpen = true;         // the one-shot admission
    ReplayAttempt* parent = nullptr;
};
inline thread_local ReplayAttempt* t_attempt = nullptr;

class ReplayScope {
public:
    ReplayScope(void* o, void* f, std::int32_t eventIndex, std::int32_t eventNumber) {
        attempt.object = o; attempt.function = f;
        attempt.eventIndex = eventIndex; attempt.eventNumber = eventNumber;
        attempt.parent = t_attempt;
        t_attempt = &attempt;
    }
    ~ReplayScope() { t_attempt = attempt.parent; }
    ReplayScope(const ReplayScope&) = delete;
    ReplayScope& operator=(const ReplayScope&) = delete;
    ReplayAttempt attempt;
};

// One dispatch attempt's result. Committed is the bound invocation's own POST. Cancelled --
// dispatched, the attempt bound its own invocation's entry, and that frame's POST never ran
// (a gate refusal: POSTs are skipped for a Cancel) -- counts against the row's retry bound,
// as does NotAttempted (never reached the call). Unknown is unproven: the dispatch's fault
// signal, OR a returned call whose invocation was never bound -- an observation gap that can
// mean the body ran unseen; effects may already exist, so it is terminal, never retried.
enum class FireOutcome { NotAttempted, Cancelled, Committed, Unknown };

// The outcome from the dispatch's proofs alone. `bound` is the attempt having bound its own
// invocation's frame (at admission or the observe-only entry) -- invocation identity, not a
// "body ran" proof.
inline FireOutcome ClassifyFireOutcome(bool dispatched, bool bound, bool committed,
                                       bool observable) {
    if (!observable) return dispatched ? FireOutcome::Committed : FireOutcome::Unknown;
    if (!dispatched) return FireOutcome::Unknown;
    if (committed) return FireOutcome::Committed;
    return bound ? FireOutcome::Cancelled : FireOutcome::Unknown;
}

// Admission: consume the innermost attempt's one-shot permit for the exact (object, function)
// whose own name argument matched (paramsMatch) on a live invocation frame, and bind that
// frame as the attempt's commit identity. Null on anything short of all of it -- the permit
// stays open when admission is refused.
inline ReplayAttempt* AttemptAdmit(void* object, void* function, bool paramsMatch,
                                   void* stack) {
    ReplayAttempt* a = t_attempt;
    if (!a || !a->permitOpen || !paramsMatch || !stack ||
        a->object != object || a->function != function) return nullptr;
    a->permitOpen = false;
    a->stack = stack;
    return a;
}

// Observe-only entry binding: the first invocation matching the attempt's own (object,
// function, name argument) claims the frame -- once. Never replaces a frame bound at
// admission, and never spends the permit.
inline bool AttemptBind(void* object, void* function, bool paramsMatch, void* stack) {
    ReplayAttempt* a = t_attempt;
    if (!a || a->stack || !paramsMatch || !stack ||
        a->object != object || a->function != function) return false;
    a->stack = stack;
    return true;
}

// The commit proof belongs to the attempt that bound THIS invocation's frame.
inline void AttemptCommit(void* stack) {
    for (ReplayAttempt* a = t_attempt; a; a = a->parent)
        if (a->stack && a->stack == stack) { a->committed = true; return; }
}

}  // namespace coop::event_fire_sync::detail
