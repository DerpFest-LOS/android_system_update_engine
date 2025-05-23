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

#ifndef UPDATE_ENGINE_AOSP_NETWORK_SELECTOR_ANDROID_H_
#define UPDATE_ENGINE_AOSP_NETWORK_SELECTOR_ANDROID_H_

#include <android-base/macros.h>

#include "update_engine/common/network_selector_interface.h"

namespace chromeos_update_engine {

class NetworkSelectorAndroid final : public NetworkSelectorInterface {
 public:
  NetworkSelectorAndroid() = default;
  ~NetworkSelectorAndroid() override = default;

  // NetworkSelectorInterface overrides.
  bool SetProcessNetwork(NetworkId network_id) override;

 private:
  DISALLOW_COPY_AND_ASSIGN(NetworkSelectorAndroid);
};

}  // namespace chromeos_update_engine

#endif  // UPDATE_ENGINE_AOSP_NETWORK_SELECTOR_ANDROID_H_
