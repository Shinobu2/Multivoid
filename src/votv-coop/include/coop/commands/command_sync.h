// coop/commands/command_sync.h -- the in-game transport of the commands folder: the chat input's
// `/` route, the two wire kinds (CommandRequest, CommandReply), the player record a command sees,
// the per-sender rate limit and the interim permission check.
//
// The folder's model files (command_line, command_targets, command_registry, command_dispatcher)
// stay engine-free; this one reaches the session, the roster ledger, the players registry and the
// game thread. The host dispatches every command: its own line locally, a client's line as a
// request answered privately, line by line, to that client alone. A reply that can only be made
// later goes through ReplyTo with the asker's address (slot and occupancy generation) and is
// dropped when the slot changed hands.

#pragma once

#include "coop/commands/command_registry.h"
#include "coop/commands/command_targets.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace coop::net {
class Session;
struct CommandReplyPayload;
}  // namespace coop::net

namespace coop::command_sync {

// Store the session pointer (subsystems Install, beside chat_sync's). Atomic.
void Install(coop::net::Session* s);

// THE production registry: filled once, on first use, with the built-ins; it exists for the
// process with or without a session (solo play dispatches too). The verbs of later lanes register
// into it. Game thread.
coop::commands::Registry& Commands();

// The text after a chat line's `/`. Any thread (the chat input calls it on the render thread); it
// posts to the game thread, where a process with no running session or a host runs the line itself
// and a client in a running session sends it to the host as a CommandRequest. An empty line (a
// bare `/`) runs locally on either role: its answer, the help hint, is local text.
void Submit(std::string line);

// HOST: a client's CommandRequest, called by the world dispatcher for the transport slot `slot`.
// Takes the slot's rate first and only then judges the bytes, so a flood of malformed lines is
// limited like any other; returns at once on a process that does not host. Game thread.
void OnRequest(const uint8_t* bytes, size_t len, int slot);

// The one way a reply reaches a caller, now or later: a remote slot (this process hosting) gets a
// CommandReply only while the slot still holds the one who asked (`to.generation`); the console
// and the local operator get the line in their own feed. Game thread.
void ReplyTo(const coop::commands::Caller& to, std::string_view line);

// CLIENT: the host's answer line, shown as a private feed line. Game thread.
void OnReply(const coop::net::CommandReplyPayload& p);

// One observer of every delivered reply line, for the dev drill; nullptr clears. Game thread.
void SetReplyObserver(void (*fn)(std::string_view line));

// A slot's occupant left: its rate bucket and notice clocks start over.
void OnSlotDisconnected(int slot);

// The session ended: every bucket and clock starts over.
void OnDisconnect();

}  // namespace coop::command_sync
