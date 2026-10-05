// coop/build_trust/build_trust_selftest.cpp -- the un-gated selftest of the signed message, the .sig
// parser and the verifier, run once per process (shape: coop/player/stat_orders_wire_selftest.cpp).
// About a millisecond (a handful of signature verifies and one config read), no engine: a parser
// that accepts a bent line, or a verifier that trusts an unknown key, makes a fork read as official.
//
// The vector is signed by the TEST key (id 255), which only a peer with the dev knob trusts. Red arm:
// with the dev row selftest_break_build_trust check 2 expects the opposite, so the run must fail.

#include "coop/build_trust/build_trust.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "ue_wrap/core/log.h"

#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace coop::build_trust {
namespace {

constexpr char kTarget[] = "0.9.0n";
constexpr uint32_t kBuild = 216;
constexpr char kShaHex[] = "5c445d2907e03e872fcc2d0b0725ec5a438cca603825c0ba93058f534facc837";
constexpr char kSigHex[] =
    "6ab2cb4ccc3398f3f07f4e850b0a28d95d3083ed98d179016cd286e39a768852b8d423deccad0fa2e0ea9f78bcc826cc"
    "73c0a38d32622d114135e38ed5fb1c01";
constexpr char kMessageHex[] =
    "6d756c7469766f69642d6275696c642d763106302e392e306ed80000005c445d2907e03e872fcc2d0b0725ec5a438cca"
    "603825c0ba93058f534facc837";
constexpr size_t kMessageBytes = 61;

// The rows of release_keys.inc, counted from the file itself.
constexpr size_t kIncRows = 0
#define MV_RELEASE_KEY(id, pub, fx) +1
#include "coop/build_trust/release_keys.inc"
#undef MV_RELEASE_KEY
    ;

struct Tally {
    int pass = 0;
    int total = 0;
    void operator()(bool ok, const char* what) {
        ++total;
        if (ok) { ++pass; return; }
        UE_LOGE("build_trust selftest FAIL: %s", what);
    }
};

// Lowercase hex into n bytes; the vector's constants are well formed.
void Hex(const char* hex, uint8_t* out, size_t n) {
    auto nib = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
    for (size_t i = 0; i < n; ++i)
        out[i] = static_cast<uint8_t>((nib(hex[i * 2]) << 4) | nib(hex[i * 2 + 1]));
}

SigFile Vector() {
    SigFile v;
    v.keyId = kTestKeyId;
    v.target = kTarget;
    v.build = kBuild;
    Hex(kShaHex, v.sha256, kShaBytes);
    Hex(kSigHex, v.sig, kSigBytes);
    return v;
}

using Lines = std::vector<std::string>;

Lines VectorLines() {
    return {"multivoid-build-sig 1", "key 255", std::string("target ") + kTarget,
            "build 216", std::string("sha256 ") + kShaHex, std::string("sig ") + kSigHex};
}

std::string Join(const Lines& lines, const char* eol, bool finalEol) {
    std::string s;
    for (size_t i = 0; i < lines.size(); ++i) {
        s += lines[i];
        if (i + 1 < lines.size() || finalEol) s += eol;
    }
    return s;
}

bool Parses(const Lines& lines) {
    SigFile f;
    return ParseSigFile(Join(lines, "\n", true), &f);
}

void RunMessage(Tally& check, bool breakIt) {
    const SigFile v = Vector();
    uint8_t expect[kMessageBytes];
    Hex(kMessageHex, expect, kMessageBytes);
    const std::string m = SignedMessage(v.target, v.build, v.sha256);
    check(m.size() == kMessageBytes && std::memcmp(m.data(), expect, kMessageBytes) == 0,
          "message: SignedMessage of the vector is not the 61 pinned bytes");

    // The red arm inverts this one expectation.
    check(SignatureVerifies(v, true) == !breakIt, "verify: the test-key vector does not verify");
    check(!SignatureVerifies(v, false), "verify: the test key was trusted without the knob");

    SigFile w = v;
    w.sig[0] ^= 1;
    check(!SignatureVerifies(w, true), "verify: a flipped signature byte verified");
    w = v;
    w.sha256[0] ^= 1;
    check(!SignatureVerifies(w, true), "verify: a flipped hash byte verified");
    w = v;
    w.build = 217;
    check(!SignatureVerifies(w, true), "verify: another build number verified");
    w = v;
    w.target = "0.9.0m";
    check(!SignatureVerifies(w, true), "verify: another game target verified");
    w = v;
    w.keyId = 1;
    check(!SignatureVerifies(w, true), "verify: the test signature verified under key 1");
    w = v;
    w.keyId = 7;
    check(!SignatureVerifies(w, true), "verify: an id in no row verified");
}

void RunParser(Tally& check) {
    const SigFile v = Vector();
    SigFile f;
    check(ParseSigFile(Join(VectorLines(), "\n", true), &f) && f.keyId == v.keyId &&
              f.target == v.target && f.build == v.build &&
              std::memcmp(f.sha256, v.sha256, kShaBytes) == 0 &&
              std::memcmp(f.sig, v.sig, kSigBytes) == 0,
          "parse: the vector's six lines did not parse to its fields");
    check(ParseSigFile(Join(VectorLines(), "\r\n", true), &f),
          "parse: a CRLF file was refused");
    check(ParseSigFile(Join(VectorLines(), "\n", false), &f),
          "parse: a file without the final newline was refused");

    Lines l = VectorLines();
    l[0] = "multivoid-build-sig 2";
    check(!Parses(l), "parse: header version 2 was accepted");
    l = VectorLines();
    l.pop_back();
    check(!Parses(l), "parse: a file without the sig line was accepted");
    l = VectorLines();
    l[4] = std::string("sha256 ") + std::string(kShaHex).substr(0, 63);
    check(!Parses(l), "parse: a 63-character hash was accepted");
    l = VectorLines();
    l[4] = "sha256 5C445D2907E03E872FCC2D0B0725EC5A438CCA603825C0BA93058F534FACC837";
    check(!Parses(l), "parse: upper-case hex was accepted");
    l = VectorLines();
    l[1] = "key 0";
    check(!Parses(l), "parse: key 0 was accepted");
    l = VectorLines();
    l[3] = "build abc";
    check(!Parses(l), "parse: a non-numeric build was accepted");
    l = VectorLines();
    l.push_back("extra 1");
    check(!Parses(l), "parse: a seventh line was accepted");
}

void RunTable(Tally& check) {
    bool noTestKey = true;
    for (size_t i = 0; i < ReleaseKeyCount(); ++i)
        if (ReleaseKeyAt(i).id == kTestKeyId) noTestKey = false;
    check(ReleaseKeyCount() == kIncRows && noTestKey,
          "table: the decoded rows differ from release_keys.inc, or hold id 255");

    uint8_t zeros[kShaBytes] = {};
    bool allVerify = true;
    for (size_t i = 0; i < ReleaseKeyCount(); ++i) {
        const ReleaseKey& k = ReleaseKeyAt(i);
        SigFile f;
        f.keyId = k.id;
        f.target = "selftest-fixture";
        f.build = 0;
        std::memcpy(f.sha256, zeros, kShaBytes);
        std::memcpy(f.sig, k.fixture, kSigBytes);
        if (!SignatureVerifies(f, false)) allVerify = false;
    }
    check(allVerify, "table: a release row's fixture signature does not verify");
}

bool RunSelftestBody() {
    Tally check;
    const bool breakIt = coop::config::ResolveFlag(coop::config_registry::rows::selftest_break_build_trust);
    RunMessage(check, breakIt);
    RunParser(check);
    RunTable(check);
    if (check.pass == check.total) {
        UE_LOGI("build_trust selftest: ALL PASS (%d checks, %zu release key(s))", check.total,
                ReleaseKeyCount());
        return true;
    }
    UE_LOGE("build_trust selftest: %d/%d checks passed", check.pass, check.total);
    return false;
}

}  // namespace

// Once per process: the session-runtime call is once per session start, and a later call returns
// the first run's verdict without output.
bool RunSelftest() {
    static std::once_flag ran;
    static bool verdict = false;
    std::call_once(ran, [] { verdict = RunSelftestBody(); });
    return verdict;
}

}  // namespace coop::build_trust
