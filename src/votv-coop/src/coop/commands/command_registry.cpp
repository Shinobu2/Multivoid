// coop/commands/command_registry.cpp -- the command tree, its node declarations, and the rules a
// tree must satisfy before the registry takes it.
//
// Shapes: LuckPerms' command tree with a node per sub-command (`/lp user <p> permission set`) and
// EssentialsX's base node derived from the command's name, `essentials.<name>`
// (reference/EssentialsX/Essentials/src/main/java/com/earth2me/essentials/User.java:133-135);
// MTA keeps one command table whose entries carry their default into the single check
// (reference/mtasa-blue/Server/mods/deathmatch/logic/CConsoleCommand.h:16-35).

#include "coop/commands/command_registry.h"

#include "coop/commands/command_line.h"

#include <algorithm>
#include <set>

namespace coop::commands {

namespace {

constexpr size_t kMaxNameLen = 32;

bool ValidName(std::string_view s) {
    if (s.empty() || s.size() > kMaxNameLen) return false;
    for (char c : s)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) return false;
    return true;
}

// Words that name nodes which are not commands (`multivoid.printer.*`, `multivoid.admin.menu`,
// `multivoid.command.selector`, `multivoid.dev.*`): no command may take them.
bool Reserved(std::string_view name) {
    return name == "printer" || name == "content" || name == "admin" || name == "command" ||
           name == "dev";
}

using NodeMap = std::map<std::string, NodeDecl, std::less<>>;

// One walk over a tree that is not yet taken: the first broken rule stops it with a reason.
struct TreeCheck {
    const NodeMap& declared;
    std::set<std::string> byThisTree;  // nodes declared by specs earlier in depth-first order
    std::string why;

    bool Fail(std::string reason) {
        why = std::move(reason);
        return false;
    }

    bool IsDeclared(const std::string& node) const {
        return declared.find(node) != declared.end() || byThisTree.count(node) != 0;
    }

    bool Spec(const CommandSpec& s, const std::string& parentNode, bool isRoot) {
        if (!ValidName(s.name)) return Fail("'" + s.name + "' is not 1..32 of [a-z0-9]");
        if (isRoot && Reserved(s.name)) return Fail("'" + s.name + "' is a reserved word");
        if (!isRoot && !s.aliases.empty()) return Fail("'" + s.name + "': an alias on a sub-verb");
        for (const Alias& a : s.aliases) {
            if (!ValidName(a.name)) return Fail("alias '" + a.name + "' is not 1..32 of [a-z0-9]");
            if (!s.subVerbs.empty()) {
                const ParsedLine preset = SplitLine(a.presetWords);
                bool names = false;
                if (!preset.words.empty())
                    for (const CommandSpec& sub : s.subVerbs)
                        names = names || EqualsAsciiNoCase(preset.words[0], sub.name);
                if (!names)
                    return Fail("alias '" + a.name + "': its first preset word names no sub-verb of '" +
                                s.name + "'");
            }
        }
        if (s.handler == nullptr && s.subVerbs.empty())
            return Fail("'" + s.name + "' has neither a handler nor sub-verbs");
        bool sawOptional = false;
        for (size_t i = 0; i < s.args.size(); ++i) {
            const ArgSpec& a = s.args[i];
            if (a.kind == ArgKind::Rest && i + 1 != s.args.size())
                return Fail("'" + s.name + "': a Rest argument must be last");
            if (a.optional) sawOptional = true;
            else if (sawOptional)
                return Fail("'" + s.name + "': a required argument after an optional one");
        }

        std::string node;
        if (!s.nodeOf.empty()) {
            if (!IsDeclared(s.nodeOf))
                return Fail("'" + s.name + "': nodeOf names the undeclared node '" + s.nodeOf + "'");
            node = s.nodeOf;
        } else {
            node = (isRoot ? std::string("multivoid") : parentNode) + "." + s.name;
            if (IsDeclared(node)) return Fail("the node '" + node + "' is already declared");
            byThisTree.insert(node);
        }

        std::set<std::string> subNames;
        for (const CommandSpec& sub : s.subVerbs) {
            if (!subNames.insert(sub.name).second)
                return Fail("'" + s.name + "': two sub-verbs named '" + sub.name + "'");
            if (!Spec(sub, node, false)) return false;
        }
        return true;
    }
};

}  // namespace

