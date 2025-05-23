//
// Copyright (C) 2016 The Android Open Source Project
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

#include <sysexits.h>
#include <unistd.h>

#include <chrono>
#include <string>
#include <vector>

#include <base/bind.h>
#include <base/callback.h>
#include <base/command_line.h>
#include <base/logging.h>
#include <base/strings/string_split.h>
#include <binder/IServiceManager.h>
#include <binderwrapper/binder_wrapper.h>
#include <brillo/binder_watcher.h>
#include <brillo/daemons/daemon.h>
#include <brillo/flag_helper.h>
#include <brillo/message_loops/message_loop.h>
#include <brillo/syslog_logging.h>
#include <utils/String16.h>
#include <utils/StrongPointer.h>

#include "android/os/BnUpdateEngineCallback.h"
#include "android/os/IUpdateEngine.h"
#include "update_engine/client_library/include/update_engine/update_status.h"
#include "update_engine/common/error_code.h"
#include "update_engine/common/error_code_utils.h"
#include "update_engine/update_status_utils.h"
#include "utils/String8.h"

using android::binder::Status;

namespace chromeos_update_engine {
namespace internal {

class UpdateEngineClientAndroid : public brillo::Daemon {
 public:
  UpdateEngineClientAndroid(int argc, char** argv) : argc_(argc), argv_(argv) {}

  int ExitWhenIdle(const Status& status);
  int ExitWhenIdle(int return_code);

 private:
  class UECallback : public android::os::BnUpdateEngineCallback {
   public:
    explicit UECallback(UpdateEngineClientAndroid* client) : client_(client) {}

    // android::os::BnUpdateEngineCallback overrides.
    Status onStatusUpdate(int status_code, float progress) override;
    Status onPayloadApplicationComplete(int error_code) override;

   private:
    UpdateEngineClientAndroid* client_;
  };

  int OnInit() override;

  // Called whenever the UpdateEngine daemon dies.
  void UpdateEngineServiceDied();
  // Register callback to watch for death notification from update_engine.
  void RegisterDeathNotification();

  static std::vector<android::String16> ParseHeaders(const std::string& arg);

  // Copy of argc and argv passed to main().
  int argc_;
  char** argv_;

  android::sp<android::os::IUpdateEngine> service_;
  android::sp<android::os::BnUpdateEngineCallback> callback_;
  android::sp<android::os::BnUpdateEngineCallback> cleanup_callback_;

