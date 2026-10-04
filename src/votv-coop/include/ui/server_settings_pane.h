// ui/server_settings_pane.h -- the F1 > Administration > Server settings content pane (host-only).
//
// Generated from the registry: every server-scope row that has a label (config_registry::RowLabel)
// and is not a credential, in table order, drawn by its kind -- a checkbox for a flag, an input box
// with Apply for a number or a string, a combo for an enum -- with the row's desc as its tooltip, a
// Reset button, and under a row that is not live (config_registry::IsLive) the words "Takes effect
// at the next session." A change never calls a setter: the pane submits the same `set <key> <value>`
// and `reset <key>` lines a person types in chat, through coop::command_sync::Submit, so the
// dispatcher's permission check and the host's setters are the ones a command meets.
//
// The pane is the host's (dev_menu gates the Administration category on the host role). Render
// thread; the values come from config::EffectiveText, which any thread may call.

#pragma once

namespace ui::server_settings_pane {

// Draw the pane content (called from dev_menu's content child).
void Render();

}  // namespace ui::server_settings_pane
