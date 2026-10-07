// coop/interactables/drive_payload_retry.h -- co-located private header of drive_payload_sync.cpp: the host's rules for
// a client's retried row, independent of engine dispatch.
//
// A failure count permits nothing: a retry is held to the class default like a first write. The one exception is a
// leftover, what a failed write left on the drive when its rollback did not land either. Identity is the row's value
// (member-wise Row ==, the photo unread): a writer that leaves bytes equal to the leftover is not told from no writer.

#pragma once

#include "ue_wrap/desk/signal_dynamic.h"

#include <cstdint>
#include <optional>

namespace coop::drive_payload_retry {

// The row the drive held before the first failed attempt (the class default that write was let onto), what it read
// after the rollback (unset when that read failed: nothing about it is known), and the author whose write it was.
struct Leftover {
    ue_wrap::signal_dynamic::Row                pre;
    std::optional<ue_wrap::signal_dynamic::Row> left;
    uint8_t  slot = 0xFF;
    uint32_t restores = 0;
};

// Whether a client's row may be written over the drive's row `now`: onto the class default once, or over its own
// failed write's leftover while the drive still reads exactly that. A drive that left the leftover had a writer since;
// an unread leftover lets no one write.
enum class Gate { Write, Held, Changed };
inline Gate GateFor(const Leftover* lo, const ue_wrap::signal_dynamic::Row& now, bool nowIsDefault, uint8_t sender) {
    if (!lo) return nowIsDefault ? Gate::Write : Gate::Held;
    if (!lo->left) return Gate::Held;
    if (!(now == *lo->left)) return Gate::Changed;
    return sender == lo->slot ? Gate::Write : Gate::Held;
}

// Whether `now` is still the leftover's, so no writer's row and none to send. An unread leftover is never told from a
// writer's row, so it stays withheld.
inline bool Withholds(const Leftover& lo, const ue_wrap::signal_dynamic::Row& now) {
    return !lo.left || now == *lo.left;
}

// Whether the previous row may be put back over the leftover: only one whose residue was read, a bounded number of
// times. The caller still requires Withholds on a fresh read.
inline bool MayRestore(const Leftover& lo, uint32_t maxRestores) {
    return lo.left.has_value() && lo.restores < maxRestores;
}

// What a failed write or restore leaves, given its read-back (null when the read failed): nothing when the drive reads
// the row from before the first failed attempt again, else a leftover that keeps that first preimage across every
// retry, never a retry's own part-written start. An unread residue is unknown, never assumed to be any row.
inline std::optional<Leftover> AfterFailedWrite(const Leftover* prior, const ue_wrap::signal_dynamic::Row& attemptPre,
                                                const ue_wrap::signal_dynamic::Row* readBack, uint8_t slot) {
    const ue_wrap::signal_dynamic::Row& pre = prior ? prior->pre : attemptPre;
    if (readBack && *readBack == pre) return std::nullopt;
    Leftover lo;
    lo.pre = pre;
    if (readBack) lo.left = *readBack;
    lo.slot = slot;
    lo.restores = prior ? prior->restores : 0;
    return lo;
}

}  // namespace coop::drive_payload_retry
