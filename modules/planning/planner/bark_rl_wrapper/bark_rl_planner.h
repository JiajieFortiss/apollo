/******************************************************************************
 * Copyright 2021 fortiss GmbH
 * Authors: Tobias Kessler, Klemens Esterle
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

/**
 * @file
 **/

#pragma once

#include <string>

#include "modules/common/status/status.h"
#include "modules/planning/common/frame.h"
#include "modules/planning/common/reference_line_info.h"
#include "modules/planning/planner/lattice/lattice_planner.h"
#include "modules/planning/planner/planner.h"
#include "modules/planning/proto/planning_config.pb.h"

namespace apollo {
namespace planning {

/**
 * @class BarkRlPlanner
 * @note LatticePlanner class from apollo served as a reference implementation!
 **/
class BarkRlPlanner : public LatticePlanner {
 public:
  BarkRlPlanner();

  virtual ~BarkRlPlanner() = default;

  std::string Name() override { return "BarkRlPlanner"; }

  common::Status Init(const PlanningConfig& config) override;

  void Stop() override;

  /**
   * @brief Override function Plan in parent class Planner.
   * @param planning_init_point The trajectory point where planning starts.
   * @param frame Current planning frame.
   * @param reference_line_info The computed reference line.
   * @return OK if planning succeeds; error otherwise.
   */
  common::Status PlanOnReferenceLine(
      const common::TrajectoryPoint& planning_init_point, Frame* frame,
      ReferenceLineInfo* reference_line_info) override;

 private:
  // TODO: convert obstacles to BarkObstacles (filter out virtual obstacles?)
  bool ProcessObstacles(const std::vector<const Obstacle*>& obstacles, double timestep);

 private:
  double minimum_valid_speed_planning_;
  double standstill_velocity_threshold_;
};

}  // namespace planning
}  // namespace apollo
