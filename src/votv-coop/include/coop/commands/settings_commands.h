// coop/commands/settings_commands.h -- the server-setting verbs as commands: /set and /reset.
//
// A domain's command bindings, built as the moderation commands are: this file registers the roots
// into a Registry and the handlers call the domain through the PORTS handed in, captured by value,
// so no file static holds them. The production registry is given the real ports (coop/config,
// command_sync); the selftest gives its own Registry fakes that record what they receive. The
// dispatcher has already decided who may run a command: a handler here never asks the permission
// system. A setting is named by its key, as Source finds a cvar by its name
// (reference/source-sdk-2013/src/tier1/convar.cpp:1269), and only a server-scope row can be named:
// the real ports answer no row for a local one, so no command changes a player's own setting.
//
// Engine-free like the model files: it reaches coop/config only for the row and result types, and
// coop/permissions for the action record.

#pragma once

#include "coop/commands/command_registry.h"
#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/permissions/action_log.h"

#include <string>
#include <string_view>

namespace coop::commands::settings {

struct Ports {
    // The row's canonical key when the typed word names a credential row, else null: a credential
    // is never set, reset or shown by name.
    const char* (*credentialKey)(std::string_view key) = nullptr;
    // The server-scope row the word names, or null (no such key, a local row, a credential row).
    const config_registry::Row* (*findServerRow)(std::string_view key) = nullptr;
    // Whether the reader would accept `value` for the row; on false `why` holds the reason.
    bool (*valid)(const config_registry::Row* row, const std::string& value, std::string* why) = nullptr;
    config::SetResult (*set)(const config_registry::Row* row, const char* value) = nullptr;
    config::SetResult (*reset)(const config_registry::Row* row) = nullptr;
    // The text the row resolves to now, read before and after a set or a reset.
    std::string (*current)(const config_registry::Row* row) = nullptr;
    // Records an admin action the verb carried out; null records nothing.
    void (*log)(const coop::permissions::Action& action) = nullptr;
};

// Registers /set, then /reset (which names /set's node), into `reg`. False (and the registry as it
// was after the last root that registered) when a root is refused.
bool Register(Registry& reg, const Ports& p);

}  // namespace coop::commands::settings
