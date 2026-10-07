// coop/moderation/ban_list_selftest.cpp -- the un-gated selftest of ban_list's file codec, address
// rule and address index, run once per session start. In memory only: the live store may hold the
// previous server's file, so these cases go through the pure functions and never through it.

#include "coop/moderation/ban_list.h"

#include "ue_wrap/core/log.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace coop::ban_list {
namespace {

constexpr const char* kIdA = "0123456789abcdef0123456789abcdef";
constexpr const char* kIdB = "fedcba9876543210fedcba9876543210";
constexpr const char* kIdC = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
constexpr const char* kIdD = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";

Entry Make(const char* id, const char* nick, const char* address, const char* reason, long long when) {
    Entry e;
    std::snprintf(e.id, sizeof(e.id), "%s", id);
    std::snprintf(e.nick, sizeof(e.nick), "%s", nick);
    std::snprintf(e.address, sizeof(e.address), "%s", address);
    std::snprintf(e.reason, sizeof(e.reason), "%s", reason);
    e.bannedUnix = when;
    return e;
}

bool Same(const Entry& a, const Entry& b) {
    return std::strcmp(a.id, b.id) == 0 && std::strcmp(a.nick, b.nick) == 0 &&
           std::strcmp(a.address, b.address) == 0 && std::strcmp(a.reason, b.reason) == 0 &&
           a.bannedUnix == b.bannedUnix;
}

std::string Rec(const char* id, const std::string& fields) {
    return std::string("{\"id\": \"") + id + "\"" + fields + "}";
}

std::string Doc(const std::vector<std::string>& recs) {
    std::string s = "{\"bans\": [";
    for (size_t i = 0; i < recs.size(); ++i) s += (i ? ", " : "") + recs[i];
    return s + "]}";
}

}  // namespace