  brillo::BinderWatcher binder_watcher_;
};

Status UpdateEngineClientAndroid::UECallback::onStatusUpdate(int status_code,
                                                             float progress) {
  update_engine::UpdateStatus status =
      static_cast<update_engine::UpdateStatus>(status_code);
  LOG(INFO) << "onStatusUpdate(" << UpdateStatusToString(status) << " ("
            << status_code << "), " << progress << ")";
  return Status::ok();
}

Status UpdateEngineClientAndroid::UECallback::onPayloadApplicationComplete(
    int error_code) {
  ErrorCode code = static_cast<ErrorCode>(error_code);
  LOG(INFO) << "onPayloadApplicationComplete(" << utils::ErrorCodeToString(code)
            << " (" << error_code << "))";
  client_->ExitWhenIdle(
      (code == ErrorCode::kSuccess || code == ErrorCode::kUpdatedButNotActive)
          ? EX_OK
          : 1);
  return Status::ok();
}

constexpr auto&& UNSPECIFIED_FLAG = "unspecified";

void UpdateEngineClientAndroid::RegisterDeathNotification() {
  // When following updates status changes, exit if the update_engine daemon
  // dies.
  android::BinderWrapper::Create();
  android::BinderWrapper::Get()->RegisterForDeathNotifications(
      android::os::IUpdateEngine::asBinder(service_),
      [this]() { UpdateEngineServiceDied(); });
}

int UpdateEngineClientAndroid::OnInit() {
  int ret = Daemon::OnInit();
  if (ret != EX_OK)
    return ret;

  DEFINE_bool(update, false, "Start a new update, if no update in progress.");
  DEFINE_string(payload,
                "http://127.0.0.1:8080/payload",
                "The URI to the update payload to use.");
  DEFINE_int64(offset,
               0,
               "The offset in the payload where the CrAU update starts. "
               "Used when --update is passed.");
  DEFINE_int64(size,
               0,
               "The size of the CrAU part of the payload. If 0 is passed, it "
               "will be autodetected. Used when --update is passed.");
  DEFINE_string(headers,
                "",
                "A list of key-value pairs, one element of the list per line. "
                "Used when --update or --allocate is passed.");

  DEFINE_bool(verify,
              false,
              "Given payload metadata, verify if the payload is applicable.");
  DEFINE_bool(allocate, false, "Given payload metadata, allocate space.");
  DEFINE_string(metadata,
                "/data/ota_package/metadata",
                "The path to the update payload metadata. "
                "Used when --verify or --allocate is passed.");

  DEFINE_string(switch_slot,
                UNSPECIFIED_FLAG,
                "Perform just the slow switching part of OTA. "
                "Used to revert a slot switch or re-do slot switch. Valid "
                "values are 'true' and 'false'");
  DEFINE_string(
      trigger_postinstall,
      UNSPECIFIED_FLAG,
      "Only run postinstall sciprts. And only run postinstall script for the "
      "specified partition. Example: \"system\", \"product\"");
  DEFINE_bool(suspend, false, "Suspend an ongoing update and exit.");
  DEFINE_bool(resume, false, "Resume a suspended update.");
  DEFINE_bool(cancel, false, "Cancel the ongoing update and exit.");
  DEFINE_bool(reset_status, false, "Reset an already applied update and exit.");
  DEFINE_bool(follow,
              false,
              "Follow status update changes until a final state is reached. "
              "Exit status is 0 if the update succeeded, and 1 otherwise.");
  DEFINE_bool(merge,
              false,
              "Wait for previous update to merge. "
              "Only available after rebooting to new slot.");
  DEFINE_bool(perf_mode, false, "Enable perf mode.");
  // Boilerplate init commands.
  base::CommandLine::Init(argc_, argv_);
  brillo::FlagHelper::Init(argc_, argv_, "Android Update Engine Client");
  if (argc_ == 1) {
    LOG(ERROR) << "Nothing to do. Run with --help for help.";
    return 1;
  }

  // Ensure there are no positional arguments.
  const std::vector<std::string> positional_args =
      base::CommandLine::ForCurrentProcess()->GetArgs();
  if (!positional_args.empty()) {
    LOG(ERROR) << "Found a positional argument '" << positional_args.front()
               << "'. If you want to pass a value to a flag, pass it as "
                  "--flag=value.";
    return 1;
  }

  bool keep_running = false;
  brillo::InitLog(brillo::kLogToStderr);

  // Initialize a binder watcher early in the process before any interaction
  // with the binder driver.
  binder_watcher_.Init();

  android::status_t status = android::getService(
      android::String16("android.os.UpdateEngineService"), &service_);
  if (status != android::OK) {
    LOG(ERROR) << "Failed to get IUpdateEngine binder from service manager: "
               << Status::fromStatusT(status).toString8();
    return ExitWhenIdle(1);
  }

  // Other commands, such as |setShouldSwitchSlotOnReboot|, might rely on the
  // follow behavior, so created callback before running these commands.
  if (FLAGS_follow) {
    // Register a callback object with the service.
    callback_ = new UECallback(this);
    bool bound = false;
    if (!service_->bind(callback_, &bound).isOk() || !bound) {
      LOG(ERROR) << "Failed to bind() the UpdateEngine daemon.";
      return 1;
    }
    keep_running = true;
  }

  if (FLAGS_suspend) {
    return ExitWhenIdle(service_->suspend());
  }

  if (FLAGS_resume) {
    return ExitWhenIdle(service_->resume());
  }

  if (FLAGS_cancel) {
    return ExitWhenIdle(service_->cancel());
  }

  if (FLAGS_reset_status) {
    return ExitWhenIdle(service_->resetStatus());
  }

  if (FLAGS_trigger_postinstall != UNSPECIFIED_FLAG) {
    return ExitWhenIdle(service_->triggerPostinstall(
        android::String16(FLAGS_trigger_postinstall.c_str())));
  }

  if (FLAGS_switch_slot != UNSPECIFIED_FLAG) {
    if (FLAGS_switch_slot != "true" && FLAGS_switch_slot != "false") {
      LOG(ERROR) << "--switch_slot should be either true or false, got "
                 << FLAGS_switch_slot;
      return 1;
    }
    const bool should_switch = FLAGS_switch_slot == "true";
    ::android::binder::Status status;
    if (should_switch) {
      status = service_->setShouldSwitchSlotOnReboot(
          android::String16(FLAGS_metadata.c_str(), FLAGS_metadata.size()));
      if (!FLAGS_follow) {
        return ExitWhenIdle(status);
      }
    } else {
      // resetShouldSwitchSlotOnReboot() is a synchronous call, no need to
      // follow
      status = service_->resetShouldSwitchSlotOnReboot();
      return ExitWhenIdle(status);
    }
  }

  if (FLAGS_verify) {
    bool applicable = false;
    Status status = service_->verifyPayloadApplicable(
        android::String16{FLAGS_metadata.data(), FLAGS_metadata.size()},
        &applicable);
    LOG(INFO) << "Payload is " << (applicable ? "" : "not ") << "applicable.";
    return ExitWhenIdle(status);
  }

  if (FLAGS_allocate) {
    auto headers = ParseHeaders(FLAGS_headers);
    int64_t ret = 0;
    Status status = service_->allocateSpaceForPayload(
        android::String16{FLAGS_metadata.data(), FLAGS_metadata.size()},
        headers,
        &ret);
    if (status.isOk()) {
      if (ret == 0) {
        LOG(INFO) << "Successfully allocated space for payload.";
      } else {
        LOG(INFO) << "Insufficient space; required " << ret << " bytes.";
      }
    } else {
      LOG(INFO) << "Allocation failed.";
    }
    return ExitWhenIdle(status);
  }

  if (FLAGS_merge) {
    // Register a callback object with the service.
    cleanup_callback_ = new UECallback(this);
    Status status = service_->cleanupSuccessfulUpdate(cleanup_callback_);
    if (!status.isOk()) {
      LOG(ERROR) << "Failed to call cleanupSuccessfulUpdate.";
      return ExitWhenIdle(status);
    }
    keep_running = true;
  }

  if (FLAGS_perf_mode) {
    return ExitWhenIdle(service_->setPerformanceMode(true));
  }

  if (FLAGS_update) {
    auto and_headers = ParseHeaders(FLAGS_headers);
    Status status = service_->applyPayload(
        android::String16{FLAGS_payload.data(), FLAGS_payload.size()},
        FLAGS_offset,
        FLAGS_size,
        and_headers);
    if (!status.isOk())
      return ExitWhenIdle(status);
  }

  if (!keep_running)
    return ExitWhenIdle(EX_OK);

  RegisterDeathNotification();
  return EX_OK;
}

int UpdateEngineClientAndroid::ExitWhenIdle(const Status& status) {
  if (status.isOk())
    return ExitWhenIdle(EX_OK);
  LOG(ERROR) << status.toString8();
  return ExitWhenIdle(status.exceptionCode());
}

int UpdateEngineClientAndroid::ExitWhenIdle(int return_code) {
  auto delayed_exit = base::Bind(
      &Daemon::QuitWithExitCode, base::Unretained(this), return_code);
  if (!brillo::MessageLoop::current()->PostTask(delayed_exit))
    return 1;
  return EX_OK;
}

void UpdateEngineClientAndroid::UpdateEngineServiceDied() {
  LOG(ERROR) << "UpdateEngineService died.";
  QuitWithExitCode(1);
}

std::vector<android::String16> UpdateEngineClientAndroid::ParseHeaders(
    const std::string& arg) {
  std::vector<std::string> headers = base::SplitString(
      arg, "\n", base::KEEP_WHITESPACE, base::SPLIT_WANT_NONEMPTY);
  std::vector<android::String16> and_headers;
  for (const auto& header : headers) {
    and_headers.push_back(android::String16{header.data(), header.size()});
  }
  return and_headers;
}

}  // namespace internal
}  // namespace chromeos_update_engine

int main(int argc, char** argv) {
  const auto start = std::chrono::system_clock::now();
  chromeos_update_engine::internal::UpdateEngineClientAndroid client(argc,
                                                                     argv);
  const auto ret = client.Run();
  const auto end = std::chrono::system_clock::now();
  const auto duration = end - start;
  LOG(INFO)
      << "Command took "
      << std::chrono::duration_cast<std::chrono::milliseconds>(duration).count()
      << " ms";
  return ret;
}
