// coop/permissions/permissions_selftest.cpp -- the boot runner of the permission model's selftest;
// see coop/permissions/permissions_selftest.h. This runner and `permission_host` (the in-game glue)
// are the only files of the folder that log or read config; every other file is engine-free.

#include "coop/permissions/permissions_selftest.h"

#include "coop/config/config.h"

#include "ue_wrap/core/log.h"

namespace coop::permissions {
namespace {

void ReportFail(const char* what) { UE_LOGE("permissions selftest FAIL: %s", what); }

}  // namespace

bool RunSelftest() {
    CheckSink sink;
    sink.breakFirst = coop::config::ResolveFlag(::coop::config_registry::rows::selftest_break_permissions);
    sink.onFail = &ReportFail;

    RunContextCases(sink);
    RunStoreCases(sink);
    RunInheritanceCases(sink);
    RunResolutionCases(sink);
    RunFilesCases(sink);
    RunGrantsCases(sink);

    if (sink.passed == sink.total) {
        UE_LOGI("permissions selftest: ALL PASS (%d checks)", sink.total);
        return true;
    }
    UE_LOGE("permissions selftest: %d/%d checks passed", sink.passed, sink.total);
    return false;
}

}  // namespace coop::permissions