bool RunSelftest() {
    int pass = 0, total = 0;
    auto check = [&](bool ok, const char* what) {
        ++total;
        if (ok) { ++pass; return; }
        UE_LOGE("ban_list selftest FAIL: %s", what);
    };

    // 1. Serialize then parse: three records come back exactly.
    {
        const std::vector<Entry> in = {
            Make(kIdA, "Alice", "10.0.0.5", "spam", 1700000001),
            Make(kIdB, "Bob", "", "", 1700000002),
            Make(kIdC, "Cy", "2001:db8::7", "griefing", 1700000003),
        };
        std::vector<Entry> out;
        std::vector<std::string> problems;
        int skipped = -1;
        const bool ok = ParseBans(SerializeBans(in), &out, &problems, &skipped);
        check(ok && skipped == 0 && problems.empty() && out.size() == 3, "a serialized list parses whole");
        check(out.size() == 3 && Same(out[0], in[0]) && Same(out[1], in[1]) && Same(out[2], in[2]),
              "the three records round-trip exactly");
    }

    // 2. A record failing a field check is skipped and counted; the others load.
    {
        const std::string text = Doc({
            Rec(kIdA, ", \"name\": \"ok\", \"since\": 5"),
            Rec("not-hex-not-hex-not-hex-not-hex!", ""),
            Rec(kIdB, ", \"name\": 42"),
            Rec(kIdC, ", \"since\": \"yesterday\""),
            Rec(kIdD, ""),
        });
        std::vector<Entry> out;
        std::vector<std::string> problems;
        int skipped = -1;
        const bool ok = ParseBans(text, &out, &problems, &skipped);
        check(ok && skipped == 3 && problems.size() == 3, "a non-hex id, a numeric name and a string time are skipped");
        check(out.size() == 2 && std::strcmp(out[0].id, kIdA) == 0 && out[0].bannedUnix == 5 &&
                  std::strcmp(out[1].id, kIdD) == 0,
              "and the records beside them load");
        check(!problems.empty() && problems[0] == "record 1 skipped (id)", "the problem line names the record and the field");
    }

    // 3. An upper-case id loads lower-cased.
    {
        std::vector<Entry> out;
        std::vector<std::string> problems;
        int skipped = -1;
        const bool ok = ParseBans(Doc({Rec("0123456789ABCDEF0123456789ABCDEF", "")}), &out, &problems, &skipped);
        check(ok && skipped == 0 && out.size() == 1 && std::strcmp(out[0].id, kIdA) == 0,
              "an upper-case id is lower-cased");
    }

    // 4. A repeated id keeps the later record and is not a skip.
    {
        std::vector<Entry> out;
        std::vector<std::string> problems;
        int skipped = -1;
        const bool ok = ParseBans(
            Doc({Rec(kIdA, ", \"reason\": \"first\""), Rec(kIdB, ""), Rec(kIdA, ", \"reason\": \"second\"")}),
            &out, &problems, &skipped);
        check(ok && skipped == 0 && out.size() == 2, "a repeated id leaves one record and no skip");
        check(out.size() == 2 && std::strcmp(out[0].reason, "second") == 0, "the later record wins");
        check(problems.size() == 1 && problems[0] == "record 2 repeats an id", "and the repeat is said");
    }

    // 5. A text that is not a list is refused whole.
    {
        std::vector<Entry> out;
        std::vector<std::string> problems;
        int skipped = 0;
        check(!ParseBans("[]", &out, &problems, &skipped), "a non-object root is refused");
        check(!ParseBans("{\"bans\": 3}", &out, &problems, &skipped), "an object without a bans array is refused");
        check(!ParseBans("{\"bans\": [", &out, &problems, &skipped), "a truncated text is refused");
        check(!ParseBans("", &out, &problems, &skipped), "an empty text is refused");
    }

    // 6. A field longer than its buffer is cut on a character boundary.
    {
        std::string name;
        for (int i = 0; i < 30; ++i) name += "\xE3\x81\x82";  // 90 bytes of three-byte codepoints
        std::vector<Entry> out;
        std::vector<std::string> problems;
        int skipped = -1;
        const bool ok = ParseBans(Doc({Rec(kIdA, ", \"name\": \"" + name + "\"")}), &out, &problems, &skipped);
        check(ok && out.size() == 1 && std::strlen(out[0].nick) == 78,
              "90 bytes of nick are cut to 78, a whole number of codepoints, under the 80-byte cap");
    }

    // 7. Text that is not UTF-8 serializes (the invalid byte is replaced) and parses back.
    {
        std::vector<Entry> in = {Make(kIdA, "n", "", "bad\xFF" "tail", 1)};
        const std::string text = SerializeBans(in);
        std::vector<Entry> out;
        std::vector<std::string> problems;
        int skipped = -1;
        check(!text.empty() && ParseBans(text, &out, &problems, &skipped) && skipped == 0 && out.size() == 1,
              "invalid UTF-8 in a reason still serializes to a readable file");
    }

    // 8. The address rule.
    check(!IsBannableAddress("") && !IsBannableAddress("::") && !IsBannableAddress("0.0.0.0"),
          "an empty or unspecified address is not bannable");
    check(!IsBannableAddress("127.0.0.1") && !IsBannableAddress("::1"), "loopback is not bannable");
    check(IsBannableAddress("10.0.0.5"), "a private address is bannable");

    // 9. The address index, over the pure functions only.
    {
        const std::vector<Entry> set = {
            Make(kIdA, "a", "10.0.0.5", "", 1),
            Make(kIdB, "b", "", "", 2),
            Make(kIdC, "c", "::", "", 3),
            Make(kIdD, "d", "127.0.0.1", "", 4),
        };
        const AddressIndex idx = BuildAddressIndex(set);
        const std::string* a = LookupAddress(idx, "10.0.0.5");
        check(a && *a == kIdA, "a bannable address finds its entry");
        check(LookupAddress(idx, "") == nullptr && LookupAddress(idx, "::") == nullptr,
              "an empty or unspecified address finds nothing");
        check(LookupAddress(idx, "10.0.0.6") == nullptr, "another address finds nothing");
        check(LookupAddress(idx, "127.0.0.1") == nullptr && idx.size() == 1, "a loopback entry is not indexed");
    }

    if (pass == total) {
        UE_LOGI("ban_list selftest: ALL PASS (%d checks)", total);
        return true;
    }
    return false;
}

}  // namespace coop::ban_list
