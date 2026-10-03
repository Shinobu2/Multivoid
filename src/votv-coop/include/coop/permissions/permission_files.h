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

// Loads one holder file's text into `m` (a group when `group`, else the user named by `stem`): the
// ParseHolderText, then Model::LoadGroup / LoadUser, a refused holder one problem `<stem>: refused`
// and each refused node one problem `<stem>: key "<escaped key>" refused`. True when the holder
// loaded (and is counted in `report`). It sees one file only: a group a holder names is checked by
// ReportMissingGroups once every file is in. Engine-free.
bool LoadHolderText(std::string_view text, std::string_view stem, bool group, Model& m, LoadReport& report);

// The checks that need the whole store, for the holder `stem` already loaded into `m`: a user's
// stored primary group, and every `group.<g>` parent node of a user or group, must name a group `m`
// has. Each miss is one problem, `<stem>: primary group "<g>" does not exist` or `<stem>: parent
// group "<g>" does not exist`. Call it for every holder after the last file is loaded: a group may
// name a parent that sorts after it.
void ReportMissingGroups(const Model& m, std::string_view stem, bool group, LoadReport& report);

// Reads `dir` (`<server folder>\permissions`) into `m`: every groups\*.json first, sorted by name so
// a user's parents exist, then every users\*.json. A missing `dir` gives an empty report. A group
// stem (lower-cased) must pass IsValidGroupName and a user stem be 32 hex; a file that fails is
// skipped with a problem. Each file goes through LoadHolderText, then every loaded holder through
// ReportMissingGroups. Expired nodes load as written (Applies filters them). `m` holds what loaded,
// even beside problems; whether to USE it is ShouldLoad's answer, never the caller's guess.
LoadReport LoadStore(const std::filesystem::path& dir, Model& m);

// The store loads whole or not at all: true only for a report with no problem. A store with a
// problem is BROKEN, not empty: a dropped deny, or a refused group whose members inherited its
// denies, would widen a holder, and an empty model would hand a default-granted node back. A server
// with no store at all (no folder, or no file) has an empty report and is not broken. PURE.
bool ShouldLoad(const LoadReport& report);

}  // namespace coop::permissions
