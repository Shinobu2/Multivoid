// coop/server_profile/server_profile_selftest.cpp -- the un-gated selftest of the server id rule,
// run once per session start: a nickname or a hand-typed name must never name a folder outside
// multivoid_servers, and a wrong rule does not crash, it writes somewhere else.

#include "coop/server_profile/server_profile.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"

#include "ue_wrap/core/log.h"

#include <string>

namespace coop::server_profile {

bool RunSelftest() {
    int pass = 0, total = 0;
    auto check = [&](bool ok, const char* what) {
        ++total;
        if (ok) { ++pass; return; }
        UE_LOGE("server_profile selftest FAIL: %s", what);
    };
    auto idCase = [&](const std::string& in, const std::string& want, const char* what) {
        const std::string got = IdFromName(in);
        check(got == want, what);
        check(IsValidId(got), "the derived id passes IsValidId");
    };

    // The red arm: the first case expects the wrong answer, so it must fail.
    const bool breakFirst = coop::config::ResolveFlag(coop::config_registry::rows::selftest_break_server_id);
    idCase("Bob", breakFirst ? "Bob" : "bob", "IdFromName lowers a plain name");

    idCase("Bob's game", "bob-s-game", "a run of other bytes becomes one dash");
    idCase("  --Ann--  ", "ann", "leading and trailing dashes are removed");
    idCase("A1 B2", "a1-b2", "digits are kept");
    idCase("\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82", "server", "a name with no ASCII letter is the fallback");
    idCase("", "server", "an empty name is the fallback");
    idCase("CON", "con-server", "a device name gets the suffix");
    idCase("com1", "com1-server", "a numbered device name gets the suffix");
    idCase("\xC3\x9C" "n" "\xC3\xAF" "code Bob", "n-code-bob", "non-ASCII bytes are one dash each run");
    idCase("abcdefghijklmnopqrstabcdefghijklmnopqrst", "abcdefghijklmnopqrstabcd", "a long name is cut to 24");
    idCase(std::string(23, 'a') + " b", std::string(23, 'a'), "a cut that leaves a dash drops it");

    check(IsValidId("bob"), "bob is valid");
    check(IsValidId("a--b"), "an inner double dash is valid");
    check(IsValidId("con-server"), "con-server is valid");
    check(IsValidId(std::string(24, 'a')), "24 bytes is valid");
    check(!IsValidId(""), "empty is invalid");
    check(!IsValidId(std::string(25, 'a')), "25 bytes is invalid");
    check(!IsValidId("-bob"), "a leading dash is invalid");
    check(!IsValidId("bob-"), "a trailing dash is invalid");
    check(!IsValidId("Bob"), "a capital is invalid");
    check(!IsValidId("b/ob"), "a slash is invalid");
    check(!IsValidId(".."), "dots are invalid");
    check(!IsValidId("nul"), "nul is invalid");
    check(!IsValidId("lpt9"), "lpt9 is invalid");

    if (pass == total) {
        UE_LOGI("server_profile selftest: ALL PASS (%d checks)", total);
        return true;
    }
    UE_LOGE("server_profile selftest: %d/%d checks passed", pass, total);
    return false;
}

}  // namespace coop::server_profile
