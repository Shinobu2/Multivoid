// coop/net/endpoint_log.cpp -- see coop/net/endpoint_log.h.

#include "coop/net/endpoint_log.h"

#include "coop/config/config_registry.h"
#include "ue_wrap/core/log.h"

namespace coop::net::endpoint_log {
namespace {

// The project's domain. Not in protocol.h: a change there is a wire bump for the rig.
constexpr std::string_view kOfficialDomain = "multivoid.dev";

char Lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

bool EqualsNoCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (Lower(a[i]) != Lower(b[i])) return false;
    return true;
}

bool IsSchemeChar(char c, bool first) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) return true;
    return !first && ((c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.');
}

// A leading `<scheme>://`: removed from `v` when present.
void DropScheme(std::string_view& v) {
    const size_t sep = v.find("://");
    if (sep == std::string_view::npos || sep == 0) return;
    for (size_t i = 0; i < sep; ++i)
        if (!IsSchemeChar(v[i], i == 0)) return;
    v.remove_prefix(sep + 3);
}

// A leading `stun:`, `turn:` or `turns:` (the ICE server forms): removed from `v` when present.
void DropIcePrefix(std::string_view& v) {
    for (const std::string_view prefix : {std::string_view("stun:"), std::string_view("turn:"),
                                          std::string_view("turns:")}) {
        if (v.size() >= prefix.size() && EqualsNoCase(v.substr(0, prefix.size()), prefix)) {
            v.remove_prefix(prefix.size());
            return;
        }
    }
}

bool IsNameChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '-' || c == '.';
}

// The host of the net.stun row's compiled default, read once.
const std::string& StunDefaultHost() {
    static const std::string host = [] {
        const ::coop::config_registry::Row* row = ::coop::config_registry::FindRow("net.stun");
        return (row && row->defS) ? HostPart(row->defS) : std::string();
    }();
    return host;
}

}  // namespace

std::string HostPart(std::string_view value) {
    std::string_view v = value;
    DropScheme(v);
    DropIcePrefix(v);
    const size_t cut = v.find_first_of("/?");
    if (cut != std::string_view::npos) v = v.substr(0, cut);
    if (!v.empty() && v.front() == '[') {
        const size_t close = v.find(']');
        if (close != std::string_view::npos) return std::string(v.substr(1, close - 1));
        return std::string(v);
    }
    size_t colons = 0, firstColon = std::string_view::npos;
    for (size_t i = 0; i < v.size(); ++i) {
        if (v[i] != ':') continue;
        if (colons++ == 0) firstColon = i;
    }
    if (colons == 1) return std::string(v.substr(0, firstColon));
    return std::string(v);
}

bool IsOfficialHost(std::string_view host) {
    if (host.empty()) return false;
    for (const char c : host)
        if (!IsNameChar(c)) return false;
    if (EqualsNoCase(host, kOfficialDomain)) return true;
    if (host.size() > kOfficialDomain.size() + 1 &&
        host[host.size() - kOfficialDomain.size() - 1] == '.' &&
        EqualsNoCase(host.substr(host.size() - kOfficialDomain.size()), kOfficialDomain))
        return true;
    const std::string& stun = StunDefaultHost();
    return !stun.empty() && EqualsNoCase(host, stun);
}

std::string LogEndpoint(std::string_view endpoint) {
    if (IsOfficialHost(HostPart(endpoint))) return std::string(endpoint);
    return ue_wrap::log::Addr(endpoint);
}

std::string LogEndpointList(std::string_view commaList) {
    std::string out;
    size_t pos = 0;
    while (true) {
        const size_t comma = commaList.find(',', pos);
        const std::string_view entry = commaList.substr(
            pos, comma == std::string_view::npos ? std::string_view::npos : comma - pos);
        out += LogEndpoint(entry);
        if (comma == std::string_view::npos) break;
        out += ',';
        pos = comma + 1;
    }
    return out;
}

std::string JoinHostPort(std::string_view host, std::string_view port) {
    std::string out;
    const bool bracket = host.find(':') != std::string_view::npos &&
                         host.find('[') == std::string_view::npos;
    if (bracket) out += '[';
    out += host;
    if (bracket) out += ']';
    out += ':';
    out += port;
    return out;
}

}  // namespace coop::net::endpoint_log
