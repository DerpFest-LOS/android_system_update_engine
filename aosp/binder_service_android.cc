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

#include "update_engine/aosp/binder_service_android.h"

#include <memory>

#include <base/bind.h>
#include <base/logging.h>
#include <binderwrapper/binder_wrapper.h>
#include <utils/String8.h>

#include "update_engine/aosp/binder_service_android_common.h"
#include "update_engine/common/error_code.h"

using android::binder::Status;
using android::os::IUpdateEngineCallback;
using android::os::ParcelFileDescriptor;
using std::string;
using std::vector;
using update_engine::UpdateEngineStatus;

namespace chromeos_update_engine {

BinderUpdateEngineAndroidService::BinderUpdateEngineAndroidService(
    ServiceDelegateAndroidInterface* service_delegate)
    : service_delegate_(service_delegate) {}

void BinderUpdateEngineAndroidService::SendStatusUpdate(
    const UpdateEngineStatus& update_engine_status) {
  last_status_ = static_cast<int>(update_engine_status.status);
  last_progress_ = update_engine_status.progress;
  for (auto& callback : callbacks_) {
    callback->onStatusUpdate(last_status_, last_progress_);
  }
}

void BinderUpdateEngineAndroidService::SendPayloadApplicationComplete(
    ErrorCode error_code) {
  for (auto& callback : callbacks_) {
    callback->onPayloadApplicationComplete(static_cast<int>(error_code));
  }
}

Status BinderUpdateEngineAndroidService::bind(
    const android::sp<IUpdateEngineCallback>& callback, bool* return_value) {
  // Send an status update on connection (except when no update sent so far).
  // Even though the status update is oneway, it still returns an erroneous
  // status in case of a selinux denial. We should at least check this status
  // and fails the binding.
  if (last_status_ != -1) {
    auto status = callback->onStatusUpdate(last_status_, last_progress_);
    if (!status.isOk()) {
      LOG(ERROR) << "Failed to call onStatusUpdate() from callback: "
                 << status.toString8();
      *return_value = false;
      return Status::ok();
    }
  }

  callbacks_.emplace_back(callback);

  const android::sp<IBinder>& callback_binder =
      IUpdateEngineCallback::asBinder(callback);
  auto binder_wrapper = android::BinderWrapper::Get();
  binder_wrapper->RegisterForDeathNotifications(
      callback_binder,
      [this, callback = callback_binder.get()]() { UnbindCallback(callback); });

  *return_value = true;
  return Status::ok();
}

Status BinderUpdateEngineAndroidService::unbind(
    const android::sp<IUpdateEngineCallback>& callback, bool* return_value) {
  const android::sp<IBinder>& callback_binder =
      IUpdateEngineCallback::asBinder(callback);
  auto binder_wrapper = android::BinderWrapper::Get();
  binder_wrapper->UnregisterForDeathNotifications(callback_binder);

  *return_value = UnbindCallback(callback_binder.get());
  return Status::ok();
}

Status BinderUpdateEngineAndroidService::applyPayload(
    const android::String16& url,
    int64_t payload_offset,
    int64_t payload_size,
    const vector<android::String16>& header_kv_pairs) {
  const string payload_url{android::String8{url}.c_str()};
  vector<string> str_headers = ToVecString(header_kv_pairs);

  Error error;
  if (!service_delegate_->ApplyPayload(
          payload_url, payload_offset, payload_size, str_headers, &error)) {
    return ErrorPtrToStatus(error);
  }
  return Status::ok();
}

Status BinderUpdateEngineAndroidService::applyPayloadFd(
    const ParcelFileDescriptor& pfd,
    int64_t payload_offset,
    int64_t payload_size,
    const vector<android::String16>& header_kv_pairs) {
  vector<string> str_headers = ToVecString(header_kv_pairs);

  Error error;
  if (!service_delegate_->ApplyPayload(
          pfd.get(), payload_offset, payload_size, str_headers, &error)) {
    return ErrorPtrToStatus(error);
  }
  return Status::ok();
}

Status BinderUpdateEngineAndroidService::suspend() {
  Error error;
  if (!service_delegate_->SuspendUpdate(&error))
    return ErrorPtrToStatus(error);
  return Status::ok();
}

Status BinderUpdateEngineAndroidService::resume() {
  Error error;
  if (!service_delegate_->ResumeUpdate(&error))
    return ErrorPtrToStatus(error);
  return Status::ok();
}

Status BinderUpdateEngineAndroidService::cancel() {
  Error error;
  if (!service_delegate_->CancelUpdate(&error))
    return ErrorPtrToStatus(error);
  return Status::ok();
}

Status BinderUpdateEngineAndroidService::resetStatus() {
  Error error;
  if (!service_delegate_->ResetStatus(&error))
    return ErrorPtrToStatus(error);
  return Status::ok();
}

Status BinderUpdateEngineAndroidService::setShouldSwitchSlotOnReboot(
    const android::String16& metadata_filename) {
  Error error;
  if (!service_delegate_->setShouldSwitchSlotOnReboot(
          android::String8(metadata_filename).c_str(), &error)) {
    return ErrorPtrToStatus(error);
  }
  return Status::ok();
}

Status BinderUpdateEngineAndroidService::resetShouldSwitchSlotOnReboot() {
  Error error;
  if (!service_delegate_->resetShouldSwitchSlotOnReboot(&error)) {
    return ErrorPtrToStatus(error);
  }
  return Status::ok();
}

Status BinderUpdateEngineAndroidService::verifyPayloadApplicable(
    const android::String16& metadata_filename, bool* return_value) {
  const std::string payload_metadata{
      android::String8{metadata_filename}.c_str()};
  LOG(INFO) << "Received a request of verifying payload metadata in "
            << payload_metadata << ".";
  Error error;
  *return_value =
      service_delegate_->VerifyPayloadApplicable(payload_metadata, &error);
  if (error.error_code != ErrorCode::kSuccess)
    return ErrorPtrToStatus(error);
  return Status::ok();
}

bool BinderUpdateEngineAndroidService::UnbindCallback(const IBinder* callback) {
  auto it = std::find_if(
      callbacks_.begin(),
      callbacks_.end(),
      [&callback](const android::sp<IUpdateEngineCallback>& elem) {
        return IUpdateEngineCallback::asBinder(elem).get() == callback;
      });
  if (it == callbacks_.end()) {
    LOG(ERROR) << "Unable to unbind unknown callback.";
    return false;
  }
  callbacks_.erase(it);
  return true;
}

Status BinderUpdateEngineAndroidService::allocateSpaceForPayload(
    const android::String16& metadata_filename,
    const vector<android::String16>& header_kv_pairs,
    int64_t* return_value) {
  const std::string payload_metadata{
      android::String8{metadata_filename}.c_str()};
  vector<string> str_headers = ToVecString(header_kv_pairs);
  LOG(INFO) << "Received a request of allocating space for " << payload_metadata
            << ".";
  Error error;
  *return_value =
      static_cast<int64_t>(service_delegate_->AllocateSpaceForPayload(
          payload_metadata, str_headers, &error));
  if (error.error_code != ErrorCode::kSuccess)
    return ErrorPtrToStatus(error);
  return Status::ok();
}

class CleanupSuccessfulUpdateCallback
    : public CleanupSuccessfulUpdateCallbackInterface {
 public:
  CleanupSuccessfulUpdateCallback(
      const android::sp<IUpdateEngineCallback>& callback)
      : callback_(callback) {}
  void OnCleanupComplete(int32_t error_code) {
    ignore_result(callback_->onPayloadApplicationComplete(error_code));
  }
  void OnCleanupProgressUpdate(double progress) {
    ignore_result(callback_->onStatusUpdate(
        static_cast<int32_t>(
            update_engine::UpdateStatus::CLEANUP_PREVIOUS_UPDATE),
        progress));
  }
  void RegisterForDeathNotifications(const std::function<void()>& unbind) {
    const android::sp<android::IBinder>& callback_binder =
        IUpdateEngineCallback::asBinder(callback_);
    auto binder_wrapper = android::BinderWrapper::Get();
    binder_wrapper->RegisterForDeathNotifications(callback_binder, unbind);
  }

 private:
  android::sp<IUpdateEngineCallback> callback_;
};

Status BinderUpdateEngineAndroidService::cleanupSuccessfulUpdate(
    const android::sp<IUpdateEngineCallback>& callback) {
  Error error;
  service_delegate_->CleanupSuccessfulUpdate(
      std::make_unique<CleanupSuccessfulUpdateCallback>(callback), &error);
  if (error.error_code != ErrorCode::kSuccess)
    return ErrorPtrToStatus(error);
  return Status::ok();
}

Status BinderUpdateEngineAndroidService::triggerPostinstall(
    const ::android::String16& partition) {
  Error error;
  service_delegate_->TriggerPostinstall(android::String8(partition).c_str(),
                                        &error);
  if (error.error_code != ErrorCode::kSuccess)
    return ErrorPtrToStatus(error);
  return Status::ok();
}

Status BinderUpdateEngineAndroidService::setPerformanceMode(bool enable) {
  Error error;
  if (!service_delegate_->SetPerformanceMode(enable, &error))
    return ErrorPtrToStatus(error);
  return Status::ok();
}

}  // namespace chromeos_update_engine
