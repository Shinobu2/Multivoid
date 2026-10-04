// coop/commands/settings_commands.cpp -- see coop/commands/settings_commands.h.
//
// A credential is answered before the lookup: a value typed into a command line travels, is
// echoed in the reply and stays in the chat history, which a password must not. Every other word
// that names no server-scope row is answered "No server setting is named <word>.".

#include "coop/commands/settings_commands.h"

#include "coop/commands/command_dispatcher.h"

#include <string>
#include <utility>

namespace coop::commands::settings {
namespace {

using config::SetResult;

std::string NotChangedHere(const char* key) {
    return std::string(key) + " is changed in its own screen, not by command.";
}

// The row the first argument names, or null with the reply already made.
const config_registry::Row* NamedRow(const Ports& p, Context& ctx) {
    const std::string& word = ctx.texts[0];
    if (const char* credential = p.credentialKey(word)) {
        ctx.Reply(NotChangedHere(credential));
        return nullptr;
    }
    const config_registry::Row* row = p.findServerRow(word);
    if (row == nullptr) ctx.Reply("No server setting is named " + word + ".");
    return row;
}

Handler SetHandler(const Ports& p) {
    return [p](Context& ctx) {
        const config_registry::Row* row = NamedRow(p, ctx);
        if (row == nullptr) return;
        const std::string& value = ctx.texts[1];
        std::string why;
        if (!p.valid(row, value, &why)) { ctx.Reply(std::string(row->key) + ": " + why); return; }
        switch (p.set(row, value.c_str())) {
            case SetResult::Saved:
                ctx.Reply(std::string(row->key) + " is now " + p.current(row) + ".");
                break;
            case SetResult::HeldNotSaved:
                ctx.Reply(std::string(row->key) + " is now " + p.current(row) +
                          " for this game; the settings file could not be written.");
                break;
            case SetResult::Refused: ctx.Reply(std::string(row->key) + " was not changed."); break;
        }
    };
}

Handler ResetHandler(const Ports& p) {
    return [p](Context& ctx) {
        const config_registry::Row* row = NamedRow(p, ctx);
        if (row == nullptr) return;
        switch (p.reset(row)) {
            case SetResult::Saved:
                ctx.Reply(std::string(row->key) + " is back to " + p.current(row) + ".");
                break;
            case SetResult::HeldNotSaved:
                ctx.Reply(std::string(row->key) +
                          " could not be reset: the settings file could not be written, so its "
                          "stored value still answers.");
                break;
            case SetResult::Refused: ctx.Reply(std::string(row->key) + " was not changed."); break;
        }
    };
}

CommandSpec SetSpec(const Ports& p) {
    CommandSpec c;
    c.name = "set";
    c.description = "Changes a server setting.";
    c.args = {{"setting", ArgKind::Word, false}, {"value", ArgKind::Rest, false}};
    c.handler = SetHandler(p);
    return c;
}

// /reset shares /set's node, so a grant of `multivoid.set` covers both verbs.
CommandSpec ResetSpec(const Ports& p) {
    CommandSpec c;
    c.name = "reset";
    c.description = "Puts a server setting back to its default.";
    c.nodeOf = "multivoid.set";
    c.args = {{"setting", ArgKind::Word, false}};
    c.handler = ResetHandler(p);
    return c;
}

}  // namespace

bool Register(Registry& reg, const Ports& p) {
    return reg.Register(SetSpec(p), nullptr) && reg.Register(ResetSpec(p), nullptr);
}

}  // namespace coop::commands::settings
