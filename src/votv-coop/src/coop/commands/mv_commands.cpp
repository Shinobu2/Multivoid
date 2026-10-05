// coop/commands/mv_commands.cpp -- see coop/commands/mv_commands.h.
//
// The tree, the leaf table, the texts and the notice are ported from LuckPerms (MIT,
// THIRD-PARTY-NOTICES.md, LuckPerms): the `/lp` tree of `user|group <x> permission set|unset|
// settemp|unsettemp` and `parent add|remove` (reference/LuckPerms/common/src/main/java/me/lucko/
// luckperms/common/command/abstraction/ParentCommand.java:61-121, commands/generic/permission/
// CommandPermission.java:37-45), the texts of luckperms_en.properties (:178, :207-212, :305: "You
// cannot delete the default group.") and the notice every online holder of the log node but the
// actor receives (actionlog/LogDispatcher.java:70-81). Deliberate divergences: a settemp over a
// timed node of the same identity replaces it (LuckPerms offers a merge modifier), and the actor
// is never sent the notice: its own line is its reply.

#include "coop/commands/mv_commands.h"

#include "coop/commands/command_dispatcher.h"
#include "coop/permissions/model.h"
#include "coop/permissions/node.h"
#include "coop/permissions/permission_edit.h"

#include <algorithm>
#include <climits>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace coop::commands::mv {
namespace {

namespace perm = coop::permissions;

using ChangeFn = std::function<bool(perm::Model&, std::string*)>;

std::string Lowered(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    }
    return s;
}

std::string ShortId(const std::string& id) { return id.substr(0, 8); }

const PlayerView* ViewOf(const std::vector<PlayerView>& players, int slot) {
    for (const PlayerView& v : players)
        if (v.slot == slot) return &v;
    return nullptr;
}

// A player as a person reads it: the seated nick, else the seen-players record's, else the first
// eight characters of the id.
std::string UserDisplay(const Ports& p, const Context& ctx, const std::string& id) {
    for (const PlayerView& v : ctx.players)
        if (v.playerId == id && !v.nick.empty()) return v.nick;
    const std::string recorded = p.recordNick != nullptr ? p.recordNick(id) : std::string();
    return recorded.empty() ? ShortId(id) : recorded;
}

// ` <k>=<v>` for each context, in ContextSet::Pairs() order: the stored, normalised form.
std::string ContextWords(const perm::ContextSet& contexts) {
    std::string out;
    for (const auto& pair : contexts.Pairs()) out += " " + pair.first + "=" + pair.second;
    return out;
}

// Whether the user holds a true, global `group.*` node of a group other than `default`, of any
// expiry: the model's default step counts the same nodes, so removing `default` from a user with none
// would be put straight back. A player with no user holds only the default.
bool HoldsOtherGroup(const perm::Holder* user) {
    if (user == nullptr) return false;
    for (const perm::Node& n : user->nodes.Nodes()) {
        if (n.value && n.contexts.Empty() && perm::KindOf(n.key) == perm::NodeKind::Inheritance &&
            perm::GroupOf(n.key) != perm::kDefaultGroup)
            return true;
    }
    return false;
}

// The edit one changing leaf asks the host for: the holder, the closure over the copy, the canonical
// line (after `/mv `) and what the action says it acted on.
struct Edit {
    perm::HolderKey key{perm::HolderKind::User, std::string()};
    ChangeFn change;
    std::string canonical;
    std::string targetType, targetId, targetName;
};

// "<nick>: <canonical>" to every notify slot the dispatcher listed and, when a client acted, to the
// host's own feed when it holds the log node (the host's reply is its own).
void NotifyChange(const Ports& p, const Context& ctx, const std::string& line) {
    for (const PlayerView& v : ctx.players) {
        if (std::find(ctx.notifySlots.begin(), ctx.notifySlots.end(), v.slot) == ctx.notifySlots.end())
            continue;
        Caller to;
        to.slot = v.slot;
        to.generation = v.generation;
        p.notify(to, line);
    }
    if (ctx.caller.slot > 0 && p.hostHoldsLog()) {
        Caller host;
        host.slot = 0;
        host.isOperator = true;
        p.notify(host, line);
    }
}

