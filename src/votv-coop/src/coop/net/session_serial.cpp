// coop/net/session_serial.cpp -- see coop/net/session_serial.h.

#include "coop/net/session_serial.h"

#include <atomic>

namespace coop::net::session_serial {
namespace {
std::atomic<uint32_t> g_serial{0};
}  // namespace

uint32_t Next() { return g_serial.fetch_add(1, std::memory_order_acq_rel) + 1; }

uint32_t Current() { return g_serial.load(std::memory_order_acquire); }

}  // namespace coop::net::session_serial
