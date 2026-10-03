// coop/permissions/permissions_selftest.cpp -- the boot runner of the permission model's selftest;
// see coop/permissions/permissions_selftest.h. The only file of the folder that logs or reads config.

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

    if (sink.passed == sink.total) {
        UE_LOGI("permissions selftest: ALL PASS (%d checks)", sink.total);
        return true;
    }
    UE_LOGE("permissions selftest: %d/%d checks passed", sink.passed, sink.total);
    return false;
}

}  // namespace coop::permissions
