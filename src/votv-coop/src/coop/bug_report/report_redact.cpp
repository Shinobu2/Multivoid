// coop/bug_report/report_redact.cpp -- see coop/bug_report/report_core.h.
//
// Apply scans a line left to right; at each position the first rule that matches wins, and every
// other byte is copied:
//   1. the address mark (ue_wrap::log::Addr) -> the address token of its value; a mark with no
//      close on the line -> addr#?(open) and the rest of the line dropped
//   2. GNS's own render of an IP identity, `ip:` + the address, which no site can mark
//   3. this machine's folders: local app data first -> %LOCALAPPDATA%, then the profile ->
//      %USERPROFILE%, each only at a name's end
//   4. `gen:` + lowercase hex of 8 or more -> key#N, keyed by its first 8
//   5. a lowercase hex run of 32 (a player id) or of 8 (the ShortId of one the bundle holds) ->
//      player#N, keyed by the first 8
// The reporter's own id and key print as player#self and key#self. The address token is
// addr#N(<class>), N counting distinct host parts (coop::net::endpoint_log::HostPart).

#include "coop/bug_report/report_core.h"

#include "coop/net/endpoint_log.h"
#include "ue_wrap/core/log.h"

#include <algorithm>
#include <cstdint>

namespace coop::bug_report {
namespace {

constexpr std::string_view kOpen = ue_wrap::log::kAddrOpen;
constexpr std::string_view kClose = ue_wrap::log::kAddrClose;
constexpr size_t kNone = std::string_view::npos;

bool IsDigit(char c) { return c >= '0' && c <= '9'; }
bool IsHex(char c) { return IsDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
bool IsUpperHex(char c) { return c >= 'A' && c <= 'F'; }
bool IsWordChar(char c) {
    return IsDigit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
bool IsSlash(char c) { return c == '\\' || c == '/'; }
bool IsAsciiLetter(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
char Lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

// A byte after a folder form that does not continue a folder's name: a line's end, one of
// `\ / ' " ) ] , ; : space tab CR`, or a `.` that ends a sentence (then the end, a space or CR).
bool FolderBoundary(std::string_view line, size_t pos) {
    if (pos >= line.size()) return true;
    switch (line[pos]) {
        case '\\': case '/': case '\'': case '"': case ')': case ']': case ',': case ';':
        case ':': case ' ': case '\t': case '\r':
            return true;
        case '.': {
            const size_t next = pos + 1;
            return next >= line.size() || line[next] == ' ' || line[next] == '\r';
        }
        default:
            return false;
    }
}

// `a.b.c.d`, each 1-3 decimal digits and at most 255.
bool ParseV4(std::string_view h, int (&oct)[4]) {
    size_t p = 0;
    for (int k = 0; k < 4; ++k) {
        size_t d = p;
        int v = 0;
        while (d < h.size() && IsDigit(h[d]) && d - p < 3) v = v * 10 + (h[d++] - '0');
        if (d == p || v > 255) return false;
        oct[k] = v;
        p = d;
        if (k < 3) {
            if (p >= h.size() || h[p] != '.') return false;
            ++p;
        }
    }
    return p == h.size();
}

bool StartsWith(std::string_view s, std::string_view prefix) { return s.substr(0, prefix.size()) == prefix; }

// The class of a host part, already lower-cased.
const char* ClassOf(std::string_view h) {
    if (h.empty()) return "empty";
    int o[4];
    if (ParseV4(h, o)) {
        if (o[0] == 127) return "v4,loopback";
        if (o[0] == 10 || (o[0] == 172 && o[1] >= 16 && o[1] <= 31) ||
            (o[0] == 192 && o[1] == 168) || (o[0] == 169 && o[1] == 254))
            return "v4,lan";
        return "v4,public";
    }
    size_t colons = 0;
    bool v6Bytes = true, nameBytes = true;
    for (const char c : h) {
        if (c == ':') ++colons;
        if (!IsHex(c) && c != ':' && c != '.') v6Bytes = false;
        if (!IsDigit(c) && !IsAsciiLetter(c) && c != '-' && c != '.') nameBytes = false;
    }
    if (colons >= 2 && v6Bytes) {
        if (h == "::1") return "v6,loopback";
        if (StartsWith(h, "fc") || StartsWith(h, "fd") || StartsWith(h, "fe80")) return "v6,lan";
        return "v6,public";
    }
    return nameBytes ? "name" : "unparsed";
}

// After a `:` and at least one digit, the port; else `e` itself.
size_t SkipPort(std::string_view l, size_t e) {
    if (e + 1 < l.size() && l[e] == ':' && IsDigit(l[e + 1])) {
        ++e;
        while (e < l.size() && IsDigit(l[e])) ++e;
    }
    return e;
}

// Where the address GNS printed after `ip:` ends, or kNone when what follows is none of the three
// shapes: a dotted quad with an optional `:port`, a `[v6]` with an optional `:port`, a bare v6.
size_t GnsValueEnd(std::string_view l, size_t b) {
    const size_t n = l.size();
    if (b >= n) return kNone;
    if (l[b] == '[') {
        const size_t close = l.find(']', b);
        return close == kNone ? kNone : SkipPort(l, close + 1);
    }
    size_t p = b;
    bool quad = true;
    for (int k = 0; k < 4 && quad; ++k) {
        size_t d = p;
        while (d < n && IsDigit(l[d]) && d - p < 3) ++d;
        if (d == p || (d < n && IsDigit(l[d]))) { quad = false; break; }
        p = d;
        if (k < 3) {
            if (p < n && l[p] == '.') ++p;
            else quad = false;
        }
    }
    if (quad) return SkipPort(l, p);
    size_t d = b;
    size_t colons = 0;
    while (d < n && (IsHex(l[d]) || l[d] == ':' || l[d] == '.')) {
        if (l[d] == ':') ++colons;
        ++d;
    }
    return colons >= 2 ? d : kNone;
}

}  // namespace

Redactor::Redactor(RedactContext ctx) {
    const auto add = [this](const std::vector<std::string>& forms, bool local) {
        std::vector<std::string> kept;
        for (const std::string& f : forms)
            if (!f.empty() && std::find(kept.begin(), kept.end(), f) == kept.end()) kept.push_back(f);
        std::stable_sort(kept.begin(), kept.end(),
                         [](const std::string& a, const std::string& b) { return a.size() > b.size(); });
        for (std::string& f : kept) folders_.push_back({std::move(f), local});
    };
    add(ctx.localAppDataForms, true);
    add(ctx.profileForms, false);
    if (ctx.selfPlayerId.size() == 32) selfPlayer8_ = ctx.selfPlayerId.substr(0, 8);
    if (ctx.selfKey.size() >= 12 && StartsWith(ctx.selfKey, "gen:")) selfKey8_ = ctx.selfKey.substr(4, 8);

    startsRule_[0xE2] = true;  // the address mark's first byte
    startsRule_[static_cast<unsigned char>('i')] = true;
    startsRule_[static_cast<unsigned char>('g')] = true;
    for (int c = 0; c < 256; ++c)
        if (IsHex(static_cast<char>(c))) startsRule_[static_cast<size_t>(c)] = true;
    for (const Folder& f : folders_) {
        const char c = f.bytes[0];
        startsRule_[static_cast<unsigned char>(c)] = true;
        if (IsAsciiLetter(c)) {
            startsRule_[static_cast<unsigned char>(Lower(c))] = true;
            startsRule_[static_cast<unsigned char>(c >= 'a' ? c - 'a' + 'A' : c)] = true;
        }
        if (IsSlash(c)) {
            startsRule_[static_cast<unsigned char>('\\')] = true;
            startsRule_[static_cast<unsigned char>('/')] = true;
        }
    }
}

// A form matches `\` and `/` for each other, ASCII letters in either case, other bytes exactly,
// and only at a name's end.
bool Redactor::MatchFolder(std::string_view line, size_t at, const Folder& f) const {
    const std::string& form = f.bytes;
    if (at + form.size() > line.size()) return false;
    for (size_t k = 0; k < form.size(); ++k) {
        const char a = line[at + k], b = form[k];
        if (a == b) continue;
        if (IsSlash(a) && IsSlash(b)) continue;
        if (IsAsciiLetter(a) && IsAsciiLetter(b) && Lower(a) == Lower(b)) continue;
        return false;
    }
    return FolderBoundary(line, at + form.size());
}

std::string Redactor::AddressToken(std::string_view value) {
    std::string host = coop::net::endpoint_log::HostPart(value);
    for (char& c : host) c = Lower(c);
    const auto it = addresses_.emplace(host, static_cast<int>(addresses_.size()) + 1).first;
    return "addr#" + std::to_string(it->second) + "(" + ClassOf(host) + ")";
}

std::string Redactor::PlayerToken(std::string_view first8) {
    if (!selfPlayer8_.empty() && first8 == selfPlayer8_) return "player#self";
    const auto it = players_.emplace(std::string(first8), static_cast<int>(players_.size()) + 1).first;
    return "player#" + std::to_string(it->second);
}

std::string Redactor::KeyToken(std::string_view first8) {
    if (!selfKey8_.empty() && first8 == selfKey8_) return "key#self";
    const auto it = keys_.emplace(std::string(first8), static_cast<int>(keys_.size()) + 1).first;
    return "key#" + std::to_string(it->second);
}

void Redactor::Learn(std::string_view line) {
    const size_t n = line.size();
    size_t i = 0;
    while (i < n) {
        if (!IsHex(line[i])) { ++i; continue; }
        size_t e = i;
        bool lower = true;
        while (e < n && IsHex(line[e])) {
            if (IsUpperHex(line[e])) lower = false;
            ++e;
        }
        if (e - i == 32 && lower) learned8_.emplace(line.substr(i, 8));
        i = e;
    }
}

std::string Redactor::Apply(std::string_view line) {
    std::string out;
    out.reserve(line.size());
    const size_t n = line.size();
    size_t i = 0;
    while (i < n) {
        size_t j = i;
        while (j < n && !startsRule_[static_cast<unsigned char>(line[j])]) ++j;
        out.append(line.data() + i, j - i);
        i = j;
        if (i >= n) break;
        const char c = line[i];

        if (static_cast<unsigned char>(c) == 0xE2 && line.substr(i, kOpen.size()) == kOpen) {
            const size_t valueBegin = i + kOpen.size();
            const size_t close = line.find(kClose, valueBegin);
            ++counts_.addresses;
            if (close == kNone) {
                out += "addr#?(open)";
                return out;
            }
            out += AddressToken(line.substr(valueBegin, close - valueBegin));
            i = close + kClose.size();
            continue;
        }
        if (c == 'i' && line.substr(i, 3) == "ip:" && (i == 0 || !IsWordChar(line[i - 1]))) {
            const size_t valueBegin = i + 3;
            const size_t valueEnd = GnsValueEnd(line, valueBegin);
            if (valueEnd != kNone) {
                ++counts_.addresses;
                out += AddressToken(line.substr(valueBegin, valueEnd - valueBegin));
                i = valueEnd;
                continue;
            }
        }
        bool folder = false;
        for (const Folder& f : folders_) {
            if (!MatchFolder(line, i, f)) continue;
            out += f.local ? "%LOCALAPPDATA%" : "%USERPROFILE%";
            ++counts_.profile;
            i += f.bytes.size();
            folder = true;
            break;
        }
        if (folder) continue;
        if (c == 'g' && line.substr(i, 4) == "gen:" && (i == 0 || !IsWordChar(line[i - 1]))) {
            const size_t hexBegin = i + 4;
            size_t e = hexBegin;
            while (e < n && IsHex(line[e]) && !IsUpperHex(line[e])) ++e;
            if (e - hexBegin >= 8) {
                out += KeyToken(line.substr(hexBegin, 8));
                ++counts_.keys;
                i = e;
                continue;
            }
        }
        if (IsHex(c) && (i == 0 || !IsHex(line[i - 1]))) {
            size_t e = i;
            bool lower = true;
            while (e < n && IsHex(line[e])) {
                if (IsUpperHex(line[e])) lower = false;
                ++e;
            }
            const size_t len = e - i;
            if (lower && (len == 32 || len == 8)) {
                const std::string_view first8 = line.substr(i, 8);
                if (len == 32 || (!selfPlayer8_.empty() && first8 == selfPlayer8_) ||
                    learned8_.count(std::string(first8)) != 0) {
                    out += PlayerToken(first8);
                    ++counts_.players;
                    i = e;
                    continue;
                }
            }
        }
        out.push_back(c);
        ++i;
    }
    return out;
}

}  // namespace coop::bug_report
