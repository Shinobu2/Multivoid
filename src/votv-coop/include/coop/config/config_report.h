// coop/config/config_report.h -- the ini as a bug report may carry it: section headers and
// `key=value` lines only, each value in its printed form (config_registry::ValueForLog), so a
// credential is `<set>`, an address is marked and an unknown key's value is `<not shown>`. A
// comment is free text and is dropped. Defined in config_census.cpp, beside the census, which
// prints values by the same rule.

#pragma once

#include <string>
#include <vector>

namespace coop::config {

// Pure; any thread. Each line is trimmed; one starting with `;` or `#` is dropped, one starting
// with `[` is kept, one that reads as `key=value` with a non-empty key becomes `key=` + the
// value's printed form (cooked as the String reader cooks it), every other line is dropped.
std::vector<std::string> IniLinesForReport(const std::vector<std::string>& lines);

}  // namespace coop::config
