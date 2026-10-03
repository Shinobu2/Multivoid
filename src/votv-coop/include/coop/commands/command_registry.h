// coop/commands/command_registry.h -- the commands as a tree of verbs, and the permission nodes
// they declare.
//
// Engine-free (no ue_wrap include). A command may hold sub-verbs, each with its own node
// `<parent's node>.<word>` (LuckPerms' per-sub-command nodes; EssentialsX derives the base node
// from the command's name). The registry is THE declaration of every command node: a node is
// declared once, here, and the dispatcher asks the policy about it.
//
// The production registry has one owner and is filled once at start; a handler is a function
// object that captures what it calls, the registry is static data.

#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <set>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace coop::commands {

enum class ArgKind : uint8_t {
    Word,
    Player,   // one player (ResolveTarget with one = true)
    PlayerOrId,  // one seated player, or, when none matches and the word is 32 hex, that id as an
                 // OFFLINE target
    Players,  // one or more
    Integer,  // a signed decimal into a long long; a leading `+` before a digit is skipped
    Rest,     // the RAW remainder of the line from its first word, trailing spaces removed; last only
};

struct ArgSpec {
    std::string name;
    ArgKind kind;
    bool optional = false;
    // A Player / PlayerOrId target that is slot 0 is refused: the host is the server, it cannot be
    // acted on ("That is the host -- it cannot be <pastTense>.").
    bool notHost = false;
};

// What a command checks about its target before the handler runs, each a node
// `<the spec's node>.<name>` declared with the spec (default false).
enum class QualKind : uint8_t {
    GateOffline,  // acting on a player who is not seated needs `.offline`
    Exempt,       // a target that holds `.exempt` explicitly cannot be acted on
    Notify,       // the seated players that hold `.notify` are told (Context::notifySlots)
};

struct Qualifier {
    std::string name;  // the node's last word: `offline`, `exempt`, `notify`
    QualKind kind;
};

// On a ROOT command. `presetWords` is split by SplitLine and put before the typed words that
// follow the alias (`/day` = {"day", "set day"} on `time` runs `time set day ...`).
struct Alias {
    std::string name;
    std::string presetWords;
};

struct Context;
// A handler captures what it calls; a domain's bindings live in the handler, never in a file static.
using Handler = std::function<void(Context& ctx)>;

struct CommandSpec {
    std::string name;                // 1..32 of [a-z0-9]
    std::vector<Alias> aliases;      // root specs only
    bool defaultGranted = false;
    std::string description;
    // Names a node already declared (before this tree, or by an earlier spec of this tree in
    // depth-first order) instead of the derived one; then that node is the spec's, none is
    // declared for it, and defaultGranted is unused (the declared node's default is what a check
    // is given). `/r` names `multivoid.msg`.
    std::string nodeOf;
    std::vector<ArgSpec> args;
    // The reply's verb (`kicked`, `banned`, `teleported`): Register requires it for a spec with a
    // notHost argument or an Exempt qualifier.
    std::string pastTense;
    // Only the console (a caller flagged isOperator) may run it: the dispatcher refuses any other
    // before the permission check, and /help does not list it to one.
    bool consoleOnly = false;
    // Each qualifier's node is declared with the spec; a second spec naming the same node (through
    // nodeOf) does not declare it again.
    std::vector<Qualifier> qualifiers;
    Handler handler = nullptr;
    std::vector<CommandSpec> subVerbs;  // a spec with sub-verbs may also have its own handler
};

struct NodeDecl {
    std::string node;
    bool defaultGranted;
    std::string description;
};

class Registry {
public:
    // Refuses (and says why) a tree that breaks a rule, checking the whole tree before taking any
    // of it: a name or alias that is not 1..32 of [a-z0-9]; an alias on a sub-verb, or whose first
    // preset word names no sub-verb of a root that has them; a nodeOf that is not declared; a
    // derived node already declared; a root name or alias already taken; two sub-verbs of one
    // parent with one name; a root named printer, content, admin, command or dev (those words
    // belong to nodes that are not commands); a Rest argument that is not last; a required
    // argument after an optional one; a spec with neither a handler nor sub-verbs; a spec with a
    // notHost argument or an Exempt qualifier and no pastTense; a qualifier whose name is not
    // 1..32 of [a-z0-9]; two qualifiers of one kind on a spec; a GateOffline or Exempt qualifier on
    // a spec that has not exactly one Player or PlayerOrId argument; a PlayerOrId argument on a spec
    // with no GateOffline qualifier; a qualifier node that is already declared as something other
    // than a qualifier node. On success every spec without a
    // nodeOf is declared as a node (derived, its default and description), and every qualifier's
    // node `<the spec's node>.<name>` that is not declared yet (default false, described as
    // "<the spec's description> -- <name>").
    bool Register(CommandSpec spec, std::string* why);

    // Adds a node that is not a command (`multivoid.command.selector`); refuses one declared.
    bool DeclareNode(NodeDecl d, std::string* why);

    // ASCII case-insensitive over root names and aliases; *alias is the alias that matched, or
    // null when the root's own name did.
    const CommandSpec* FindRoot(std::string_view word, const Alias** alias) const;

    // Every spec with a handler, depth-first: roots by name, sub-verbs in registration order.
    std::vector<const CommandSpec*> AllVerbs() const;

    const NodeDecl* FindNode(std::string_view node) const;

    // Read from the table filled at registration (a sub-verb has no parent link of its own);
    // empty for a spec this registry does not own.
    std::string NodeOf(const CommandSpec& c) const;
    std::string PathOf(const CommandSpec& c) const;  // the words from the root: `time set`

    // `/` + PathOf, then ` <name>` or ` [name]` per argument (`<name...>` / `[name...]` for Rest),
    // then the sub-verbs joined by `|`: in `<...>` when the spec has no handler of its own
    // (`/mv <user|group>`), in `[...]` when it has one (`/time [set|add]`).
    std::string Usage(const CommandSpec& c) const;

private:
    struct SpecInfo {
        std::string path;
        std::string node;
    };

    void Record(const CommandSpec& c, const std::string& parentPath, const std::string& parentNode);

    std::vector<std::unique_ptr<CommandSpec>> roots_;  // never moved or changed after Register
    std::map<std::string, NodeDecl, std::less<>> nodes_;
    std::set<std::string> qualifierNodes_;  // the nodes of nodes_ that a qualifier declared
    std::unordered_map<const CommandSpec*, SpecInfo> info_;
};

}  // namespace coop::commands
