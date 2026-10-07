// coop/net/session_serial.h -- a serial per session start. Session::Start advances it, on either
// role, before the session is marked running, so a running session's serial is already current.
// What a session produces and keeps in a process-wide slot is stamped with it: a reader that finds
// another serial knows the value belongs to a session that has ended. The first serial is 1; 0 is
// "no session yet". Any thread.
#pragma once

#include <cstdint>

namespace coop::net::session_serial {

// Starts a new session's serial and returns it.
uint32_t Next();

// The serial of the latest session start; 0 before the first.
uint32_t Current();

}  // namespace coop::net::session_serial
