/******************************************************************************
 * Copyright 2021 fortiss GmbH
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

#include "modules/fake_obstacle/fake_obstacle_component.h"

#include "modules/common/adapters/adapter_gflags.h"
#include "modules/fake_obstacle/proto/fake_obstacle_conf.pb.h"

namespace apollo {
namespace fake_obstacle {

// using apollo::hdmap::HDMapUtil;
using apollo::perception::PerceptionObstacles;
using apollo::routing::RoutingResponse;
using apollo::localization::LocalizationEstimate;

FakeObstacleComponent::FakeObstacleComponent()
    : monitor_logger_buffer_(common::monitor::MonitorMessageItem::PLANNING) {
  AERROR << "Started fake obstacle node!";
}

bool FakeObstacleComponent::Init() {
  // load proto file
  FakeObstacleConf fake_obst_config;
  if (!cyber::common::GetProtoFromFile(config_file_path_, &fake_obst_config)) {
    monitor_logger_buffer_.ERROR("Unable to load fake obstacle conf file: " +
                                 config_file_path_);
  }
  // initialize all readers and writers

  routing_reader_ = node_->CreateReader<RoutingResponse>(
      FLAGS_routing_response_topic,
      [this](const std::shared_ptr<RoutingResponse>& routing) {
        AINFO << "Received routing data: run routing callback."
              << routing->header().DebugString();
        std::lock_guard<std::mutex> lock(mutex_);
        latest_routing_.CopyFrom(*routing);
      });

  obstacle_writer_ =
      node_->CreateWriter<PerceptionObstacles>(FLAGS_perception_obstacle_topic);
  return true;
}

bool FakeObstacleComponent::Proc(const std::shared_ptr<LocalizationEstimate>&
        localization_estimate) {
  latest_localization_ = *localization_estimate;

  auto response = std::make_shared<PerceptionObstacles>();
  // TODO: fill response

  obstacle_writer_->Write(response);

  return true;
}

}  // namespace fake_obstacle
}  // namespace apollo
