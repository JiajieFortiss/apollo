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
#include "src/miqp_planner_c_api.h"

namespace apollo {
namespace planning {

/**
 * @class MiqpPlanner
 * @note LatticePlanner class from apollo served as a reference implementation!
 **/
class MiqpPlanner : public LatticePlanner {
 public:
  MiqpPlanner();

  virtual ~MiqpPlanner() = default;

  std::string Name() override { return "MIQP"; }

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
  apollo::planning::DiscretizedTrajectory RawCTrajectoryToApolloTrajectory(
      double traj[], int size, bool low_speed_check);

  void ConvertToInitialStateSecondOrder(
      const common::TrajectoryPoint& planning_init_point,
      double initial_state[]);

  MiqpPlannerSettings DefaultSettings();

  bool ProcessStaticObstacles(const std::vector<const Obstacle*>& obstacles);

  bool ProcessDynamicObstacles(const std::vector<const Obstacle*>& obstacles,
                               double timestep);

  bool FillInflatedPtsFromPolygon(const common::math::Polygon2d poly,
                                  double& p1_x, double& p1_y, double& p2_x,
                                  double& p2_y, double& p3_x, double& p3_y,
                                  double& p4_x, double& p4_y);

  bool IsVxVyValid(const double& vx, const double& vy);

 private:
  CMiqpPlanner planner_;
  bool firstrun_;
  int egoCarIdx_;
  double minimum_valid_speed_planning_;
  double minimum_valid_speed_vx_vy_;
  double standstill_velocity_threshold_;
  std::string logdir_;
};

}  // namespace planning
}  // namespace apollo
