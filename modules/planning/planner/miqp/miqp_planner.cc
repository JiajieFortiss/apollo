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

#include "modules/planning/planner/miqp/miqp_planner.h"

#include <limits>
#include <memory>
#include <utility>
#include <vector>

#include "cyber/common/log.h"
#include "cyber/common/macros.h"
#include "cyber/logger/logger_util.h"
#include "modules/common/math/path_matcher.h"
#include "modules/common/time/time.h"
#include "modules/planning/common/fortiss_common.h"
#include "modules/planning/common/planning_gflags.h"
#include "modules/planning/constraint_checker/collision_checker.h"
#include "modules/planning/constraint_checker/constraint_checker.h"

namespace apollo {
namespace planning {

using apollo::common::ErrorCode;
using apollo::common::PathPoint;
using apollo::common::Status;
using apollo::common::TrajectoryPoint;
using apollo::common::math::Box2d;
using apollo::common::math::Polygon2d;
using apollo::common::math::Vec2d;
using apollo::common::time::Clock;
using apollo::planning::DiscretizedTrajectory;
using apollo::planning::fortiss::MapOffset;

MiqpPlanner::MiqpPlanner() {
  // from cyber/logger/log_file_object.cc
  struct ::tm tm_time;
  const time_t timestamp = static_cast<time_t>(Clock::NowInSeconds());
  localtime_r(&timestamp, &tm_time);
  std::ostringstream time_pid_stream;
  time_pid_stream.fill('0');
  time_pid_stream << 1900 + tm_time.tm_year << std::setw(2)
                  << 1 + tm_time.tm_mon << std::setw(2) << tm_time.tm_mday
                  << '-' << std::setw(2) << tm_time.tm_hour << std::setw(2)
                  << tm_time.tm_min << std::setw(2) << tm_time.tm_sec << '.'
                  << apollo::cyber::logger::GetMainThreadPid();
  const std::string& time_pid_string = time_pid_stream.str();
  logdir_ += "/apollo/data/log/";
  // logdir_ += time_pid_string;
}

common::Status MiqpPlanner::Init(const PlanningConfig& config) {
  MiqpPlannerSettings settings = DefaultSettings();
  planner_ = NewCMiqpPlannerSettings(settings);
  firstrun_ = true;                      // add car only in first run
  egoCarIdx_ = -1;                       // set invalid
  minimum_valid_speed_planning_ = 1.0;   // below our model is invalid
  standstill_velocity_threshold_ = 0.1;  // set velocity hard to zero below this
  minimum_valid_speed_vx_vy_ = 0.5;  // below this individual speed threshold
                                     // for vx and vy the model is invalid

  LOG(INFO) << "Writing MIQP Planner Logs to " << logdir_.c_str();
  char logdir_cstr[logdir_.length()];
  strcpy(logdir_cstr, logdir_.c_str());
  char name_prefix_cstr[14];
  strcpy(name_prefix_cstr, "miqp_planner_");
  ActivateDebugFileWriteCMiqpPlanner(planner_, logdir_cstr, name_prefix_cstr);

  config_ = config;
  if (!config_.has_miqp_planner_config()) {
    AERROR << "Please provide miqp planner parameter file! " +
                  config_.DebugString();
    return Status(ErrorCode::PLANNING_ERROR,
                  "miqp planner parameters missing!");
  } else {
    AINFO << "MIQP Planner Configuration: "
          << config_.miqp_planner_config().DebugString();
  }

  return common::Status::OK();
}

void MiqpPlanner::Stop() { DelCMiqpPlanner(planner_); }

Status MiqpPlanner::PlanOnReferenceLine(
    const TrajectoryPoint& planning_init_point, Frame* frame,
    ReferenceLineInfo* reference_line_info) {
  const double timestep = Clock::NowInSeconds();
  AINFO << std::setprecision(15)
        << "############## MIQP Planner called at t = " << timestep;
  double current_time = timestep;
  const double start_time = timestep;
  const MapOffset map_offset(config_.miqp_planner_config().pts_offset_x(),
                             config_.miqp_planner_config().pts_offset_y());

  double stop_dist;
  fortiss::PlannerState planner_status = fortiss::DeterminePlannerState(
      planning_init_point.v(), reference_line_info, stop_dist,
      config_.miqp_planner_config().destination_distance_stop_threshold(),
      standstill_velocity_threshold_, minimum_valid_speed_planning_);

  if (planner_status == fortiss::PlannerState::STANDSTILL_TRAJECTORY) {
    fortiss::CreateStandstillTrajectory(planning_init_point,
                                        reference_line_info);
    return Status::OK();
  }

  // Initialized raw C trajectory output
  const int N = GetNCMiqpPlanner(planner_);
  double traj[TRAJECTORY_SIZE * N];
  int size;

  // Obtain a reference line and transform it to the PathPoint format.
  reference_line_info->set_is_on_reference_line();
  std::vector<PathPoint> discrete_reference_line =
      fortiss::ToDiscretizedReferenceLine(
          reference_line_info, stop_dist,
          config_.miqp_planner_config().cutoff_distance_reference_after_stop());

  // Reference line to raw c format
  const int ref_size =
      discrete_reference_line.size();  // aka N optimization support points
  AINFO << "Reference Line has " << ref_size << " points";
  double ref[ref_size * 2];
  for (int i = 0; i < ref_size; ++i) {
    PathPoint refPoint = discrete_reference_line.at(i);
    ref[2 * i] = refPoint.x() - config_.miqp_planner_config().pts_offset_x();
    ref[2 * i + 1] =
        refPoint.y() - config_.miqp_planner_config().pts_offset_y();
  }
  AINFO << "ReferenceLine Time [s] = "
        << (Clock::NowInSeconds() - current_time);
  current_time = Clock::NowInSeconds();

  // Map
  fortiss::RoadBoundaries road_bounds;
  if (config_.miqp_planner_config().use_environment_polygon()) {
    current_time = Clock::NowInSeconds();
    road_bounds = fortiss::ToLeftAndRightBoundary(reference_line_info);
    const int poly_size = road_bounds.left.size() + road_bounds.right.size();
    double poly_pts[poly_size * 2];
    fortiss::ConvertToPolyPts(road_bounds, map_offset, poly_pts);
    UpdateConvexifiedMapCMiqpPlaner(planner_, poly_pts, poly_size);
    AINFO << "Map Processing Time [s] = "
          << (Clock::NowInSeconds() - current_time);
  }

  // Initial State
  double initial_state[6];
  ConvertToInitialStateSecondOrder(planning_init_point, initial_state);

  // Target velocity
  bool track_ref_pos;
  double vDes;
  double deltaSDes;
  const double dist_start_slowdown =
      config_.miqp_planner_config().distance_start_slowdown();
  const double dist_stop_before =
      config_.miqp_planner_config().distance_stop_before();
  if ((stop_dist - dist_stop_before < dist_start_slowdown) &&
      (planner_status != fortiss::PlannerState::START_TRAJECTORY)) {
    track_ref_pos = false;  // only relevant for miqp
    vDes = 0;
    deltaSDes = std::max(0.0, stop_dist - dist_stop_before);
  } else if ((stop_dist - dist_stop_before < dist_start_slowdown) &&
             (planner_status == fortiss::PlannerState::START_TRAJECTORY)) {
    track_ref_pos = false;  // only relevant for miqp
    vDes = FLAGS_default_cruise_speed;
    deltaSDes = std::max(0.0, stop_dist - dist_stop_before);
  } else {
    track_ref_pos = true;
    vDes = FLAGS_default_cruise_speed;
    deltaSDes = config_.miqp_planner_config().delta_s_desired();
  }

  // Add/update ego car
  if (firstrun_) {
    current_time = Clock::NowInSeconds();
    egoCarIdx_ = AddCarCMiqpPlanner(planner_, initial_state, ref, ref_size,
                                    vDes, deltaSDes, timestep, track_ref_pos);
    firstrun_ = false;
    AINFO << "Added ego car, Time [s] = "
          << (Clock::NowInSeconds() - current_time);
  } else {
    current_time = Clock::NowInSeconds();
    UpdateCarCMiqpPlanner(planner_, egoCarIdx_, initial_state, ref, ref_size,
                          timestep, track_ref_pos);
    AINFO << "Update ego car Time [s] = "
          << (Clock::NowInSeconds() - current_time);
    current_time = Clock::NowInSeconds();
    UpdateDesiredVelocityCMiqpPlanner(planner_, egoCarIdx_, vDes, deltaSDes);
    AINFO << "UpdateDesiredVelocityCMiqpPlanner Time [s] = "
          << (Clock::NowInSeconds() - current_time);
  }

  // Obstacles as obstacles
  if (config_.miqp_planner_config().consider_obstacles()) {
    RemoveAllObstaclesCMiqpPlanner(planner_);
    bool success1 = ProcessStaticObstacles(frame->obstacles());
    bool success2 = ProcessDynamicObstacles(
        frame->obstacles(), planning_init_point.relative_time());
    if (!success1 || !success2) {
      AERROR << "Processing of obstacles failed";
      return Status(ErrorCode::PLANNING_ERROR,
                    "processing of obstacles failed!");
    }
  }

  // Plan
  DiscretizedTrajectory apollo_traj;
  if (planner_status == fortiss::PlannerState::START_TRAJECTORY ||
      planner_status == fortiss::PlannerState::STOP_TRAJECTORY) {
    AERROR << "Start/Stop Trajectory, using reference instead of miqp solution";
    GetRawCLastReferenceTrajectoryCMiqpPlaner(
        planner_, egoCarIdx_, planning_init_point.relative_time(), traj, size);
    apollo_traj = RawCTrajectoryToApolloTrajectory(traj, size, false);
  } else {
    current_time = Clock::NowInSeconds();
    bool success = PlanCMiqpPlanner(planner_, timestep);
    AINFO << "Miqp planning Time [s] = "
          << (Clock::NowInSeconds() - current_time);
    current_time = Clock::NowInSeconds();

    // Planning failed
    if (!success) {
      AINFO << "Planning failed";
      return Status(ErrorCode::PLANNING_ERROR, "miqp planner failed!");
    }

    // Get trajectory from miqp planner
    AINFO << "Planning Success!";
    // trajectories shall start at t=0 with an offset of
    // planning_init_point.relative_time()
    GetRawCMiqpTrajectoryCMiqpPlanner(
        planner_, egoCarIdx_, planning_init_point.relative_time(), traj, size);
    apollo_traj = RawCTrajectoryToApolloTrajectory(traj, size, true);
  }

  if (config_.miqp_planner_config().minimum_percentage_valid_miqp_points() *
          config_.miqp_planner_config().nr_steps() >
      apollo_traj.size()) {
    AERROR << "Trajectory has too many invalid points, setting error state";
    return Status(ErrorCode::PLANNING_ERROR, "invalid points!");
  }

  // Check resulting trajectory for collision with obstacles
  if (config_.miqp_planner_config().consider_obstacles()) {
    const auto& vehicle_config =
        common::VehicleConfigHelper::Instance()->GetConfig();
    const double ego_length = vehicle_config.vehicle_param().length();
    const double ego_width = vehicle_config.vehicle_param().width();
    const double ego_back_edge_to_center =
        vehicle_config.vehicle_param().back_edge_to_center();
    auto obstacles_non_virtual = fortiss::FilterNonVirtualObstacles(frame->obstacles());
    const bool obstacle_collision = CollisionChecker::InCollision(
        obstacles_non_virtual, apollo_traj, ego_length, ego_width,
        ego_back_edge_to_center);
    if (obstacle_collision) {
      AERROR << "Planning success but collision with obstacle!";
    }
  }

  // Check resulting trajectory for collision with environment
  if (config_.miqp_planner_config().use_environment_polygon()) {
    if (fortiss::EnvironmentCollision(road_bounds, apollo_traj)) {
      AERROR << "Planning success but collision with environment!";
    }
  }

  // Planning success -> publish trajectory
  Status return_status;
  int subsampling = 3;
  if (config_.miqp_planner_config().use_smoothing()) {
    auto smoothed_apollo_trajectory = fortiss::SmoothTrajectory(
        apollo_traj, planning_init_point, logdir_.c_str(), map_offset, subsampling);
    if (smoothed_apollo_trajectory.first) {
      reference_line_info->SetTrajectory(smoothed_apollo_trajectory.second);
      reference_line_info->SetCost(0);
      reference_line_info->SetDrivable(true);
      return_status = Status::OK();
    } else {
      return_status = Status(ErrorCode::PLANNING_ERROR, "Smoothing failed!");
    }
  } else {
    reference_line_info->SetTrajectory(apollo_traj);
    reference_line_info->SetCost(0);
    reference_line_info->SetDrivable(true);
    return_status = Status::OK();
  }

  AINFO << "MIQP Planner postprocess took [s]: "
        << (Clock::NowInSeconds() - current_time);
  AINFO << "MiqpPlanner::PlanOnReferenceLine() took "
        << (Clock::NowInSeconds() - start_time);

  return return_status;
}

DiscretizedTrajectory MiqpPlanner::RawCTrajectoryToApolloTrajectory(
    double traj[], int size, bool low_speed_check) {
  double s = 0.0f;
  double lastx =
      traj[0 + TRAJECTORY_X_IDX] + config_.miqp_planner_config().pts_offset_x();
  double lasty =
      traj[0 + TRAJECTORY_Y_IDX] + config_.miqp_planner_config().pts_offset_y();

  DiscretizedTrajectory apollo_trajectory;
  for (int trajidx = 0; trajidx < size; ++trajidx) {
    const double time = traj[trajidx * TRAJECTORY_SIZE + TRAJECTORY_TIME_IDX];
    const double x = traj[trajidx * TRAJECTORY_SIZE + TRAJECTORY_X_IDX] +
                     config_.miqp_planner_config().pts_offset_x();
    const double y = traj[trajidx * TRAJECTORY_SIZE + TRAJECTORY_Y_IDX] +
                     config_.miqp_planner_config().pts_offset_y();
    const double vx = traj[trajidx * TRAJECTORY_SIZE + TRAJECTORY_VX_IDX];
    const double vy = traj[trajidx * TRAJECTORY_SIZE + TRAJECTORY_VY_IDX];
    const double ax = traj[trajidx * TRAJECTORY_SIZE + TRAJECTORY_AX_IDX];
    const double ay = traj[trajidx * TRAJECTORY_SIZE + TRAJECTORY_AY_IDX];

    // at the first invalid vx vy point cut off the current trajectory
    if (low_speed_check && !IsVxVyValid(vx, vy)) {
      AINFO << "Trajectory at idx = " << trajidx << " has invalid (vx,vy) = ("
            << vx << ", " << vy << "); skipping further points.";
      break;
    }

    const double theta = atan2(vy, vx);
    const double v = vx / cos(theta);
    // const double a = ax / cos(theta); // probably wrong
    const double a = cos(theta) * ax + sin(M_PI_4 - theta) * ay;  // TODO: check
    s += sqrt(pow(x - lastx, 2) + pow(y - lasty, 2));
    double kappa;
    if ((vx * vx + vy * vy) < 1e-3) {
      kappa = 0;
    } else {
      kappa = (vx * ay - ax * vy) / (pow((vx * vx + vy * vy), 3 / 2));
    }
    TrajectoryPoint trajectory_point;
    trajectory_point.mutable_path_point()->set_x(x);
    trajectory_point.mutable_path_point()->set_y(y);
    trajectory_point.mutable_path_point()->set_s(s);
    trajectory_point.mutable_path_point()->set_theta(theta);
    trajectory_point.mutable_path_point()->set_kappa(kappa);
    trajectory_point.set_v(v);
    trajectory_point.set_a(a);
    trajectory_point.set_relative_time(time);
    apollo_trajectory.AppendTrajectoryPoint(trajectory_point);

    lastx = x;
    lasty = y;
  }
  fortiss::FillTimeDerivativesInApolloTrajectory(apollo_trajectory);

  for (size_t trajidx = 0; trajidx < apollo_trajectory.size(); ++trajidx) {
    AINFO << "Planned trajectory at i=" << trajidx << ": "
          << apollo_trajectory[trajidx].DebugString();
  }

  return apollo_trajectory;
}

bool MiqpPlanner::IsVxVyValid(const double& vx, const double& vy) {
  return (fabs(vx) > minimum_valid_speed_vx_vy_ ||
          fabs(vy) > minimum_valid_speed_vx_vy_);
}

void MiqpPlanner::ConvertToInitialStateSecondOrder(
    const TrajectoryPoint& planning_init_point, double initial_state[]) {
  // Intial position to raw c format
  AINFO << std::setprecision(15) << "planning_init_point = "
        << " rel. time:" << planning_init_point.relative_time()
        << " x:" << planning_init_point.path_point().x()
        << ", y:" << planning_init_point.path_point().y()
        << ", v:" << planning_init_point.v()
        << ", a:" << planning_init_point.a()
        << ", theta:" << planning_init_point.path_point().theta()
        << ", kappa:" << planning_init_point.path_point().kappa();

  double vel = std::max(planning_init_point.v(), 0.1);
  double theta = planning_init_point.path_point().theta();
  double kappa = planning_init_point.path_point().kappa();
  // cplex throws an exception if vel=0
  initial_state[0] = planning_init_point.path_point().x() -
                     config_.miqp_planner_config().pts_offset_x();
  initial_state[1] = vel * cos(theta);
  initial_state[2] =
      planning_init_point.a() * cos(theta) - pow(vel, 2) * kappa * sin(theta);
  initial_state[3] = planning_init_point.path_point().y() -
                     config_.miqp_planner_config().pts_offset_y();
  initial_state[4] = vel * sin(theta);
  initial_state[5] =
      planning_init_point.a() * sin(theta) + pow(vel, 2) * kappa * cos(theta);
  AINFO << std::setprecision(15)
        << "initial state in miqp = x:" << initial_state[0]
        << ", xd:" << initial_state[1] << ", xdd:" << initial_state[2]
        << ", y:" << initial_state[3] << ", yd:" << initial_state[4]
        << ", ydd:" << initial_state[5];
}

MiqpPlannerSettings MiqpPlanner::DefaultSettings() {
  MiqpPlannerSettings s = MiqpPlannerSettings();
  auto& conf = config_.miqp_planner_config();

  if (conf.has_nr_regions()) {
    s.nr_regions = conf.nr_regions();
  } else {
    s.nr_regions = 16;
  }
  if (conf.has_max_velocity_fitting()) {
    s.max_velocity_fitting = conf.max_velocity_fitting();
  } else {
    s.max_velocity_fitting = 10;
  }
  if (conf.has_nr_steps()) {
    s.nr_steps = conf.nr_steps();
  } else {
    s.nr_steps = 20;
  }
  if (conf.has_nr_neighbouring_possible_regions()) {
    s.nr_neighbouring_possible_regions =
        conf.nr_neighbouring_possible_regions();
  } else {
    s.nr_neighbouring_possible_regions = 1;
  }
  if (conf.has_ts()) {
    s.ts = conf.ts();
  } else {
    s.ts = 0.25;
  }
  if (conf.has_max_solution_time()) {
    s.max_solution_time = conf.max_solution_time();
  } else {
    s.max_solution_time = 5.0;
  }
  if (conf.has_relative_mip_gap_tolerance()) {
    s.relative_mip_gap_tolerance = conf.relative_mip_gap_tolerance();
  } else {
    s.relative_mip_gap_tolerance = 0.1;
  }
  if (conf.has_mipemphasis()) {
    s.mipemphasis = conf.mipemphasis();
  } else {
    s.mipemphasis = 1;
  }
  if (conf.has_relobjdif()) {
    s.relobjdif = conf.relobjdif();
  } else {
    s.relobjdif = 0.9;
  }
  if (conf.has_minimum_region_change_speed()) {
    s.minimum_region_change_speed = conf.minimum_region_change_speed();
  } else {
    s.minimum_region_change_speed = 2;
  }
  if (conf.has_additional_steps_for_reference_longer_horizon()) {
    s.additionalStepsForReferenceLongerHorizon =
        conf.additional_steps_for_reference_longer_horizon();
  } else {
    s.additionalStepsForReferenceLongerHorizon = 2;
  }
  if (conf.has_use_sos()) {
    s.useSos = conf.use_sos();
  } else {
    s.useSos = false;
  }
  if (conf.has_use_branching_priorities()) {
    s.useBranchingPriorities = conf.use_branching_priorities();
  } else {
    s.useBranchingPriorities = true;
  }
  if (conf.has_warmstart_type()) {
    s.warmstartType =
        static_cast<MiqpPlannerWarmstartType>(conf.warmstart_type());
  } else {
    s.warmstartType = MiqpPlannerWarmstartType::NO_WARMSTART;
  }
  float collision_radius_add;
  if (conf.has_collision_radius_add()) {
    collision_radius_add = conf.collision_radius_add();
  } else {
    collision_radius_add = 0.0;
  }
  float wheelbase_add;
  if (conf.has_wheelbase_add()) {
    wheelbase_add = conf.wheelbase_add();
  } else {
    wheelbase_add = 0.0;
  }
  if (conf.has_jerk_weight()) {
    s.jerkWeight = conf.jerk_weight();
  } else {
    s.jerkWeight = 1.0;
  }
  if (conf.has_position_weight()) {
    s.positionWeight = conf.position_weight();
  } else {
    s.positionWeight = 2.0;
  }
  if (conf.has_velocity_weight()) {
    s.velocityWeight = conf.velocity_weight();
  } else {
    s.velocityWeight = 0.0;
  }
  if (conf.has_obstacle_roi_filter()) {
    s.obstacle_roi_filter = conf.obstacle_roi_filter();
  } else {
    s.obstacle_roi_filter = false;
  }
  if (conf.has_obstacle_roi_behind_distance()) {
    s.obstacle_roi_behind_distance = conf.obstacle_roi_behind_distance();
  } else {
    s.obstacle_roi_behind_distance = 5.0;
  }
  if (conf.has_obstacle_roi_front_distance()) {
    s.obstacle_roi_front_distance = conf.obstacle_roi_front_distance();
  } else {
    s.obstacle_roi_front_distance = 30.0;
  }
  if (conf.has_obstacle_roi_side_distance()) {
    s.obstacle_roi_side_distance = conf.obstacle_roi_side_distance();
  } else {
    s.obstacle_roi_side_distance = 15.0;
  }
  s.wheelBase = common::VehicleConfigHelper::Instance()
                    ->GetConfig()
                    .vehicle_param()
                    .wheel_base() +
                wheelbase_add;
  s.collisionRadius = common::VehicleConfigHelper::Instance()
                              ->GetConfig()
                              .vehicle_param()
                              .width() /
                          2 +
                      collision_radius_add;

  s.slackWeight = 30;
  s.slackWeightObstacle = 2000;
  s.acclerationWeight = 0;
  if (conf.has_acc_lon_max_limit()) {
    s.accLonMaxLimit = conf.acc_lon_max_limit();
  } else {
    s.accLonMaxLimit = 2;
  }
  if (conf.has_acc_lon_min_limit()) {
    s.accLonMinLimit = conf.acc_lon_min_limit();
  } else {
    s.accLonMinLimit = -4;
  }
  if (conf.has_jerk_lon_max_limit()) {
    s.jerkLonMaxLimit = conf.jerk_lon_max_limit();
  } else {
    s.jerkLonMaxLimit = 3;
  }
  if (conf.has_acc_lat_min_max_limit()) {
    s.accLatMinMaxLimit = conf.acc_lat_min_max_limit();
  } else {
    s.accLatMinMaxLimit = 1.6;
  }
  if (conf.has_jerk_lat_min_max_limit()) {
    s.jerkLatMinMaxLimit = conf.jerk_lat_min_max_limit();
  } else {
    s.jerkLatMinMaxLimit = 1.4;
  }
  s.simplificationDistanceMap = 0.2;
  s.simplificationDistanceReferenceLine = 0.05;
  s.bufferReference = 1.0;
  s.buffer_for_merging_tolerance = 1.0;  // probably too high
  s.refLineInterpInc = 0.2;
  strcpy(s.cplexModelpath,
         "../bazel-bin/modules/planning/libplanning_component.so.runfiles/"
         "miqp_planner/cplex_modfiles/");
  s.mipdisplay = 3;
  s.cutpass = 0;
  s.probe = 0;
  s.repairtries = 5;
  s.rinsheur = 5;
  s.varsel = 0;
  s.mircuts = 0;
  s.precision = 12;
  s.constant_agent_safety_distance_slack = 3;
  s.lambda = 0.5;
  s.buffer_cplex_outputs = true;
  return s;
}

bool MiqpPlanner::ProcessStaticObstacles(
    const std::vector<const Obstacle*>& obstacles) {
  double ext_l = config_.miqp_planner_config().extension_length_static();
  double ext_w = config_.miqp_planner_config().extension_width_static();
  bool is_soft = (ext_w > 0 || ext_l > 0) ? true : false;

  std::vector<Polygon2d> static_polygons;
  for (const Obstacle* obstacle : obstacles) {
    bool merged = false;
    if (!obstacle->IsVirtual() && !obstacle->HasTrajectory()) {
      common::math::Box2d obst_box = obstacle->PerceptionBoundingBox();
      obst_box.LongitudinalExtend(ext_l);
      obst_box.LateralExtend(ext_w);
      const common::math::Polygon2d obst_poly = Polygon2d(obst_box);

      if (config_.miqp_planner_config().merge_static_obstacles()) {
        for (auto it = static_polygons.begin(); it != static_polygons.end();
             it++) {
          double d = obst_poly.DistanceTo(*it);
          if (d < config_.miqp_planner_config()
                      .static_obstacle_distance_criteria()) {
            // merge polygons!
            std::vector<Vec2d> vertices = obst_poly.GetAllVertices();
            std::vector<Vec2d> vsp = it->GetAllVertices();
            vertices.insert(vertices.end(), vsp.begin(), vsp.end());
            Polygon2d convex_polygon;
            Polygon2d::ComputeConvexHull(vertices, &convex_polygon);
            AINFO << "Not adding polygon from obstacle id " << obstacle->Id()
                  << " explicitly, but merging with existing";
            *it = convex_polygon;
            merged = true;
            break;
          }
        }
      }
      if (!merged) {
        AINFO << "Adding polygon from obstacle id" << obstacle->Id();
        static_polygons.push_back(obst_poly);
      }
    }
  }

  const int N = GetNCMiqpPlanner(planner_);
  for (const Polygon2d polygon : static_polygons) {
    double p1_x[N], p1_y[N], p2_x[N], p2_y[N], p3_x[N], p3_y[N], p4_x[N],
        p4_y[N];
    bool is_static = true;
    for (int i = 0; i < N; ++i) {
      FillInflatedPtsFromPolygon(polygon, p1_x[i], p1_y[i], p2_x[i], p2_y[i],
                                 p3_x[i], p3_y[i], p4_x[i], p4_y[i]);
    }

    int idx_obs =
        AddObstacleCMiqpPlanner(planner_, p1_x, p1_y, p2_x, p2_y, p3_x, p3_y,
                                p4_x, p4_y, N, is_static, is_soft);
    if (idx_obs != -1) {
      AINFO << "Added static obstacle "
            << " with miqp idx = " << idx_obs << " is_static = " << is_static;
    }
  }
  return true;
}

bool MiqpPlanner::ProcessDynamicObstacles(
    const std::vector<const Obstacle*>& obstacles, double timestep) {
  bool is_soft = true;  // make dynamic obstacles always soft!
  const int N = GetNCMiqpPlanner(planner_);
  for (const Obstacle* obstacle : obstacles) {
    double p1_x[N], p1_y[N], p2_x[N], p2_y[N], p3_x[N], p3_y[N], p4_x[N],
        p4_y[N];
    if (!obstacle->IsVirtual() && obstacle->HasTrajectory()) {
      const float ts = GetTsCMiqpPlanner(planner_);
      AINFO << "Dynamic obstacle " << obstacle->Id();
      for (int i = 0; i < N; ++i) {
        double pred_time = timestep + i * ts;
        TrajectoryPoint point = obstacle->GetPointAtTime(pred_time);

        common::math::Box2d box_i = obstacle->GetBoundingBox(point);
        AINFO << "idx: " << i << ", box: " << box_i.DebugString();
        box_i.LongitudinalExtend(
            config_.miqp_planner_config().extension_length_dynamic());
        AINFO << "idx: " << i << ", extended box: " << box_i.DebugString();
        common::math::Polygon2d poly2d_i = Polygon2d(box_i);
        FillInflatedPtsFromPolygon(poly2d_i, p1_x[i], p1_y[i], p2_x[i], p2_y[i],
                                   p3_x[i], p3_y[i], p4_x[i], p4_y[i]);
      }

      bool is_static = false;
      int idx_obs =
          AddObstacleCMiqpPlanner(planner_, p1_x, p1_y, p2_x, p2_y, p3_x, p3_y,
                                  p4_x, p4_y, N, is_static, is_soft);
      if (idx_obs != -1) {
        AINFO << "Added dynamic obstacle " << obstacle->Id()
              << " with miqp idx = " << idx_obs << " is_static = " << is_static
              << " is_soft = " << is_soft;
      }
    }
  }
  return true;
}

bool MiqpPlanner::FillInflatedPtsFromPolygon(const common::math::Polygon2d poly,
                                             double& p1_x, double& p1_y,
                                             double& p2_x, double& p2_y,
                                             double& p3_x, double& p3_y,
                                             double& p4_x, double& p4_y) {
  const double radius = GetCollisionRadius(planner_);
  common::math::Polygon2d poly2d_buff = poly.ExpandByDistance(radius);
  common::math::Box2d box_buff = poly2d_buff.MinAreaBoundingBox();
  std::vector<Vec2d> pts = box_buff.GetAllCorners();
  if (pts.size() != 4) {
    return false;
  }

  // TODO: could use MapOffset struct
  p1_x = pts.at(0).x() - config_.miqp_planner_config().pts_offset_x();
  p1_y = pts.at(0).y() - config_.miqp_planner_config().pts_offset_y();
  p2_x = pts.at(1).x() - config_.miqp_planner_config().pts_offset_x();
  p2_y = pts.at(1).y() - config_.miqp_planner_config().pts_offset_y();
  p3_x = pts.at(2).x() - config_.miqp_planner_config().pts_offset_x();
  p3_y = pts.at(2).y() - config_.miqp_planner_config().pts_offset_y();
  p4_x = pts.at(3).x() - config_.miqp_planner_config().pts_offset_x();
  p4_y = pts.at(3).y() - config_.miqp_planner_config().pts_offset_y();

  return true;
}

}  // namespace planning
}  // namespace apollo
