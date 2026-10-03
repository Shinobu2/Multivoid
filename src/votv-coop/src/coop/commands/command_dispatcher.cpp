// coop/commands/command_dispatcher.cpp -- Dispatch and the built-in /help.
//
// The order of a dispatch: split, find the root (an alias becomes its expansion), walk to the
// sub-verb, refuse a console-only verb to anyone else, ask the policy about the reached verb's
// node, parse and resolve the arguments, ask about the selector node when `@a` or `@r` was used,
// apply the qualifiers (the host as a target, an offline target, an exempt target, who is told),
// run the handler. Each refusal is one reply line and sets no `ran`.

#include "coop/commands/command_dispatcher.h"

#include "coop/commands/command_line.h"

#include <algorithm>
#include <charconv>
#include <cstddef>

namespace coop::commands {

namespace {

constexpr size_t kPresetOffset = static_cast<size_t>(-1);
constexpr const char* kSelectorNode = "multivoid.command.selector";

// The typed line and its effective words: for an alias, [root name] + the preset words + the
// words typed after the alias. A preset word has no offset in the typed line.
struct Expanded {
    ParsedLine typed;
    std::vector<std::string> words;
    std::vector<size_t> begins;  // kPresetOffset for a preset word
    size_t typedStart = 1;       // index in `words` of the first word typed after the alias
};

// The root the line's first word names, or null (unknown, or no word).
const CommandSpec* Expand(const Registry& reg, std::string_view line, Expanded& ex) {
    ex.typed = SplitLine(line);
    if (ex.typed.words.empty()) return nullptr;
    const Alias* alias = nullptr;
    const CommandSpec* root = reg.FindRoot(ex.typed.words[0], &alias);
    if (root == nullptr) return nullptr;
    if (alias == nullptr) {
        ex.words = ex.typed.words;
        ex.begins = ex.typed.begins;
        ex.typedStart = 1;
        return root;
    }
    ex.words.push_back(root->name);
    ex.begins.push_back(ex.typed.begins[0]);
    const ParsedLine preset = SplitLine(alias->presetWords);
    for (const std::string& w : preset.words) {
        ex.words.push_back(w);
        ex.begins.push_back(kPresetOffset);
    }
    ex.typedStart = ex.words.size();
    for (size_t i = 1; i < ex.typed.words.size(); ++i) {
        ex.words.push_back(ex.typed.words[i]);
        ex.begins.push_back(ex.typed.begins[i]);
    }
    return root;
}

struct Walked {
    const CommandSpec* spec;
    size_t next;  // the first effective word the sub-verbs did not consume
};

// While the next word equals (ASCII, case-insensitive) a sub-verb of the current spec, descend.
Walked Walk(const CommandSpec* root, const Expanded& ex) {
    Walked w{root, 1};
    while (w.next < ex.words.size()) {
        const CommandSpec* hit = nullptr;
        for (const CommandSpec& sub : w.spec->subVerbs)
            if (EqualsAsciiNoCase(ex.words[w.next], sub.name)) { hit = &sub; break; }
        if (hit == nullptr) break;
        w.spec = hit;
        ++w.next;
    }
    return w;
}

std::string TrimRight(std::string_view s) {
    size_t n = s.size();
    while (n > 0 && s[n - 1] == ' ') --n;
    return std::string(s.substr(0, n));
}

// The raw text from effective word `at` to the end of the line. A word typed in the line takes
// the line from its offset; a preset word has none, so the preset words from there are joined by
// one space, then one space and the raw typed remainder after the alias.
std::string RawRest(const Expanded& ex, size_t at, std::string_view line) {
    if (ex.begins[at] != kPresetOffset) return TrimRight(line.substr(ex.begins[at]));
    std::string out;
    for (size_t i = at; i < ex.typedStart; ++i) {
        if (!out.empty()) out += ' ';
        out += ex.words[i];
    }
    if (ex.typedStart < ex.words.size()) {
        out += ' ';
        out += line.substr(ex.begins[ex.typedStart]);
        return TrimRight(out);
    }
    return out;
}

// A leading `+` before a digit is skipped; from_chars reads a `-` itself and must take it all.
bool ParseInteger(std::string_view s, long long* out) {
    if (s.size() >= 2 && s[0] == '+' && s[1] >= '0' && s[1] <= '9') s.remove_prefix(1);
    long long v = 0;
    const char* end = s.data() + s.size();
    const auto r = std::from_chars(s.data(), end, v);
    if (r.ec != std::errc() || r.ptr != end) return false;
    *out = v;
    return true;
}

bool Passes(const Registry& reg, const Policy& policy, const Caller& caller, std::string_view node) {
    if (policy.check == nullptr) return false;
    const NodeDecl* d = reg.FindNode(node);
    return policy.check(caller, node, d != nullptr && d->defaultGranted);
}

DispatchResult Said(std::string line) {
    DispatchResult r;
    r.replies.push_back(std::move(line));
    return r;
}

const Qualifier* FindQualifier(const CommandSpec& spec, QualKind kind) {
    for (const Qualifier& q : spec.qualifiers)
        if (q.kind == kind) return &q;
    return nullptr;
}

std::string NickOf(const std::vector<PlayerView>& players, int slot) {
    for (const PlayerView& p : players)
        if (p.slot == slot) return p.nick;
    return std::string();
}

// A player id as a person reads it: its first eight characters.
std::string ShortId(const std::string& id) { return id.substr(0, 8); }

// The qualifier steps over the resolved targets, before the handler: the host as a target, an
// offline target, an exempt target, then who is told. Returns the refusal line, or empty when the
// handler may run (`ctx.notifySlots` is then filled). The offline, exempt and identity steps judge
// the spec's one Player / PlayerOrId argument, when it was given.
std::string ApplyQualifiers(Context& ctx) {
    const CommandSpec& spec = ctx.spec;
    const Caller& caller = ctx.caller;
    const Policy& policy = ctx.policy;
    const std::string node = ctx.registry.NodeOf(spec);

    // Register refuses a GateOffline or Exempt qualifier on a spec without exactly one such
    // argument, and a PlayerOrId argument on a spec without GateOffline.
    size_t at = spec.args.size();
    for (size_t i = 0; i < spec.args.size(); ++i) {
        const ArgKind kind = spec.args[i].kind;
        if (kind == ArgKind::Player || kind == ArgKind::PlayerOrId) at = i;
    }

    for (size_t i = 0; i < spec.args.size(); ++i) {
        if (!ctx.given[i] || !spec.args[i].notHost) continue;
        for (int slot : ctx.targets[i].slots)
            if (slot == 0) return "That is the host -- it cannot be " + spec.pastTense + ".";
    }

    if (at != spec.args.size() && ctx.given[at]) {
        const TargetResult& t = ctx.targets[at];
        const Qualifier* gate = FindQualifier(spec, QualKind::GateOffline);
        if (gate != nullptr && t.offline) {
            const std::string offlineNode = node + "." + gate->name;
            if (!Passes(ctx.registry, policy, caller, offlineNode))
                return "You do not have permission for /" + ctx.registry.PathOf(spec) +
                       " on an offline player (" + offlineNode + ").";
        }

        const Qualifier* exempt = FindQualifier(spec, QualKind::Exempt);
        if (exempt != nullptr && !caller.isOperator) {
            const std::string exemptNode = node + "." + exempt->name;
            for (size_t k = 0; k < t.slots.size(); ++k) {
                const std::string& id = t.playerIds[k];
                const std::string nick = NickOf(ctx.players, t.slots[k]);
                if (id.empty() || policy.isExplicit == nullptr)
                    return nick + "'s identity is not proved yet.";
                if (policy.isExplicit(id, exemptNode))
                    return nick + " cannot be " + spec.pastTense + ".";
            }
            if (t.offline) {
                if (policy.isExplicit == nullptr)
                    return ShortId(t.offlineId) + "'s identity is not proved yet.";
                if (policy.isExplicit(t.offlineId, exemptNode))
                    return ShortId(t.offlineId) + " cannot be " + spec.pastTense + ".";
            }
        }
    }

    const Qualifier* notify = FindQualifier(spec, QualKind::Notify);
    if (notify != nullptr && policy.holds != nullptr) {
        const std::string notifyNode = node + "." + notify->name;
        for (const PlayerView& p : ctx.players)
            if (p.slot > 0 && p.slot != caller.slot && !p.playerId.empty() &&
                policy.holds(p.playerId, notifyNode, false))
                ctx.notifySlots.push_back(p.slot);
        std::sort(ctx.notifySlots.begin(), ctx.notifySlots.end());
    }
    return std::string();
}

void HelpHandler(Context& ctx) {
    if (!ctx.given[0]) {
        ctx.Reply("Commands you can use:");
        for (const CommandSpec* v : ctx.registry.AllVerbs()) {
            if (v->consoleOnly && !ctx.caller.isOperator) continue;
            if (!Passes(ctx.registry, ctx.policy, ctx.caller, ctx.registry.NodeOf(*v))) continue;
            ctx.Reply(ctx.registry.Usage(*v) + " -- " + v->description);
        }
        return;
    }
    Expanded ex;
    const CommandSpec* root = Expand(ctx.registry, ctx.texts[0], ex);
    if (root == nullptr) {
        const std::string first = ex.typed.words.empty() ? std::string() : ex.typed.words[0];
        ctx.Reply("Unknown command '/" + first + "'.");
        return;
    }
    const Walked w = Walk(root, ex);
    ctx.Reply(ctx.registry.Usage(*w.spec) + " -- " + w.spec->description);
}

}  // namespace

DispatchResult Dispatch(const Registry& reg, const Caller& caller, std::string_view line,
                        const std::vector<PlayerView>& players, const Policy& policy) {
    Expanded ex;
    const CommandSpec* root = Expand(reg, line, ex);
    if (ex.typed.words.empty()) return Said(std::string(kHelpHint));
    if (root == nullptr)
        return Said("Unknown command '/" + ex.typed.words[0] + "'. Type /help for the commands.");

    const Walked walked = Walk(root, ex);
    const CommandSpec& spec = *walked.spec;
    if (spec.handler == nullptr) return Said("Usage: " + reg.Usage(spec));

    if (spec.consoleOnly && !caller.isOperator)
        return Said("/" + reg.PathOf(spec) + " is the host's until /tp can name a destination.");

    const std::string node = reg.NodeOf(spec);
    if (!Passes(reg, policy, caller, node))
        return Said("You do not have permission for /" + reg.PathOf(spec) + " (" + node + ").");

    const size_t argCount = spec.args.size();
    Context ctx{caller, spec, players, reg, policy, {}, {}, {}, {}, {}, {}};
    ctx.targets.resize(argCount);
    ctx.integers.assign(argCount, 0);
    ctx.texts.resize(argCount);
    ctx.given.assign(argCount, false);

    bool usedSelector = false;
    size_t next = walked.next;
    for (size_t i = 0; i < argCount; ++i) {
        const ArgSpec& a = spec.args[i];
        if (next >= ex.words.size()) {
            if (a.optional) continue;
            return Said("Usage: " + reg.Usage(spec));
        }
        ctx.given[i] = true;
        if (a.kind == ArgKind::Rest) {
            ctx.texts[i] = RawRest(ex, next, line);
            next = ex.words.size();
            continue;
        }
        const std::string& word = ex.words[next++];
        ctx.texts[i] = word;
        if (a.kind == ArgKind::Integer) {
            if (!ParseInteger(word, &ctx.integers[i]))
                return Said("'" + word + "' is not a whole number. Usage: " + reg.Usage(spec));
        } else if (a.kind == ArgKind::Player || a.kind == ArgKind::PlayerOrId ||
                   a.kind == ArgKind::Players) {
            TargetResult r = ResolveTarget(word, a.kind != ArgKind::Players,
                                           a.kind == ArgKind::PlayerOrId, caller, players, policy.pick);
            if (r.error != TargetError::None) return Said(DescribeTargetError(r, word, players));
            usedSelector = usedSelector || r.usedSelector;
            ctx.targets[i] = std::move(r);
        }
    }
    if (next < ex.words.size()) return Said("Usage: " + reg.Usage(spec));

    if (usedSelector && !Passes(reg, policy, caller, kSelectorNode))
        return Said(std::string("You do not have permission to use @a or @r (") + kSelectorNode + ").");

    const std::string refusal = ApplyQualifiers(ctx);
    if (!refusal.empty()) return Said(refusal);

    spec.handler(ctx);
    DispatchResult out;
    out.ran = true;
    out.replies = std::move(ctx.replies);
    return out;
}

bool RegisterBuiltins(Registry& reg) {
    if (!reg.DeclareNode({kSelectorNode, false, "Use @a and @r in a command's target."}, nullptr))
        return false;
    CommandSpec help;
    help.name = "help";
    help.defaultGranted = true;
    help.description = "Lists the commands you can use, or shows how to use one.";
    help.args = {{"command", ArgKind::Rest, true}};
    help.handler = &HelpHandler;
    return reg.Register(std::move(help), nullptr);
}

}  // namespace coop::commands
