// coop/build_trust/build_trust.cpp -- the signed message, the .sig parser, the release-key table and
// the one verifier every caller uses. Pure: no engine, no wire, no file, no thread of its own.

#include "coop/build_trust/build_trust.h"

#include <utility>

// Ed25519 open, from the donna translation unit GNS already compiles into the static library we
// link (declared as coop/net/peer_identity.cpp declares it, so no GNS-internal header is pulled
// in). 0 means the signature is valid.
extern "C" {
int ed25519_sign_open(const unsigned char* m, size_t mlen, const unsigned char pk[32],
                      const unsigned char RS[64]);
}

namespace coop::build_trust {
namespace {

constexpr char kMessageTag[] = "multivoid-build-v1";
constexpr char kTestPubHex[] = "3deae44494d0794e183acf6c4445756458b27318cd531f355628249c54656e40";

// --- the release table -------------------------------------------------------------------------

struct ReleaseKeyRow {
    uint8_t id;
    const char* pubHex;
    const char* fixtureHex;
};

// The sentinel keeps an empty table legal C++; an empty table is the fail-closed state.
constexpr ReleaseKeyRow kRows[] = {
#define MV_RELEASE_KEY(id, pub, fx) {id, pub, fx},
#include "coop/build_trust/release_keys.inc"
#undef MV_RELEASE_KEY
    {0, nullptr, nullptr}};

constexpr bool IsLowerHex(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }

constexpr bool IsHexOfLength(const char* s, size_t n) {
    if (s == nullptr) return false;
    for (size_t i = 0; i < n; ++i)
        if (!IsLowerHex(s[i])) return false;
    return s[n] == '\0';
}

constexpr size_t RowCount() {
    size_t n = 0;
    while (kRows[n].pubHex != nullptr) ++n;
    return n;
}

constexpr bool RowsWellFormed() {
    for (size_t i = 0; kRows[i].pubHex != nullptr; ++i) {
        const ReleaseKeyRow& r = kRows[i];
        if (r.id < 1 || r.id > 254) return false;
        if (!IsHexOfLength(r.pubHex, kPubBytes * 2)) return false;
        if (!IsHexOfLength(r.fixtureHex, kSigBytes * 2)) return false;
        for (size_t j = 0; j < i; ++j)
            if (kRows[j].id == r.id) return false;
    }
    return true;
}

// A bad row fails the BUILD, never a run.
static_assert(RowsWellFormed(), "release_keys.inc: a malformed row");

// --- hex ---------------------------------------------------------------------------------------

int Nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

// Lowercase hex only; exactly n bytes.
bool DecodeHex(std::string_view hex, uint8_t* out, size_t n) {
    if (hex.size() != n * 2) return false;
    for (size_t i = 0; i < n; ++i) {
        const int hi = Nibble(hex[i * 2]);
        const int lo = Nibble(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return true;
}

// The table decoded once, on first use. Row count + 1 keeps the array legal when the table is empty.
// A row that fails to decode cannot get past the static_assert above; were it to, the count would
// fall short of the rows in the file and the selftest's check 20 fails.
struct DecodedTable {
    ReleaseKey keys[RowCount() + 1]{};
    size_t count = 0;
    uint8_t testPub[kPubBytes]{};
    bool testPubOk = false;
};

const DecodedTable& Table() {
    static const DecodedTable t = [] {
        DecodedTable d;
        for (size_t i = 0; i < RowCount(); ++i) {
            ReleaseKey& k = d.keys[d.count];
            k.id = kRows[i].id;
            if (DecodeHex(kRows[i].pubHex, k.pub, kPubBytes) &&
                DecodeHex(kRows[i].fixtureHex, k.fixture, kSigBytes))
                ++d.count;
        }
        d.testPubOk = DecodeHex(kTestPubHex, d.testPub, kPubBytes);
        return d;
    }();
    return t;
}

// --- the .sig grammar --------------------------------------------------------------------------

// The next line of `rest` without its terminator ("\n" or "\r\n"); false when nothing is left.
bool NextLine(std::string_view* rest, std::string_view* line) {
    if (rest->empty()) return false;
    const size_t nl = rest->find('\n');
    if (nl == std::string_view::npos) {
        *line = *rest;
        *rest = std::string_view();
    } else {
        *line = rest->substr(0, nl);
        rest->remove_prefix(nl + 1);
    }
    if (!line->empty() && line->back() == '\r') line->remove_suffix(1);
    return true;
}

// `line` is exactly `<key> <value>`; the value goes to *value.
bool Field(std::string_view line, std::string_view key, std::string_view* value) {
    if (line.size() <= key.size() + 1) return false;
    if (line.substr(0, key.size()) != key || line[key.size()] != ' ') return false;
    *value = line.substr(key.size() + 1);
    return true;
}

// Plain decimal: digits only, no leading zero but a lone "0", at most max.
bool ParseDecimal(std::string_view s, uint64_t max, uint64_t* out) {
    if (s.empty() || s.size() > 10) return false;
    if (s.size() > 1 && s[0] == '0') return false;
    uint64_t v = 0;
    for (const char c : s) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + static_cast<uint64_t>(c - '0');
    }
    if (v > max) return false;
    *out = v;
    return true;
}

bool PrintableNoSpace(std::string_view s) {
    for (const char c : s)
        if (c < 0x21 || c > 0x7E) return false;
    return true;
}

}  // namespace

std::string SignedMessage(std::string_view target, uint32_t build, const uint8_t sha256[kShaBytes]) {
    std::string m;
    m.reserve(sizeof kMessageTag - 1 + 1 + target.size() + 4 + kShaBytes);
    m.append(kMessageTag, sizeof kMessageTag - 1);
    m.push_back(static_cast<char>(static_cast<uint8_t>(target.size())));
    m.append(target);
    for (int i = 0; i < 4; ++i) m.push_back(static_cast<char>((build >> (8 * i)) & 0xFF));
    m.append(reinterpret_cast<const char*>(sha256), kShaBytes);
    return m;
}

bool ParseSigFile(std::string_view text, SigFile* out) {
    std::string_view rest = text;
    std::string_view line, value;
    SigFile f;
    uint64_t n = 0;

    if (!NextLine(&rest, &line) || line != "multivoid-build-sig 1") return false;
    if (!NextLine(&rest, &line) || !Field(line, "key", &value) || !ParseDecimal(value, 255, &n) ||
        n < 1)
        return false;
    f.keyId = static_cast<uint8_t>(n);
    if (!NextLine(&rest, &line) || !Field(line, "target", &value) || value.size() > kTargetMax ||
        !PrintableNoSpace(value))
        return false;
    f.target.assign(value);
    if (!NextLine(&rest, &line) || !Field(line, "build", &value) ||
        !ParseDecimal(value, 0xFFFFFFFFull, &n))
        return false;
    f.build = static_cast<uint32_t>(n);
    if (!NextLine(&rest, &line) || !Field(line, "sha256", &value) ||
        !DecodeHex(value, f.sha256, kShaBytes))
        return false;
    if (!NextLine(&rest, &line) || !Field(line, "sig", &value) || !DecodeHex(value, f.sig, kSigBytes))
        return false;
    if (!rest.empty()) return false;

    if (out != nullptr) *out = std::move(f);
    return true;
}

bool SignatureVerifies(const SigFile& sig, bool trustTestKey) {
    if (sig.target.empty() || sig.target.size() > kTargetMax) return false;
    const DecodedTable& t = Table();
    const uint8_t* pub = nullptr;
    if (sig.keyId == kTestKeyId) {
        if (trustTestKey && t.testPubOk) pub = t.testPub;
    } else if (sig.keyId != 0) {
        for (size_t i = 0; i < t.count; ++i)
            if (t.keys[i].id == sig.keyId) pub = t.keys[i].pub;
    }
    if (pub == nullptr) return false;
    const std::string msg = SignedMessage(sig.target, sig.build, sig.sha256);
    return ed25519_sign_open(reinterpret_cast<const unsigned char*>(msg.data()), msg.size(), pub,
                             sig.sig) == 0;
}

size_t ReleaseKeyCount() { return Table().count; }

const ReleaseKey& ReleaseKeyAt(size_t index) { return Table().keys[index]; }

}  // namespace coop::build_trust
