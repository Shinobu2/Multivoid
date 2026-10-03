// coop/commands/command_targets.h -- who a word in a command line names.
//
// Engine-free (no ue_wrap include). The host builds the PlayerView list when a command runs and
// hands it in; nothing here knows where it came from. A resolver that picks the wrong player
// kicks the wrong player, so the order of the forms below is the contract.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace coop::commands {

struct PlayerView {
    int slot = -1;
    unsigned playerNo = 0;   // the TAB number: minted by the host, never reused within a session
    std::string nick;        // UTF-8
    std::string playerId;    // 32 lower-case hex; empty when unknown
    bool hasPosition = false;
    float x = 0, y = 0, z = 0;
};

struct Caller {
    int slot = -1;           // -1 is the console
    uint32_t generation = 0; // the slot's occupancy generation: the address of a later reply
    bool isOperator = false;
};

enum class TargetError : uint8_t { None, NoMatch, Ambiguous, TooMany, NoCaller, NoPosition };

struct TargetResult {
    std::vector<int> slots;                // ascending
    std::vector<std::string> playerIds;    // beside slots, index for index
    TargetError error = TargetError::None;
    std::vector<int> matches;              // the candidates when Ambiguous, ascending
    bool usedSelector = false;             // @a or @r
};

// In order, the first form that applies: an empty word (nobody); `@s`, `@a`, `@p`, `@r` (any other
// `@` word names nobody); `#<player number>`; a player id (8 to 32 hex digits, a prefix of exactly
// one proved id); an exact nick; a unique nick substring. Nicks compare by coop::text::CaseFold per
// codepoint, never by the OS's locale. `one` is a verb whose argument is a single player: `@a`
// over more than one refuses (TooMany). `pick` is the only source of chance (`@r`); a null `pick`
// names nobody. The result holds players in any state; a verb refuses a loading one. Pure.
TargetResult ResolveTarget(std::string_view word, bool one, const Caller& caller,
                           const std::vector<PlayerView>& players, int (*pick)(int count));

// The line a caller is shown for a failed resolution; empty for TargetError::None. Pure.
std::string DescribeTargetError(const TargetResult& r, std::string_view word,
                                const std::vector<PlayerView>& players);

}  // namespace coop::commands
