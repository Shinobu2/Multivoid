// coop/permissions/permission_files.h -- the hosted server's permission store, read-only: LuckPerms'
// JSON layout under `<server folder>\permissions\` (groups\<name>.json, users\<32 hex>.json). A
// holder is named by its FILE stem, as LuckPerms names one. Engine-free: nlohmann and
// <filesystem> only, no ue_wrap include, no logging; the caller logs the report.
//
// Ported from LuckPerms common/src/main/java/me/lucko/luckperms/common/storage/implementation/file/
// AbstractConfigurateStorage.java:411-504 (MIT, THIRD-PARTY-NOTICES.md): the three permission
// forms, the three parent forms, the attributes. The store is edited by hand and read at each host
// start; nothing is written back.
#pragma once

#include "coop/permissions/model.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace coop::permissions {

struct LoadReport {
    int groups = 0;
    int users = 0;
    std::vector<std::string> problems;
};

// Narrows a file-name part to `*out` by a per-unit cast, never through the ANSI code page (a name
// the code page cannot hold throws). False, `*out` cleared, when any unit is above 0x7F: the file
// is then skipped with a problem. PURE.
bool NarrowAscii(const std::wstring& name, std::string* out);

// Parses one holder file's text. PURE. False when the text is not a JSON object (the problem is
// appended), and an invalid UTF-8 byte anywhere refuses the file whole. On true, `primaryGroup` is
// the stored `primaryGroup` (`default` when absent or not a string, the latter with a problem) and
// `nodes` the file's `permissions` entries then its `parents` entries (each a `group.<name>` node
// with that entry's expiry and context). An entry with a wrong-typed attribute, or a context pair
// ContextSet::Add refuses, is refused WHOLE (dropping the pair would widen it to global) with a
// problem naming `stem` and the field. Every other top-level key is ignored.
bool ParseHolderText(std::string_view text, std::string_view stem, std::string* primaryGroup,
                     std::vector<Node>* nodes, std::vector<std::string>* problems);

// Reads `dir` (`<server folder>\permissions`) into `m`: every groups\*.json first, sorted by name so
// a user's parents exist, then every users\*.json. A missing `dir` gives an empty report. A group
// stem (lower-cased) must pass IsValidGroupName and a user stem be 32 hex; a file that fails is
// skipped with a problem. Expired nodes load as written (Applies filters them). `m` holds what
// loaded, even beside problems; whether to USE it is ShouldLoad's answer, never the caller's guess.
LoadReport LoadStore(const std::filesystem::path& dir, Model& m);

// The store loads whole or not at all: true only for a report with no problem. A dropped deny, or a
// refused group whose members inherited its denies, could only widen a holder; an empty store can
// only narrow, since grants come only from the store. PURE.
bool ShouldLoad(const LoadReport& report);

}  // namespace coop::permissions