// Hands the host the edit, sends every line it composed, and tells the others of a change.
void RunEdit(const Ports& p, Context& ctx, Edit edit) {
    const PlayerView* actor = ViewOf(ctx.players, ctx.caller.slot);
    const std::string actorNick = actor != nullptr ? actor->nick : std::string("Someone");
    perm::Action action;
    action.sourceId = ctx.caller.playerId;
    action.sourceName = actorNick;
    action.targetType = edit.targetType;
    action.targetId = edit.targetId;
    action.targetName = edit.targetName;
    action.description = "/mv " + edit.canonical;

    // Every declared node, for the owner invariant: no delegate may take one from the host.
    std::vector<std::string> nodes;
    ctx.registry.ForEachNode([&](const NodeDecl& d) { nodes.push_back(d.node); });

    const perm::host::ApplyResult result = p.apply(edit.key, edit.change, ctx.caller.isOperator, nodes, action);
    if (result.outcome == perm::host::ApplyOutcome::NoSession) {
        ctx.Reply(kNoSession);
        return;
    }
    for (const std::string& line : result.replies) ctx.Reply(line);
    if (result.outcome == perm::host::ApplyOutcome::Changed)
        NotifyChange(p, ctx, "[mv] " + actorNick + ": " + edit.canonical);
}

enum class Op : uint8_t { Set, Unset, SetTemp, UnsetTemp, ParentAdd, ParentRemove, SetWeight };

// The holder a `user <who>` or `group <name>` leaf acts on. False with `*refusal` for a group word
// that is no group name. `key.name` is the proved id (or the offline id), or the word lower-cased.
bool HolderOf(const Ports& p, const Context& ctx, bool isUser, Edit* edit, std::string* refusal) {
    if (isUser) {
        const TargetResult& t = ctx.targets[0];
        const std::string id = !t.slots.empty() ? t.playerIds[0] : t.offlineId;
        edit->key = {perm::HolderKind::User, id};
        edit->targetType = "user";
        edit->targetId = id;
        edit->targetName = UserDisplay(p, ctx, id);
        return true;
    }
    const std::string name = Lowered(ctx.texts[0]);
    if (!perm::IsValidGroupName(name)) {
        *refusal = "'" + ctx.texts[0] + "' is not a valid group name.";
        return false;
    }
    edit->key = {perm::HolderKind::Group, name};
    edit->targetType = "group";
    edit->targetId = name;
    edit->targetName = name;
    return true;
}

// `<kind> <holder> ` of the canonical line: `user <id> ` or `group <name> `.
std::string HolderWords(bool isUser, const Edit& edit) {
    return std::string(isUser ? "user " : "group ") + edit.key.name + " ";
}

// permission set | unset | settemp | unsettemp: the node (as stored), the value, the expiry, the contexts.
void BuildPermission(const Context& ctx, Op op, bool isUser, Edit* edit) {
    const bool setting = op == Op::Set || op == Op::SetTemp;
    const std::string word = ctx.texts[1];
    std::string node;
    const bool nodeOk = perm::NormalizeKey(word, &node);
    const bool value = setting && ctx.given[2] ? ctx.booleans[2] : true;
    const long long expiry = op == Op::SetTemp ? ctx.integers[3] : (op == Op::UnsetTemp ? 1 : 0);
    const perm::ContextSet contexts = ctx.contexts;
    const char* verb = op == Op::Set ? "set" : op == Op::Unset ? "unset" : op == Op::SetTemp ? "settemp" : "unsettemp";

    edit->canonical = HolderWords(isUser, *edit) + "permission " + verb + " " + (nodeOk ? node : word);
    if (setting) edit->canonical += value ? " true" : " false";
    if (op == Op::SetTemp) edit->canonical += " until " + std::to_string(expiry);
    edit->canonical += ContextWords(contexts);

    const perm::HolderKey key = edit->key;
    edit->change = [=](perm::Model& copy, std::string* why) {
        if (!nodeOk) {
            *why = "'" + word + "' is not a valid permission.";
            return false;
        }
        if (key.kind == perm::HolderKind::Group && copy.FindGroup(key.name) == nullptr) {
            *why = "No group named " + key.name + ".";
            return false;
        }
        perm::Node n;
        n.key = node;
        n.value = value;
        n.expiry = expiry;
        n.contexts = contexts;
        // A setter's own false is "already so": the plan answers NoChange, it is no refusal.
        if (setting) copy.SetNode(key.kind, key.name, n);
        else copy.UnsetNode(key.kind, key.name, n);
        return true;
    };
}

