// coop/bug_report/report_selftest.cpp -- the un-gated boot selftest of the bug report's pure
// core, run once per session start on both peers (shape: coop/commands/commands_selftest.cpp). A
// redactor that misses a shape leaves a stranger's address in a file a player sends, and nothing
// else would notice. It covers every pure function the report rests on: the redactor, the form
// check, the log-format test, the ini's report form, the printed form of a config value, and the
// endpoint and master-label functions.
//
// Red arm: with the dev row selftest_break_bug_report the first case expects the wrong text, so
// the run must fail. The fixtures are fabricated: 203.0.113.0/24, 198.51.100.0/24 and 2001:db8::/32
// are documentation ranges, and "Pat Example" is no one.

#include "coop/bug_report/report_core.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/config/config_report.h"
#include "coop/net/endpoint_log.h"
#include "coop/net/master_slots.h"
#include "coop/net/protocol.h"
#include "ue_wrap/core/log.h"

#include <cstring>
#include <string>
#include <vector>

namespace coop::bug_report {
namespace {

namespace EL = coop::net::endpoint_log;
namespace CR = coop::config_registry;

struct Checker {
    int total = 0, pass = 0;
    void operator()(bool ok, const char* what) {
        ++total;
        if (ok) { ++pass; return; }
        UE_LOGE("bug_report selftest FAIL: %s", what);
    }
};

std::string A(std::string_view v) { return ue_wrap::log::Addr(v); }

std::string Rep(const char* s, int times) {
    std::string out;
    for (int i = 0; i < times; ++i) out += s;
    return out;
}

RedactContext Fixture() {
    RedactContext c;
    c.localAppDataForms = {"C:\\Users\\Pat Example\\AppData\\Local"};
    c.profileForms = {"C:\\Users\\Pat Example"};
    c.selfPlayerId = "11111111222222223333333344444444";
    c.selfKey = "gen:" + std::string(64, 'b');
    return c;
}

std::string Run(const RedactContext& ctx, const std::string& in) {
    Redactor r(ctx);
    return r.Apply(in);
}

// The case a fresh Redactor answers.
void Case(Checker& check, const char* what, const std::string& in, const std::string& want,
          const RedactContext& ctx) {
    check(Run(ctx, in) == want, what);
}

void FolderCases(Checker& check, bool breakIt) {
    const RedactContext fx = Fixture();
    std::string firstWant = "backup -> '%LOCALAPPDATA%\\VotV\\Saved\\x' (39 file(s))";
    if (breakIt) firstWant += " (the wrong text)";
    Case(check, "local app data, backslashes",
         "backup -> 'C:\\Users\\Pat Example\\AppData\\Local\\VotV\\Saved\\x' (39 file(s))", firstWant, fx);
    Case(check, "local app data, slashes", "C:/Users/Pat Example/AppData/Local/VotV/",
         "%LOCALAPPDATA%/VotV/", fx);
    Case(check, "profile", "UE4SS host: 'C:\\Users\\Pat Example\\Downloads\\a09n\\ue4ss.dll'",
         "UE4SS host: '%USERPROFILE%\\Downloads\\a09n\\ue4ss.dll'", fx);
    Case(check, "profile, other case", "c:\\users\\pat example\\x", "%USERPROFILE%\\x", fx);
    Case(check, "a longer name is not the folder", "C:\\Users\\Pat Examples\\x",
         "C:\\Users\\Pat Examples\\x", fx);
    Case(check, "profile before a paren", "(C:\\Users\\Pat Example)", "(%USERPROFILE%)", fx);
    Case(check, "profile before a comma", "C:\\Users\\Pat Example, x", "%USERPROFILE%, x", fx);
    Case(check, "profile before a sentence's end", "at C:\\Users\\Pat Example.", "at %USERPROFILE%.", fx);
    Case(check, "a dot that continues the name", "C:\\Users\\Pat Example.old\\x",
         "C:\\Users\\Pat Example.old\\x", fx);
    Case(check, "profile before CR", "C:\\Users\\Pat Example\r", "%USERPROFILE%\r", fx);
    Case(check, "LocalLow falls to the profile form",
         "C:\\Users\\Pat Example\\AppData\\LocalLow\\x", "%USERPROFILE%\\AppData\\LocalLow\\x", fx);

    RedactContext forms = fx;
    forms.profileForms = {"C:\\\\Users\\\\Pat Example", "C:\\Users\\PATEXA~1"};
    Case(check, "doubled backslashes", "\"C:\\\\Users\\\\Pat Example\\\\x\"", "\"%USERPROFILE%\\\\x\"", forms);
    Case(check, "8.3 short form", "C:\\Users\\PATEXA~1\\Temp", "%USERPROFILE%\\Temp", forms);
}

void AddressCases(Checker& check) {
    const RedactContext fx = Fixture();
    {
        Redactor r(fx);
        check(r.Apply("ip=" + A("192.168.1.20") + ") -- 3 known") == "ip=addr#1(v4,lan)) -- 3 known",
              "an address mark, then text");
        check(r.Apply("dialed " + A("192.168.1.20") + ":47621 x") == "dialed addr#1(v4,lan):47621 x",
              "the same host keeps its number");
        check(r.Apply(A("192.168.1.20:47621")) == "addr#1(v4,lan)", "the same host with a port");
        check(r.Apply(A("203.0.113.7")) == "addr#2(v4,public)", "a second host is the next number");
        check(r.Counts().addresses == 4, "four marks, four counted");
    }
    struct Row { const char* what; const char* value; const char* want; };
    const Row rows[] = {
        {"v4 loopback", "127.0.0.1", "addr#1(v4,loopback)"},
        {"v6 loopback", "::1", "addr#1(v6,loopback)"},
        {"v6 link-local", "fe80::1", "addr#1(v6,lan)"},
        {"v6 in brackets with a port", "[2001:db8::5]:47621", "addr#1(v6,public)"},
        {"a name with a port", "Home.Example-1.org:10443", "addr#1(name)"},
        {"an empty value", "", "addr#1(empty)"},
        {"a value that is no address", "1.2.3:x:y", "addr#1(unparsed)"},
        {"a list entry", "default,Home=home.example.org:10443", "addr#1(unparsed)"},
        {"a turn endpoint", "turn:203.0.113.9:3478", "addr#1(v4,public)"},
        {"a url", "https://Home.Example.org:10443/x", "addr#1(name)"},
    };
    for (const Row& row : rows) Case(check, row.what, A(row.value), row.want, fx);
    Case(check, "a mark with no close", "x " + A("203.0.113.7").substr(0, 10), "x addr#?(open)", fx);
    {
        Redactor r(fx);
        check(r.Apply("Cert was issued to ip:203.0.113.9:27015, not ip:2001:db8::5") ==
                  "Cert was issued to addr#1(v4,public), not addr#2(v6,public)",
              "GNS's ip: render, a v4 with a port and a bare v6");
    }
    Case(check, "GNS's ip: render, a v6 in brackets", "ip:[2001:db8::5]:27015 x", "addr#1(v6,public) x", fx);
    Case(check, "ip: inside a word is not GNS's render", "skip:1.2.3.4 and zip:1.2.3.4",
         "skip:1.2.3.4 and zip:1.2.3.4", fx);
}

void IdCases(Checker& check) {
    const RedactContext fx = Fixture();
    {
        Redactor r(fx);
        r.Learn("guid 9af578e3c0ffee00112233445566778a");
        check(r.Apply("guid 9af578e3c0ffee00112233445566778a") == "guid player#1", "a 32-hex id");
        check(r.Apply("id 9af578e3..., x") == "id player#1..., x", "a ShortId");
        check(r.Apply("Banned Bob (9af578e3).") == "Banned Bob (player#1).", "a bare 8-hex of a held id");
        check(r.Apply("hash deadbeef.") == "hash deadbeef.", "an 8-hex of no held id stays");
        check(r.Apply("me 11111111.") == "me player#self.", "the reporter's own id counts as held");
    }
    Case(check, "the reporter's own id", "id 11111111222222223333333344444444", "id player#self", fx);
    Case(check, "a key", "gen:" + std::string(64, 'a') + " and gen:aaaaaaaa...", "key#1 and key#1...", fx);
    Case(check, "the reporter's own key", "gen:" + std::string(64, 'b'), "key#self", fx);
    Case(check, "gen: inside a word", "xgen:" + std::string(64, 'a'), "xgen:" + std::string(64, 'a'), fx);
    const char* kUntouched[] = {"exe fileversion 4.27.2.0", "nothing to replace here, at all."};
    for (const char* line : kUntouched) Case(check, "a line with nothing to replace", line, line, fx);
    const std::string hex31 = "id " + std::string(31, 'a'), hex33 = "id " + std::string(33, 'a'),
                      hexUpper = "id " + std::string(32, 'A');
    Case(check, "a 31-hex run", hex31, hex31, fx);
    Case(check, "a 33-hex run", hex33, hex33, fx);
    Case(check, "an uppercase 32-hex run", hexUpper, hexUpper, fx);
}

void ConfigCases(Checker& check) {
    const auto row = [](const char* key) { return CR::FindRow(key); };
    check(CR::ValueForLog(row("net.lobby_password"), "hunter2") == "<set>", "a credential row");
    check(CR::ValueForLog(row("net.turn_user"), "bob") == "<set>", "net.turn_user is a credential");
    check(CR::ValueForLog(row("net.master"), "") == "", "net.master, empty");
    check(CR::ValueForLog(row("net.master"), "USA") == A("USA"), "net.master, set");
    check(CR::ValueForLog(row("net.peer"), "203.0.113.5") == A("203.0.113.5"), "net.peer, an address");
    check(CR::ValueForLog(row("net.peer"), "127.0.0.1") == "127.0.0.1", "net.peer, its default");
    check(CR::ValueForLog(row("browser.lastdirect"), coop::net::kDefaultDirectAddr) ==
              coop::net::kDefaultDirectAddr,
          "browser.lastdirect, its default");
    check(CR::ValueForLog(row("net.masters"), "default") == "default", "net.masters, its default");
    check(CR::ValueForLog(row("net.masters"), "default,Home=home.example.org:10443") ==
              A("default,Home=home.example.org:10443"),
          "net.masters, a list of its own");
    check(CR::ValueForLog(nullptr, "x") == "<not shown>", "a null row");

    const std::vector<std::string> in = {"[net]", "; peer=203.0.113.5", "net.lobby_password=hunter2",
                                         "net.peer=203.0.113.5", "net.peer=127.0.0.1",
                                         "net.lobby_pasword=hunter2", "=x", "voice.mode=activation"};
    const std::vector<std::string> want = {"[net]", "net.lobby_password=<set>",
                                           "net.peer=" + A("203.0.113.5"), "net.peer=127.0.0.1",
                                           "net.lobby_pasword=<not shown>", "voice.mode=activation"};
    check(coop::config::IniLinesForReport(in) == want, "the ini's report form");
}

void EndpointCases(Checker& check) {
    check(EL::IsOfficialHost("master.multivoid.dev") && EL::IsOfficialHost("MASTER2.multivoid.dev") &&
              EL::IsOfficialHost("multivoid.dev") && EL::IsOfficialHost("stun.l.google.com"),
          "the project's own hosts");
    check(!EL::IsOfficialHost("evilmultivoid.dev") && !EL::IsOfficialHost("multivoid.dev.example.com") &&
              !EL::IsOfficialHost("203.0.113.9"),
          "hosts that are not the project's");
    check(EL::LogEndpoint("https://master.multivoid.dev:10443/x") == "https://master.multivoid.dev:10443/x",
          "an official endpoint prints plain");
    check(EL::LogEndpoint("home.example.org:10443") == A("home.example.org:10443"),
          "another endpoint is marked");
    check(EL::LogEndpointList("stun.l.google.com:19302,turn:203.0.113.9:3478") ==
              "stun.l.google.com:19302," + A("turn:203.0.113.9:3478"),
          "an endpoint list");
    check(EL::JoinHostPort("::1", "5") == "[::1]:5" && EL::JoinHostPort("[::1]", "5") == "[::1]:5" &&
              EL::JoinHostPort("h", "5") == "h:5",
          "host and port joined");
    check(EL::HostPart("[::1]:5555") == "::1", "the host part of a bracketed v6");

    namespace MS = coop::net::master_slots;
    const std::vector<MS::Slot> official = MS::Parse("default", nullptr);
    check(official.size() == 2 && MS::LogLabel(official[0]) == "USA" && MS::LogLabel(official[1]) == "EU",
          "the official masters' labels print plain");
    const std::vector<MS::Slot> raw = MS::Parse("203.0.113.9:10443", nullptr);
    check(raw.size() == 1 && MS::LogLabel(raw[0]) == A("203.0.113.9"), "a master given as an address");
    const std::vector<MS::Slot> named = MS::Parse("Home=home.example.org:1", nullptr);
    check(named.size() == 1 && MS::LogLabel(named[0]) == A("Home"), "a master with its own label");
}

bool Says(const char* got, const char* want) { return got && std::strcmp(got, want) == 0; }

void FormCases(Checker& check) {
    const char* const kShort = "Please describe what happened (at least 20 characters).";
    Form f;
    f.happened = Rep("\xD0\xB0", 19);  // U+0430, 19 code points in 38 bytes
    check(Says(ValidateForm(f), kShort), "19 code points is too short");
    f.happened = Rep("\xD0\xB0", 20);
    check(ValidateForm(f) == nullptr, "20 code points is enough");
    f.happened = "  " + std::string(18, 'x') + "  ";
    check(Says(ValidateForm(f), kShort), "the form is trimmed before it is counted");
    f.happened = std::string(20, 'x');
    f.contact = std::string(129, 'c');
    check(Says(ValidateForm(f), "That text is too long."), "a 129-byte contact");
    f.contact = std::string(128, 'c');
    check(ValidateForm(f) == nullptr, "a 128-byte contact");
    f.expected = std::string(4001, 'e');
    check(Says(ValidateForm(f), "That text is too long."), "an expectation over its cap");

    Form g;
    g.happened = "a";
    g.expected = "b";
    g.contact = "c";
    check(ReportText(g) == "What happened:\na\n\nWhat you expected:\nb\n\nContact: c\n", "the report's text");

    check(IsReadableLogFormat("log format 1") && IsReadableLogFormat("log format 1\r"),
          "this build's log format");
    check(!IsReadableLogFormat("log format 2") && !IsReadableLogFormat("[12:00:00] [INFO ] x"),
          "another log format");
}

// A 1500-byte line, which the logger writes whole: the smoke looks for its last word.
void LongLine() {
    static const char kEnd[] = "END-OF-LONG-LINE";
    std::string line = "bug_report selftest: one long line, written whole: ";
    while (line.size() + sizeof(kEnd) - 1 < 1500) line += "0123456789";
    line.resize(1500 - (sizeof(kEnd) - 1));
    line += kEnd;
    UE_LOGI("%s", line.c_str());
}

}  // namespace

bool RunSelftest() {
    Checker check;
    const bool breakIt = coop::config::ResolveFlag(coop::config_registry::rows::selftest_break_bug_report);
    FolderCases(check, breakIt);
    AddressCases(check);
    IdCases(check);
    ConfigCases(check);
    EndpointCases(check);
    FormCases(check);
    LongLine();

    if (check.pass == check.total) {
        UE_LOGI("bug_report selftest: ALL PASS (%d checks)", check.total);
        return true;
    }
    UE_LOGE("bug_report selftest: %d/%d checks passed", check.pass, check.total);
    return false;
}

}  // namespace coop::bug_report
