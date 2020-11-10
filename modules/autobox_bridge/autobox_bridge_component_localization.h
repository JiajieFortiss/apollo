/******************************************************************************
 * Copyright 2020 fortiss GmbH
 * Authors: Tobias Kessler, Jianjie Lin
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *****************************************************************************/

#pragma once

#include <mutex>
#include <thread>

#include "cyber/cyber.h"
#include "cyber/component/timer_component.h"
#include "cyber/timer/timer.h"
#include "modules/common/monitor_log/monitor_log_buffer.h"
#include "modules/localization/proto/localization.pb.h"
#include "modules/autobox_bridge/proto/autobox_bridge_conf.pb.h"
#include "modules/autobox_bridge/proto/autobox_localization.pb.h"

namespace apollo {
namespace autobox_bridge {

using apollo::cyber::Component;
using apollo::cyber::Reader;
using apollo::cyber::Writer;

class AutoBoxBridgeComponent_LOCALIZATION final: public apollo::cyber::TimerComponent{
 public:

  AutoBoxBridgeComponent_LOCALIZATION();

  bool Init() override;
  bool Proc() override;
 private:


  bool checkLocalizationMsg(const std::shared_ptr<apollo::localization::LocalizationEstimate>& trajectory_msg);
  void localizationMsgCallback(const std::shared_ptr<apollo::localization::LocalizationEstimate>& localization_msg);

  void  publishLocalization();

  apollo::common::monitor::MonitorLogBuffer monitor_logger_buffer_;
  std::shared_ptr<apollo::localization::LocalizationEstimate> localization_; //the last received loca

  bool new_localization_;

  std::shared_ptr<apollo::cyber::Reader<apollo::localization::LocalizationEstimate>> localization_reader_ = nullptr;

  std::shared_ptr<cyber::Writer<apollo::localization::LocalizationToAutoboxBridge>> localization_writer_;

  std::mutex localization_mutex_;

  bool check_timeouts_;

};

CYBER_REGISTER_COMPONENT(AutoBoxBridgeComponent_LOCALIZATION)

}  // namespace autobox_bridge
}  // namespace apollo
