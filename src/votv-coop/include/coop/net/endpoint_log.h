// coop/net/endpoint_log.h -- how a log line prints an endpoint: a master, a signaling relay, a
// STUN or TURN server. One that belongs to the project (its domain, or the compiled STUN default)
// prints as written; any other is marked with ue_wrap::log::Addr, because a configured or
// handed-out address is its owner's and a bug report must not carry it. Pure; any thread.

#pragma once

#include <string>
#include <string_view>

namespace coop::net::endpoint_log {

// The host part of a log value: a leading `<scheme>://` dropped, then a leading `stun:`, `turn:`
// or `turns:`; cut at the first `/` or `?`; then the v6 of `[v6]:port`, the part before the colon
// of a value with exactly one `:`, else the whole value. Case kept. A log value's grammar, not a
// master list's (master_slots' HostOf parses `host:port` with the port required).
std::string HostPart(std::string_view value);

// A host the project owns: `multivoid.dev`, any name under it, or the host of the net.stun
// row's compiled default. False for anything that is not a plain name (an IP, a v6, a name with
// other bytes), and for a name that merely ends in the domain's letters (`evilmultivoid.dev`).
bool IsOfficialHost(std::string_view host);

// `endpoint` as written when IsOfficialHost(HostPart(endpoint)), else ue_wrap::log::Addr(endpoint).
std::string LogEndpoint(std::string_view endpoint);

// LogEndpoint of each comma-separated entry, the commas kept.
std::string LogEndpointList(std::string_view commaList);

// `host:port` for a log line: `[host]:port` when the host holds a `:` and no `[`, else
// `host:port`.
std::string JoinHostPort(std::string_view host, std::string_view port);

}  // namespace coop::net::endpoint_log