// parent add | remove <group>.
void BuildParent(const Context& ctx, Op op, bool isUser, Edit* edit) {
    const std::string group = Lowered(ctx.texts[1]);
    edit->canonical = HolderWords(isUser, *edit) + "parent " + (op == Op::ParentAdd ? "add " : "remove ") + group;
    const perm::HolderKey key = edit->key;
    edit->change = [=](perm::Model& copy, std::string* why) {
        if (key.kind == perm::HolderKind::Group && copy.FindGroup(key.name) == nullptr) {
            *why = "No group named " + key.name + ".";
            return false;
        }
        perm::Node n;
        n.key = "group." + group;
        if (op == Op::ParentAdd) {
            if (copy.FindGroup(group) == nullptr) {
                *why = "No group named " + group + ".";
                return false;
            }
            copy.SetNode(key.kind, key.name, n);
            return true;
        }
        if (key.kind == perm::HolderKind::User && group == perm::kDefaultGroup &&
            !HoldsOtherGroup(copy.FindUser(key.name))) {
            *why = "A player is always in a group: add another before removing default.";
            return false;
        }
        copy.UnsetNode(key.kind, key.name, n);
        return true;
    };
}

// group setweight <n>: every weight node of the group goes, the new one is set.
void BuildWeight(const Context& ctx, Edit* edit) {
    const std::string word = ctx.texts[1];
    const long long weight = ctx.integers[1];
    edit->canonical = HolderWords(false, *edit) + "setweight " + std::to_string(weight);
    const perm::HolderKey key = edit->key;
    edit->change = [=](perm::Model& copy, std::string* why) {
        const perm::Holder* group = copy.FindGroup(key.name);
        if (group == nullptr) {
            *why = "No group named " + key.name + ".";
            return false;
        }
        if (weight < INT_MIN || weight > INT_MAX) {
            *why = "'" + word + "' is not a weight: a whole number from -2147483648 to 2147483647.";
            return false;
        }
        // The nodes are copied first: unsetting changes the group the loop would walk.
        const std::vector<perm::Node> nodes = group->nodes.Nodes();
        for (const perm::Node& n : nodes) {
            if (perm::KindOf(n.key) == perm::NodeKind::Weight) copy.UnsetNode(key.kind, key.name, n);
        }
        perm::Node n;
        n.key = "weight." + std::to_string(weight);
        copy.SetNode(key.kind, key.name, n);
        return true;
    };
}

// A leaf that changes a holder of `isUser`'s kind.
Handler HolderLeaf(const Ports& p, bool isUser, Op op) {
    return [p, isUser, op](Context& ctx) {
        if (!p.hosted()) { ctx.Reply(kNoSession); return; }
        Edit edit;
        std::string refusal;
        if (!HolderOf(p, ctx, isUser, &edit, &refusal)) { ctx.Reply(refusal); return; }
        switch (op) {
            case Op::Set:
            case Op::Unset:
            case Op::SetTemp:
            case Op::UnsetTemp: BuildPermission(ctx, op, isUser, &edit); break;
            case Op::ParentAdd:
            case Op::ParentRemove: BuildParent(ctx, op, isUser, &edit); break;
            case Op::SetWeight: BuildWeight(ctx, &edit); break;
        }
        RunEdit(p, ctx, std::move(edit));
    };
}

