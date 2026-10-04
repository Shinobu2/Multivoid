// coop/commands/commands_cases_settings.cpp -- the cases of coop/commands/settings_commands.h:
// each verb's replies and the arguments its setter receives, over fake ports that record into
// statics local to this file. Called from RunSelftest.

#include "coop/commands/command_dispatcher.h"
#include "coop/commands/command_registry.h"
#include "coop/commands/command_targets.h"
#include "coop/commands/commands_selftest.h"
#include "coop/commands/settings_commands.h"

#include <string>
#include <string_view>
#include <vector>

namespace coop::commands {
namespace {

using config::SetResult;
using Ports = settings::Ports;
namespace reg_ns = coop::config_registry;

// What one set of fake ports received and what it answers.
struct Rec {
    SetResult result = SetResult::Saved;
    bool validOk = true;
    std::string why;
    std::string current;
    int sets = 0;
    int resets = 0;
    std::string setKey;
    std::string setValue;
    std::string resetKey;
    std::string validValue;
};

// Two sets of ports with separate records: `Id` is only a name for the statics. The server row is
// the real voice range; the local row the real display name, which the fake never finds.
template <int Id>
struct Fakes {
    static Rec& R() {
        static Rec rec;
        return rec;
    }
    static const char* CredentialKey(std::string_view key) {
        return key == "net.lobby_password" ? "net.lobby_password" : nullptr;
    }
    static const reg_ns::Row* FindServerRow(std::string_view key) {
        return key == "voice.distance_cm" ? reg_ns::rows::voice_distance_cm.row : nullptr;
    }
    static bool Valid(const reg_ns::Row*, const std::string& value, std::string* why) {
        R().validValue = value;
        if (!R().validOk && why) *why = R().why;
        return R().validOk;
    }
    static SetResult Set(const reg_ns::Row* row, const char* value) {
        R().sets++;
        R().setKey = row->key;
        R().setValue = value;
        return R().result;
    }
    static SetResult Reset(const reg_ns::Row* row) {
        R().resets++;
        R().resetKey = row->key;
        return R().result;
    }
    static std::string Current(const reg_ns::Row*) { return R().current; }
    static Ports MakePorts() {
        Ports p;
        p.credentialKey = &CredentialKey;
        p.findServerRow = &FindServerRow;
        p.valid = &Valid;
        p.set = &Set;
        p.reset = &Reset;
        p.current = &Current;
        return p;
    }
};

bool AllowAll(const Caller&, std::string_view, bool) { return true; }
bool DenyAll(const Caller&, std::string_view, bool) { return false; }

Policy PolicyWith(CheckFn check) {
    Policy p;
    p.check = check;
    return p;
}

bool Said(const DispatchResult& r, const char* line) {
    return r.ran && r.replies.size() == 1 && r.replies[0] == line;
}

// Run `line` with the fake answering `result`, the value it reads back being `current`.
template <int Id>
DispatchResult RunAs(const Registry& reg, const char* line, SetResult result, const char* current = "") {
    Fakes<Id>::R() = Rec{};
    Fakes<Id>::R().result = result;
    Fakes<Id>::R().current = current;
    return Dispatch(reg, Caller{0, 0, true}, line, {}, PolicyWith(&AllowAll));
}

void SetCases(Checker& check, const Registry& reg) {
    using F = Fakes<1>;
    check(Said(RunAs<1>(reg, "set voice.distance_cm 6000", SetResult::Saved, "6000"),
               "voice.distance_cm is now 6000.") &&
              F::R().sets == 1 && F::R().resets == 0 && F::R().setKey == "voice.distance_cm" &&
              F::R().setValue == "6000" && F::R().validValue == "6000",
          "settings: /set validates the typed value, sets the row and says what it resolves to now");
    check(Said(RunAs<1>(reg, "set voice.distance_cm 6000", SetResult::HeldNotSaved, "6000"),
               "voice.distance_cm is now 6000 for this game; the settings file could not be written."),
          "settings: /set answers a set that was held and not saved");
    check(Said(RunAs<1>(reg, "set voice.distance_cm 6000", SetResult::Refused),
               "voice.distance_cm was not changed.") &&
              F::R().sets == 1,
          "settings: /set answers a set the setter refused");
    check(Said(RunAs<1>(reg, "set voice.distance_cm 1 2", SetResult::Saved, "1 2"),
               "voice.distance_cm is now 1 2.") &&
              F::R().setValue == "1 2",
          "settings: /set passes the raw remainder of the line as the value");
    {
        Fakes<1>::R() = Rec{};
        F::R().validOk = false;
        F::R().why = "not a number in [0, 1e+06]";
        const DispatchResult r =
            Dispatch(reg, Caller{0, 0, true}, "set voice.distance_cm banana", {}, PolicyWith(&AllowAll));
        check(Said(r, "voice.distance_cm: not a number in [0, 1e+06]") && F::R().sets == 0 &&
                  F::R().validValue == "banana",
              "settings: /set refuses a value the reader refuses, with its reason, and sets nothing");
    }
    check(Said(RunAs<1>(reg, "set nosuch.key 1", SetResult::Saved), "No server setting is named nosuch.key.") &&
              F::R().sets == 0,
          "settings: /set of an unknown key says so and sets nothing");
}

void ResetCases(Checker& check, const Registry& reg) {
    using F = Fakes<1>;
    check(Said(RunAs<1>(reg, "reset voice.distance_cm", SetResult::Saved, "4800"),
               "voice.distance_cm is back to 4800.") &&
              F::R().resets == 1 && F::R().sets == 0 && F::R().resetKey == "voice.distance_cm",
          "settings: /reset resets the row and says what it resolves to now");
    check(Said(RunAs<1>(reg, "reset voice.distance_cm", SetResult::HeldNotSaved, "6000"),
               "voice.distance_cm could not be reset: the settings file could not be written, so its "
               "stored value still answers."),
          "settings: /reset answers a reset that was held and not saved");
    check(Said(RunAs<1>(reg, "reset voice.distance_cm", SetResult::Refused),
               "voice.distance_cm was not changed."),
          "settings: /reset answers a reset the setter refused");
    check(Said(RunAs<1>(reg, "reset nosuch.key", SetResult::Saved), "No server setting is named nosuch.key.") &&
              F::R().resets == 0,
          "settings: /reset of an unknown key says so and resets nothing");
}

void RefusalCases(Checker& check, const Registry& reg) {
    using F = Fakes<1>;
    check(Said(RunAs<1>(reg, "set net.nick Bob", SetResult::Saved), "No server setting is named net.nick.") &&
              F::R().sets == 0,
          "settings: /set of a local row is refused, the ports finding no server row for it");
    check(Said(RunAs<1>(reg, "reset net.nick", SetResult::Saved), "No server setting is named net.nick.") &&
              F::R().resets == 0,
          "settings: /reset of a local row is refused, the ports finding no server row for it");
    {
        const DispatchResult r = RunAs<1>(reg, "set net.lobby_password hunter2", SetResult::Saved);
        check(Said(r, "net.lobby_password is changed in its own screen, not by command.") &&
                  F::R().sets == 0 && F::R().validValue.empty() &&
                  r.replies[0].find("hunter2") == std::string::npos,
              "settings: /set of a credential row answers with its own reply and never echoes the value");
    }
    check(Said(RunAs<1>(reg, "reset net.lobby_password", SetResult::Saved),
               "net.lobby_password is changed in its own screen, not by command.") &&
              F::R().resets == 0,
          "settings: /reset of a credential row answers with its own reply");
}

}  // namespace

void SettingsCases(Checker& check) {
    Registry reg;
    check(settings::Register(reg, Fakes<1>::MakePorts()), "settings: the two roots register");
    check(reg.FindRoot("set", nullptr) != nullptr && reg.FindRoot("reset", nullptr) != nullptr,
          "settings: /set and /reset are roots");
    {
        const CommandSpec* set = reg.FindRoot("set", nullptr);
        const CommandSpec* reset = reg.FindRoot("reset", nullptr);
        const NodeDecl* node = reg.FindNode("multivoid.set");
        check(set != nullptr && reset != nullptr && reg.NodeOf(*set) == "multivoid.set" &&
                  reg.NodeOf(*reset) == "multivoid.set" && node != nullptr &&
                  !node->defaultGranted && reg.FindNode("multivoid.reset") == nullptr,
              "settings: /reset names /set's node, which is declared and not granted by default");
        check(set != nullptr && reg.Usage(*set) == "/set <setting> <value...>" && reset != nullptr &&
                  reg.Usage(*reset) == "/reset <setting>",
              "settings: the usage lines name the setting and the value");
    }

    SetCases(check, reg);
    ResetCases(check, reg);
    RefusalCases(check, reg);

    {
        Fakes<1>::R() = Rec{};
        const DispatchResult r =
            Dispatch(reg, Caller{1, 7, false}, "set voice.distance_cm 6000", {}, PolicyWith(&DenyAll));
        check(!r.ran && Fakes<1>::R().sets == 0 && Fakes<1>::R().validValue.empty(),
              "settings: a caller the permission check refuses reaches no setter");
    }
    {
        Registry other;
        check(settings::Register(other, Fakes<2>::MakePorts()), "settings: a second registry registers");
        Fakes<1>::R() = Rec{};
        Fakes<2>::R() = Rec{};
        const DispatchResult a =
            Dispatch(reg, Caller{0, 0, true}, "set voice.distance_cm 1", {}, PolicyWith(&AllowAll));
        const DispatchResult b =
            Dispatch(other, Caller{0, 0, true}, "reset voice.distance_cm", {}, PolicyWith(&AllowAll));
        check(a.ran && b.ran && Fakes<1>::R().sets == 1 && Fakes<1>::R().resets == 0 &&
                  Fakes<2>::R().sets == 0 && Fakes<2>::R().resets == 1,
              "settings: two registries with different ports each call their own");
    }
}

}  // namespace coop::commands
