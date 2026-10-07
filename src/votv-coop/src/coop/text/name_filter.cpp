// coop/text/name_filter.cpp -- see coop/text/name_filter.h.

#include "coop/text/name_filter.h"

#include "coop/text/repertoire.h"
#include "coop/text/utf8_codec.h"
#include "ue_wrap/core/log.h"

namespace coop::text {

bool IsDeniedInName(uint32_t cp) {
    return cp < 0x20 || (cp >= 0x7F && cp <= 0x9F) || (cp >= 0xD800 && cp <= 0xDFFF) ||
           cp == 0x2028 || cp == 0x2029 || IsDefaultIgnorable(cp);
}

std::wstring FilterNameChars(const std::wstring& raw) {
    std::wstring out;
    out.reserve(raw.size());
    bool lastWasSpace = true;  // primes the leading-space trim
    // In codepoints: iterating wchar_t units cannot see a supplementary-plane character, so every
    // tag character, all ignorable, would pass the denylist as two anonymous halves.
    for (size_t i = 0; i < raw.size(); ) {
        uint32_t c = 0;
        const size_t units = DecodeCodepoint(raw, i, &c);
        const wchar_t* at = raw.data() + i;
        i += units;
        if (IsDeniedInName(c)) continue;
        if (c == L' ') {
            if (!lastWasSpace) { out.push_back(L' '); lastWasSpace = true; }
            continue;
        }
        // A combining mark with nothing to combine with stacks onto whatever the UI drew before the
        // name. Only at position 0: a mark in the middle is legitimate text in five scripts.
        // No hand-written mark range here: five scripts' marks draw now, and a range beside the
        // generated table would be a second owner of one fact, silently policing Latin diacritics
        // alone.
        if (out.empty() && IsCombiningMark(c)) continue;
        out.append(at, units);
        lastWasSpace = false;
    }
    return out;
}

bool RunNameFilterSelftest() {
    int pass = 0, total = 0;
    auto check = [&](bool ok, const char* what) {
        ++total;
        if (ok) { ++pass; return; }
        UE_LOGE("name_filter selftest FAIL: %s", what);
    };

    check(IsDeniedInName(0x1B) && IsDeniedInName(0x7F), "a C0 control and DEL are denied");
    check(IsDeniedInName(0x85) && IsDeniedInName(0x9B), "C1 controls are denied");
    check(IsDeniedInName(0xD800), "an unpaired surrogate is denied");
    check(IsDeniedInName(0x2028) && IsDeniedInName(0x2029),
          "the line and paragraph separators are denied");
    check(IsDeniedInName(0x200B) && IsDeniedInName(0x202E),
          "a zero-width space and a right-to-left override are denied");
    check(!IsDeniedInName(L'a') && !IsDeniedInName(L' '), "a letter and a space pass");
    check(!IsDeniedInName(0x0430) && !IsDeniedInName(0x00E9), "Cyrillic and Latin-1 letters pass");
    check(!IsDeniedInName(0x1F600), "an astral emoji passes");

    check(FilterNameChars(L"a\u0085b  c\u009B") == L"ab c", "C1 dropped, spaces collapsed");
    check(FilterNameChars(L"  x\u2028y") == L"xy", "leading spaces and a separator dropped");
    check(FilterNameChars(L"\u0301a") == L"a", "a leading combining mark is dropped");
    check(FilterNameChars(L"a\u0301") == L"a\u0301", "a mark after a base is kept");
    check(FilterNameChars(L"\u202Eevil") == L"evil", "a leading override is dropped");
    check(FilterNameChars(L"a\U0001F600b") == L"a\U0001F600b",
          "an astral character is kept whole");

    check(!AnyRepertoireCodepoint(&IsDeniedInName),
          "the repertoire and the name denylist do not overlap");

    if (pass == total) {
        UE_LOGI("name_filter selftest: ALL PASS (%d checks)", total);
        return true;
    }
    UE_LOGE("name_filter selftest: %d/%d checks passed", pass, total);
    return false;
}

}  // namespace coop::text