// creategroup <name> and deletegroup <name>: the group is the leaf's own argument.
Handler GroupLifeLeaf(const Ports& p, bool create) {
    return [p, create](Context& ctx) {
        if (!p.hosted()) { ctx.Reply(kNoSession); return; }
        const std::string word = ctx.texts[0];
        const std::string name = Lowered(word);
        Edit edit;
        edit.key = {perm::HolderKind::Group, name};
        edit.targetType = "group";
        edit.targetId = name;
        edit.targetName = name;
        edit.canonical = std::string(create ? "creategroup " : "deletegroup ") + name;
        edit.change = [=](perm::Model& copy, std::string* why) {
            if (create) {
                if (!perm::IsValidGroupName(name)) {
                    *why = "'" + word + "' is not a valid group name.";
                    return false;
                }
                if (!copy.CreateGroup(name)) {
                    *why = "A group named " + name + " already exists.";
                    return false;
                }
                return true;
            }
            if (name == perm::kDefaultGroup) {
                *why = "You cannot delete the default group.";
                return false;
            }
            if (copy.FindGroup(name) == nullptr) {
                *why = "No group named " + name + ".";
                return false;
            }
            copy.DeleteGroup(name);
            return true;
        };
        RunEdit(p, ctx, std::move(edit));
    };
}

constexpr const char* kBrokenStore =
    "The permission files did not load at the host start: fix them, then /mv reload.";

// `info` of a user or a group: the stored holder of the live model, read within this call. A broken
// store has loaded nothing, so there is nothing to show.
Handler InfoLeaf(const Ports& p, bool isUser) {
    return [p, isUser](Context& ctx) {
        if (!p.hosted()) { ctx.Reply(kNoSession); return; }
        if (p.storeBroken()) { ctx.Reply(kBrokenStore); return; }
        Edit holder;
        std::string refusal;
        if (!HolderOf(p, ctx, isUser, &holder, &refusal)) { ctx.Reply(refusal); return; }
        for (const std::string& line : InfoLines(p.live(), holder.key, holder.targetName, p.now()))
            ctx.Reply(line);
    };
}

Handler ListGroupsLeaf(const Ports& p) {
    return [p](Context& ctx) {
        if (!p.hosted()) { ctx.Reply(kNoSession); return; }
        if (p.storeBroken()) { ctx.Reply(kBrokenStore); return; }
        for (const std::string& line : ListGroupLines(p.live(), p.now())) ctx.Reply(line);
    };
}

// reload changes no file: no action line and no notice, the host logs who ran it. An empty answer
// is the host saying there is no hosted session.
Handler ReloadLeaf(const Ports& p) {
    return [p](Context& ctx) {
        if (!p.hosted()) { ctx.Reply(kNoSession); return; }
        const std::vector<std::string> lines = p.reload(ctx.caller.playerId);
        if (lines.empty()) { ctx.Reply(kNoSession); return; }
        for (const std::string& line : lines) ctx.Reply(line);
    };
}

// A spec of the tree. Every changing leaf carries a Notify qualifier on the log node; the user
// leaves carry the offline gate their PlayerOrId target needs.
CommandSpec Spec(const char* name, std::string description) {
    CommandSpec c;
    c.name = name;
    c.description = std::move(description);
    return c;
}

CommandSpec Leaf(const char* name, std::string description, std::vector<ArgSpec> args, Handler handler,
                 bool isUser, bool notifies) {
    CommandSpec c = Spec(name, std::move(description));
    c.args = std::move(args);
    c.handler = std::move(handler);
    if (isUser) c.qualifiers.push_back({"offline", QualKind::GateOffline, std::string()});
    if (notifies) c.qualifiers.push_back({"notify", QualKind::Notify, kAdminLogNode});
    return c;
}

const ArgSpec kNodeArg{"node", ArgKind::Word, false};
const ArgSpec kValueArg{"value", ArgKind::Boolean, true};
const ArgSpec kForArg{"for", ArgKind::Duration, false};
const ArgSpec kContextsArg{"contexts", ArgKind::Contexts, true};
const ArgSpec kGroupArg{"group", ArgKind::Word, false};
const ArgSpec kWeightArg{"weight", ArgKind::Integer, false};
const ArgSpec kNameArg{"name", ArgKind::Word, false};

