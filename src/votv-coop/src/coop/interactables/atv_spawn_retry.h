// Bounded runtime-ATV retries; game thread.
#pragma once
#include <string>
namespace coop::net { struct AtvSpawnPayload; }
namespace coop::atv_sync::spawn_retry {
void DeferSpawn(const std::wstring& key, const coop::net::AtvSpawnPayload& payload,
                const char* why);
void DiscardSpawn(const std::wstring& key);
void RetryPendingSpawns();
void Clear();
}
