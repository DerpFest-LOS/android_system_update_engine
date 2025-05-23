//
// Copyright (C) 2015 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
#include "update_engine/update_status_utils.h"

#include <base/logging.h>
#include <brillo/key_value_store.h>

using brillo::KeyValueStore;
using std::string;
using update_engine::UpdateEngineStatus;
using update_engine::UpdateStatus;

namespace chromeos_update_engine {

namespace {

// Note: Do not change these, autotest depends on these string variables being
// exactly these matches.
const char kCurrentOp[] = "CURRENT_OP";
const char kIsInstall[] = "IS_INSTALL";
const char kIsEnterpriseRollback[] = "IS_ENTERPRISE_ROLLBACK";
const char kLastCheckedTime[] = "LAST_CHECKED_TIME";
const char kNewSize[] = "NEW_SIZE";
const char kNewVersion[] = "NEW_VERSION";
const char kProgress[] = "PROGRESS";
const char kWillPowerwashAfterReboot[] = "WILL_POWERWASH_AFTER_REBOOT";

namespace update_engine {
const char kUpdateStatusIdle[] = "UPDATE_STATUS_IDLE";
const char kUpdateStatusCheckingForUpdate[] =
    "UPDATE_STATUS_CHECKING_FOR_UPDATE";
const char kUpdateStatusUpdateAvailable[] = "UPDATE_STATUS_UPDATE_AVAILABLE";
const char kUpdateStatusDownloading[] = "UPDATE_STATUS_DOWNLOADING";
const char kUpdateStatusVerifying[] = "UPDATE_STATUS_VERIFYING";
const char kUpdateStatusFinalizing[] = "UPDATE_STATUS_FINALIZING";
const char kUpdateStatusUpdatedNeedReboot[] =
    "UPDATE_STATUS_UPDATED_NEED_REBOOT";
const char kUpdateStatusReportingErrorEvent[] =
    "UPDATE_STATUS_REPORTING_ERROR_EVENT";
const char kUpdateStatusAttemptingRollback[] =
    "UPDATE_STATUS_ATTEMPTING_ROLLBACK";
const char kUpdateStatusDisabled[] = "UPDATE_STATUS_DISABLED";
const char kUpdateStatusNeedPermissionToUpdate[] =
    "UPDATE_STATUS_NEED_PERMISSION_TO_UPDATE";
const char kUpdateStatusCleanupPreviousUpdate[] =
    "UPDATE_STATUS_CLEANUP_PREVIOUS_UPDATE";
}  // namespace update_engine

}  // namespace

const char* UpdateStatusToString(const UpdateStatus& status) {
  switch (status) {
    case UpdateStatus::IDLE:
      return update_engine::kUpdateStatusIdle;
    case UpdateStatus::CHECKING_FOR_UPDATE:
      return update_engine::kUpdateStatusCheckingForUpdate;
    case UpdateStatus::UPDATE_AVAILABLE:
      return update_engine::kUpdateStatusUpdateAvailable;
    case UpdateStatus::NEED_PERMISSION_TO_UPDATE:
      return update_engine::kUpdateStatusNeedPermissionToUpdate;
    case UpdateStatus::DOWNLOADING:
      return update_engine::kUpdateStatusDownloading;
    case UpdateStatus::VERIFYING:
      return update_engine::kUpdateStatusVerifying;
    case UpdateStatus::FINALIZING:
      return update_engine::kUpdateStatusFinalizing;
    case UpdateStatus::UPDATED_NEED_REBOOT:
      return update_engine::kUpdateStatusUpdatedNeedReboot;
    case UpdateStatus::REPORTING_ERROR_EVENT:
      return update_engine::kUpdateStatusReportingErrorEvent;
    case UpdateStatus::ATTEMPTING_ROLLBACK:
      return update_engine::kUpdateStatusAttemptingRollback;
    case UpdateStatus::DISABLED:
      return update_engine::kUpdateStatusDisabled;
    case UpdateStatus::CLEANUP_PREVIOUS_UPDATE:
      return update_engine::kUpdateStatusCleanupPreviousUpdate;
  }

  NOTREACHED();
  return nullptr;
}

string UpdateEngineStatusToString(const UpdateEngineStatus& status) {
  KeyValueStore key_value_store;

  key_value_store.SetString(kLastCheckedTime,
                            std::format("{}", status.last_checked_time));
  key_value_store.SetString(kProgress, std::format("{}", status.progress));
  key_value_store.SetString(kNewSize, std::format("{}", status.new_size_bytes));
  key_value_store.SetString(kCurrentOp, UpdateStatusToString(status.status));
  key_value_store.SetString(kNewVersion, status.new_version);
  key_value_store.SetBoolean(kIsEnterpriseRollback,
                             status.is_enterprise_rollback);
  key_value_store.SetBoolean(kIsInstall, status.is_install);
  key_value_store.SetBoolean(kWillPowerwashAfterReboot,
                             status.will_powerwash_after_reboot);

  return key_value_store.SaveToString();
}

}  // namespace chromeos_update_engine