// `permission` and its four verbs, for a player or for a group.
CommandSpec PermissionSpec(const Ports& p, bool isUser) {
    const std::string a = isUser ? "a player" : "a group";
    const std::string whose = isUser ? "a player's" : "a group's";
    CommandSpec c = Spec("permission", "The permissions set on a holder.");
    c.subVerbs.push_back(Leaf("set", "Sets a permission on " + a + ".", {kNodeArg, kValueArg, kContextsArg},
                              HolderLeaf(p, isUser, Op::Set), isUser, true));
    c.subVerbs.push_back(Leaf("unset", "Removes a permission from " + a + ".", {kNodeArg, kContextsArg},
                              HolderLeaf(p, isUser, Op::Unset), isUser, true));
    c.subVerbs.push_back(Leaf("settemp", "Sets a permission on " + a + " for a time.",
                              {kNodeArg, kValueArg, kForArg, kContextsArg},
                              HolderLeaf(p, isUser, Op::SetTemp), isUser, true));
    c.subVerbs.push_back(Leaf("unsettemp", "Removes " + whose + " timed permission.", {kNodeArg, kContextsArg},
                              HolderLeaf(p, isUser, Op::UnsetTemp), isUser, true));
    return c;
}

// `parent` and its two verbs.
CommandSpec ParentSpec(const Ports& p, bool isUser) {
    CommandSpec c = Spec("parent", "The groups a holder inherits.");
    c.subVerbs.push_back(Leaf("add", isUser ? "Puts a player in a group." : "Makes a group inherit another.",
                              {kGroupArg}, HolderLeaf(p, isUser, Op::ParentAdd), isUser, true));
    c.subVerbs.push_back(Leaf("remove", isUser ? "Takes a player out of a group." : "Stops a group inheriting another.",
                              {kGroupArg}, HolderLeaf(p, isUser, Op::ParentRemove), isUser, true));
    return c;
}

// `user <who>` and `group <name>`: a targeted parent whose one argument is the holder.
CommandSpec HolderSpec(const Ports& p, bool isUser) {
    CommandSpec c = Spec(isUser ? "user" : "group",
                         isUser ? "A player's groups and permissions." : "A group's parents, weight and permissions.");
    c.args = {isUser ? ArgSpec{"who", ArgKind::PlayerOrId, false} : kNameArg};
    c.subVerbs.push_back(PermissionSpec(p, isUser));
    c.subVerbs.push_back(ParentSpec(p, isUser));
    if (!isUser) {
        c.subVerbs.push_back(Leaf("setweight", "Sets a group's weight (the highest wins).", {kWeightArg},
                                  HolderLeaf(p, false, Op::SetWeight), false, true));
    }
    c.subVerbs.push_back(Leaf("info",
                              isUser ? "Shows a player's groups and permissions."
                                     : "Shows a group's parents, weight and permissions.",
                              {}, InfoLeaf(p, isUser), isUser, false));
    return c;
}

}  // namespace

bool Register(Registry& reg, const Ports& p) {
    CommandSpec mv = Spec("mv", "Manages permissions (LuckPerms' /lp).");
    mv.subVerbs.push_back(HolderSpec(p, true));
    mv.subVerbs.push_back(HolderSpec(p, false));
    mv.subVerbs.push_back(Leaf("creategroup", "Creates a group.", {kNameArg}, GroupLifeLeaf(p, true), false, true));
    mv.subVerbs.push_back(Leaf("deletegroup", "Deletes a group no one names.", {kNameArg}, GroupLifeLeaf(p, false),
                               false, true));
    mv.subVerbs.push_back(Leaf("listgroups", "Lists the groups.", {}, ListGroupsLeaf(p), false, false));
    mv.subVerbs.push_back(Leaf("reload", "Reloads the permission files from disk.", {}, ReloadLeaf(p), false, false));
    return reg.Register(std::move(mv), nullptr);
}

}  // namespace coop::commands::mv
