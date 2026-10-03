// coop/commands/commands_selftest.cpp -- the un-gated boot selftest of coop/commands/, run once
// per session start on both peers (shape: coop/net/stream_slot_selftest.cpp). A resolver that picks
// the wrong player kicks the wrong player, and a wrong pick reads as working until it does.
//
// Red arm: with the dev row selftest_break_commands the FIRST case expects the wrong word count,
// so the run must fail.

#include "coop/commands/commands_selftest.h"

#include "coop/commands/command_line.h"
#include "coop/commands/command_targets.h"
#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "ue_wrap/core/log.h"

#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

namespace coop::commands {

namespace {

// UTF-8 spelled out as bytes: the file's own encoding is not what is under test.
const char kLQuote[] = "\xE2\x80\x9C";                  // U+201C
const char kRQuote[] = "\xE2\x80\x9D";                  // U+201D
const char kIvanUpper[] = "\xD0\x98\xD0\xB2\xD0\xB0\xD0\xBD";  // U+0418 U+0432 U+0430 U+043D
const char kIvanLower[] = "\xD0\xB8\xD0\xB2\xD0\xB0\xD0\xBD";  // U+0438 U+0432 U+0430 U+043D

struct Checker {
    int pass = 0, total = 0;
    void operator()(bool ok, const char* what) {
        ++total;
        if (ok) { ++pass; return; }
        UE_LOGE("commands selftest FAIL: %s", what);
    }
};

bool SameWords(const ParsedLine& p, std::initializer_list<const char*> want) {
    if (p.words.size() != want.size() || p.begins.size() != want.size()) return false;
    size_t i = 0;
    for (const char* w : want)
        if (p.words[i++] != w) return false;
    return true;
}

bool SameBegins(const ParsedLine& p, std::initializer_list<size_t> want) {
    if (p.begins.size() != want.size()) return false;
    size_t i = 0;
    for (size_t b : want)
        if (p.begins[i++] != b) return false;
    return true;
}

PlayerView MakePlayer(int slot, unsigned no, const char* nick, const std::string& id,
                      bool hasPos, float x) {
    PlayerView p;
    p.slot = slot;
    p.playerNo = no;
    p.nick = nick;
    p.playerId = id;
    p.hasPosition = hasPos;
    p.x = x;
    return p;
}

std::string Repeat(const char* s, int times) {
    std::string out;
    for (int i = 0; i < times; ++i) out += s;
    return out;
}

std::vector<PlayerView> FourPlayers() {
    return {MakePlayer(0, 1, "Host", Repeat("a", 32), true, 0),
            MakePlayer(1, 2, "Bob", Repeat("b1", 16), true, 100),
            MakePlayer(2, 5, "bobby", Repeat("b2", 16), true, 50),
            MakePlayer(3, 4, kIvanUpper, Repeat("c", 32), false, 0)};
}

int PickOne(int) { return 1; }
int PickNine(int) { return 9; }

bool Resolves(const TargetResult& r, std::initializer_list<int> slots) {
    if (r.error != TargetError::None || r.slots.size() != slots.size() ||
        r.playerIds.size() != slots.size())
        return false;
    size_t i = 0;
    for (int s : slots)
        if (r.slots[i++] != s) return false;
    return true;
}

bool Matches(const TargetResult& r, std::initializer_list<int> slots) {
    if (r.error != TargetError::Ambiguous || r.matches.size() != slots.size()) return false;
    size_t i = 0;
    for (int s : slots)
        if (r.matches[i++] != s) return false;
    return true;
}

void SplitCases(Checker& check, bool breakIt) {
    {
        const ParsedLine p = SplitLine("kick Bob");
        const size_t expectedWords = breakIt ? 3 : 2;
        check(p.words.size() == expectedWords && SameWords(p, {"kick", "Bob"}) &&
                  SameBegins(p, {0, 5}),
              "split: kick Bob is two words at 0 and 5");
    }
    check(SameWords(SplitLine("  kick   Bob  "), {"kick", "Bob"}),
          "split: a run of spaces separates once, the ends make no word");
    {
        const ParsedLine p = SplitLine("ban \"Bob Smith\" griefing");
        check(SameWords(p, {"ban", "Bob Smith", "griefing"}) && SameBegins(p, {0, 4, 16}),
              "split: a quoted word keeps its spaces; begins is the opening quote");
    }
    check(SameWords(SplitLine("say \"unclosed text"), {"say", "unclosed text"}),
          "split: an unclosed quote runs to the end");
    check(SameWords(SplitLine("a\"b c"), {"a\"b", "c"}), "split: a quote inside a word is a byte");
    check(SameWords(SplitLine("x \"\""), {"x", ""}), "split: an empty quoted word is a word");
    check(SameWords(SplitLine("\"a b\"c"), {"a b", "c"}), "split: a closing quote ends its word");
    {
        const std::string curly = std::string("msg Bob ") + kLQuote + "hi there" + kRQuote;
        check(SameWords(SplitLine(curly), {"msg", "Bob", "hi there"}),
              "split: U+201C and U+201D quote like \"");
    }
    check(SplitLine("").words.empty() && SplitLine("   ").words.empty(),
          "split: nothing makes no words");
}

void TargetCases(Checker& check) {
    const std::vector<PlayerView> players = FourPlayers();
    Caller host;
    host.slot = 0;
    host.isOperator = true;
    Caller console;
    Caller slot3;
    slot3.slot = 3;
    auto resolve = [&](const char* word, bool one = true, const Caller& who = Caller{0, 0, true},
                       int (*pick)(int) = nullptr) {
        return ResolveTarget(word, one, who, players, pick);
    };

    check(resolve("").error == TargetError::NoMatch, "target: an empty word names nobody");
    check(Resolves(resolve("@s"), {0}), "target: @s is the caller");
    {
        const TargetResult r = resolve("@a", false);
        check(Resolves(r, {0, 1, 2, 3}) && r.usedSelector, "target: @a is everyone");
    }
    check(resolve("@a", true).error == TargetError::TooMany, "target: @a for one player refuses");
    check(Resolves(resolve("@p"), {2}), "target: @p is the nearest other player");
    check(resolve("@p", true, console).error == TargetError::NoCaller,
          "target: @p by the console needs a caller");
    check(resolve("@p", true, slot3).error == TargetError::NoPosition,
          "target: @p from a caller without a position");
    {
        const TargetResult r = resolve("@r", true, host, PickOne);
        check(Resolves(r, {1}) && r.usedSelector, "target: @r takes pick's index");
    }
    check(resolve("@r", true, host, PickNine).error == TargetError::NoMatch,
          "target: @r with a pick outside the range names nobody");
    check(Resolves(resolve("#5"), {2}), "target: #5 is player number 5");
    check(Resolves(resolve("#05"), {2}), "target: leading zeros allowed");
    check(Resolves(resolve("#2"), {1}), "target: #2 is player number 2");
    check(resolve("#7").error == TargetError::NoMatch, "target: #7 names nobody");
    check(resolve("#").error == TargetError::NoMatch, "target: # alone names nobody");
    check(resolve("#0").error == TargetError::NoMatch, "target: #0 names nobody");
    check(resolve("#65537").error == TargetError::NoMatch, "target: #65537 is past 16 bits");
    check(resolve("@x").error == TargetError::NoMatch, "target: an unknown selector names nobody");
    {
        const TargetResult r = resolve("bob");
        check(Resolves(r, {1}) && r.playerIds[0] == Repeat("b1", 16),
              "target: an exact nick beats the substring and carries its id");
    }
    check(Resolves(resolve("BOBBY"), {2}), "target: nicks fold case");
    {
        const TargetResult r = resolve("bo");
        check(Matches(r, {1, 2}), "target: a shared substring is ambiguous, ascending");
        check(DescribeTargetError(r, "bo", players) ==
                  "'bo' matches several players: Bob (#2), bobby (#5).",
              "target: the ambiguous line lists the matches with their # forms");
    }
    check(Resolves(resolve(kIvanLower), {3}), "target: a Cyrillic nick folds");
    check(Resolves(resolve("b2b2b2b2"), {2}), "target: an id prefix");
    check(Resolves(resolve("B2B2B2B2"), {2}), "target: an id prefix is lowered");
    check(resolve("b1b1").error == TargetError::NoMatch, "target: under 8 hex digits is a nick");
    check(resolve("zzz").error == TargetError::NoMatch, "target: a stranger names nobody");

    const std::vector<PlayerView> clash = {
        MakePlayer(0, 1, "b2b2b2b2", Repeat("a", 32), true, 0),
        MakePlayer(1, 2, "x", Repeat("b2", 16), true, 0)};
    check(Resolves(ResolveTarget("b2b2b2b2", true, host, clash, nullptr), {1}),
          "target: an id beats an identical nick");
}

}  // namespace

bool RunSelftest() {
    Checker check;
    const bool breakIt = coop::config::ResolveFlag(coop::config_registry::rows::selftest_break_commands);
    SplitCases(check, breakIt);
    TargetCases(check);

    if (check.pass == check.total) {
        UE_LOGI("commands selftest: ALL PASS (%d checks)", check.total);
        return true;
    }
    UE_LOGE("commands selftest: %d/%d checks passed", check.pass, check.total);
    return false;
}

}  // namespace coop::commands
