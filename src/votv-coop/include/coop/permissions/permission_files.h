// coop/permissions/permission_files.h -- the hosted server's permission store: LuckPerms'
// JSON layout under `<server folder>\permissions\` (groups\<name>.json, users\<32 hex>.json). A
// holder is named by its FILE stem, as LuckPerms names one. Engine-free: nlohmann, <filesystem> and
// coop/atomic_file (Win32 only), no ue_wrap include, no logging; the caller logs the report.
//
// Ported from LuckPerms common/src/main/java/me/lucko/luckperms/common/storage/implementation/file/
// AbstractConfigurateStorage.java:411-504 (MIT, THIRD-PARTY-NOTICES.md): the three permission
// forms, the three parent forms, the attributes. The store is edited by hand or by the in-game
// edit, read at each host start and at each edit, and written one holder at a time
// (`WriteHolderFile`).
#pragma once

#include "coop/atomic_file/atomic_file.h"
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
// and each refused node one problem `<stem>: key "<escaped key>" refused`. For a user that loaded,
// the primary group AS WRITTEN (`default` when the field is absent) must name a group `m` has, else
// one problem `<stem>: primary group "<g>" does not exist`; LoadUser's default step rewrites the
// stored one, so only the parsed value can be checked. That needs every group already in `m`:
// LoadHolders loads every group text before any user text, and a caller must do the same. True when
// the holder loaded (and is counted in `report`). A parent group a holder names is checked by
// ReportMissingGroups once every file is in. Engine-free.
bool LoadHolderText(std::string_view text, std::string_view stem, bool group, Model& m, LoadReport& report);

// The parent check that needs the whole store, for the holder `stem` already loaded into `m`: every
// true `group.<g>` node of a user or group must name a group `m` has. A false node is no parent and
// is not checked. Each miss is one problem, `<stem>: parent group "<g>" does not exist`. Call it for
// every holder after the last file is loaded: a group may name a parent that sorts after it.
void ReportMissingGroups(const Model& m, std::string_view stem, bool group, LoadReport& report);

// One holder file as read: its lower-cased stem, whether it is a group's, and its text.
struct HolderText {
    std::string stem;
    bool group;
    std::string text;
};

// Reads `dir` (`<server folder>\permissions`) into `*texts`, replacing it: every groups\*.json
// sorted by lower-cased stem, then every users\*.json sorted the same, so a user's parents come
// first. A missing `dir` is true with no texts. A group stem (lower-cased) must pass
// IsValidGroupName and a user stem be 32 hex; a file that fails, or cannot be read, is skipped
// with a problem appended to `*report`, as is a folder that cannot be listed. False only when
// `dir` itself could not be read (the problem `permissions: could not read the folder`).
bool ReadStoreTexts(const std::filesystem::path& dir, std::vector<HolderText>* texts, LoadReport* report);

// Loads `texts` into `m`: every group text, in order, through LoadHolderText, then every user text,
// then every holder that loaded through ReportMissingGroups. Expired nodes load as written
// (Applies filters them) and are left out when that holder is next written. `m` holds what loaded,
// even beside problems; whether to USE it is ShouldLoad's answer, never the caller's guess. The
// one load path: a store read from disk and a store held in memory both go through it.
LoadReport LoadHolders(const std::vector<HolderText>& texts, Model& m);

// ReadStoreTexts, then LoadHolders; the read problems come first in the returned report.
LoadReport LoadStore(const std::filesystem::path& dir, Model& m);

// The text of one holder's file, in the keys the loader reads and no other: for a user its
// `primaryGroup`, then `permissions`, one entry per node in the holder's store order (a `group.<g>`
// parent is an entry like any node). An entry is the plain string when the node is true, permanent
// and global, else an object with `permission` and only the `value` (false), `expiry` (non-zero)
// and `context` (not empty; a string for one value of a key, a list for more) the node needs. With
// `pruneExpired` a node whose expiry has passed (non-zero and below `now`) is left out. A key the
// loader does not read, and any formatting, in a hand-written file are not kept. PURE.
std::string SerializeHolder(const Holder& h, int64_t now, bool pruneExpired);

// True when the user holds nothing but the permanent, global, true `group.default` and has the
// primary `default`: LuckPerms keeps no file for such a user. With `ignoreExpired` a node whose
// expiry has passed is not counted; without it an expired node counts, as LuckPerms counts it. PURE.
bool IsDefaultUser(const Holder& user, int64_t now, bool ignoreExpired);

// True for the group named `default` with no node: the model always holds it, so an empty one is
// the default state and keeps no file, as a default-state user keeps none. PURE.
bool IsDefaultGroup(const Holder& g);

// Writes `*text` as `dir\groups\<stem>.json` (a group) or `dir\users\<stem>.json`, creating the
// folder when absent, through atomic_file::Write (Replace, ToDisk); with a null `text` deletes the
// file, a missing file or folder being success. `dir` is `<server folder>\permissions`. A folder
// that cannot be created is `Step::Open` with its error, a delete that fails `Step::Move` with its
// code. The stem is the caller's to validate. Any thread; no state.
atomic_file::Result WriteHolderFile(const std::filesystem::path& dir, bool group, std::string_view stem,
                                    const std::string* text);

// The store loads whole or not at all: true only for a report with no problem. A store with a
// problem is BROKEN, not empty: a dropped deny, or a refused group whose members inherited its
// denies, would widen a holder, and an empty model would hand a default-granted node back. A server
// with no store at all (no folder, or no file) has an empty report and is not broken. PURE.
bool ShouldLoad(const LoadReport& report);

}  // namespace coop::permissions