bool Registry::Register(CommandSpec spec, std::string* why) {
    auto refuse = [&](const std::string& reason) {
        if (why != nullptr) *why = reason;
        return false;
    };

    std::set<std::string> taken;
    for (const auto& r : roots_) {
        taken.insert(r->name);
        for (const Alias& a : r->aliases) taken.insert(a.name);
    }
    if (taken.count(spec.name) != 0) return refuse("'" + spec.name + "' is already a command or alias");
    std::set<std::string> own{spec.name};
    for (const Alias& a : spec.aliases) {
        if (taken.count(a.name) != 0) return refuse("alias '" + a.name + "' is already a command or alias");
        if (!own.insert(a.name).second) return refuse("alias '" + a.name + "' is named twice");
    }

    TreeCheck check{nodes_, {}, {}};
    if (!check.Spec(spec, std::string(), true)) return refuse(check.why);

    auto owned = std::make_unique<CommandSpec>(std::move(spec));
    Record(*owned, std::string(), std::string());
    roots_.push_back(std::move(owned));
    return true;
}

void Registry::Record(const CommandSpec& c, const std::string& parentPath,
                      const std::string& parentNode) {
    SpecInfo info;
    info.path = parentPath.empty() ? c.name : parentPath + " " + c.name;
    if (!c.nodeOf.empty()) {
        info.node = c.nodeOf;
    } else {
        info.node = (parentNode.empty() ? std::string("multivoid") : parentNode) + "." + c.name;
        nodes_.emplace(info.node, NodeDecl{info.node, c.defaultGranted, c.description});
    }
    const std::string path = info.path;
    const std::string node = info.node;
    info_.emplace(&c, std::move(info));
    for (const CommandSpec& sub : c.subVerbs) Record(sub, path, node);
}

bool Registry::DeclareNode(NodeDecl d, std::string* why) {
    auto refuse = [&](const char* reason) {
        if (why != nullptr) *why = reason;
        return false;
    };
    if (d.node.empty()) return refuse("an empty node");
    if (nodes_.find(d.node) != nodes_.end()) return refuse("the node is already declared");
    const std::string key = d.node;
    nodes_.emplace(key, std::move(d));
    return true;
}

const CommandSpec* Registry::FindRoot(std::string_view word, const Alias** alias) const {
    if (alias != nullptr) *alias = nullptr;
    for (const auto& r : roots_) {
        if (EqualsAsciiNoCase(word, r->name)) return r.get();
        for (const Alias& a : r->aliases) {
            if (!EqualsAsciiNoCase(word, a.name)) continue;
            if (alias != nullptr) *alias = &a;
            return r.get();
        }
    }
    return nullptr;
}

std::vector<const CommandSpec*> Registry::AllVerbs() const {
    std::vector<const CommandSpec*> roots;
    roots.reserve(roots_.size());
    for (const auto& r : roots_) roots.push_back(r.get());
    std::sort(roots.begin(), roots.end(),
              [](const CommandSpec* a, const CommandSpec* b) { return a->name < b->name; });

    std::vector<const CommandSpec*> out;
    struct Walk {
        static void Into(const CommandSpec& c, std::vector<const CommandSpec*>& out) {
            if (c.handler != nullptr) out.push_back(&c);
            for (const CommandSpec& sub : c.subVerbs) Into(sub, out);
        }
    };
    for (const CommandSpec* r : roots) Walk::Into(*r, out);
    return out;
}

const NodeDecl* Registry::FindNode(std::string_view node) const {
    const auto it = nodes_.find(node);
    return it == nodes_.end() ? nullptr : &it->second;
}

std::string Registry::NodeOf(const CommandSpec& c) const {
    const auto it = info_.find(&c);
    return it == info_.end() ? std::string() : it->second.node;
}

std::string Registry::PathOf(const CommandSpec& c) const {
    const auto it = info_.find(&c);
    return it == info_.end() ? std::string() : it->second.path;
}

std::string Registry::Usage(const CommandSpec& c) const {
    std::string out = "/" + PathOf(c);
    for (const ArgSpec& a : c.args) {
        out += a.optional ? " [" : " <";
        out += a.name;
        if (a.kind == ArgKind::Rest) out += "...";
        out += a.optional ? "]" : ">";
    }
    if (!c.subVerbs.empty()) {
        std::string names;
        for (const CommandSpec& sub : c.subVerbs) {
            if (!names.empty()) names += "|";
            names += sub.name;
        }
        out += c.handler != nullptr ? " [" + names + "]" : " <" + names + ">";
    }
    return out;
}

}  // namespace coop::commands
