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

#pragma once

#include <nlopt.hpp>

#include "Eigen/Dense"
#include "Eigen/SparseCore"
#include "modules/planning/common/trajectory/discretized_trajectory.h"

namespace apollo {
namespace planning {

class TrajectorySmootherNLOpt {
 public:
  struct ProblemParameters {
    ProblemParameters()
        : cost_offset_x(1e1),
          cost_offset_y(1e1),
          cost_offset_theta(0.0),
          cost_offset_v(1e1),
          cost_curvature(1e2),
          cost_acceleration(0.0),
          cost_curvature_change(2e1),
          cost_acceleration_change(2e0),
          lower_bound_acceleration(-8.0),
          upper_bound_acceleration(4.0),
          tol_acceleration(1e-2),
          lower_bound_curvature(-0.2),
          upper_bound_curvature(0.2),
          tol_curvature(1e-2),
          lower_bound_velocity(0.0),
          upper_bound_velocity(15.0),
          tol_velocity(1e-2),
          lower_bound_jerk(-5.0),  //(-1.2),
          upper_bound_jerk(5.0),   //(1.2),
          tol_jerk(1e-2),
          lower_bound_curvature_change(-5.0),  //(-0.2),
          upper_bound_curvature_change(5.0),   //(0.2),
          tol_curvature_change(1e-2) {}
    // costs for deviation from the initial reference
    double cost_offset_x;
    double cost_offset_y;
    double cost_offset_theta;
    double cost_offset_v;
    // costs on absolute values
    double cost_curvature;
    double cost_acceleration;
    // costs on input
    double cost_curvature_change;  // xi
    double cost_acceleration_change;
    // tolerances for the initial and final curvature
    double curvature_tolerance;
    // upper and lower bounds
    double lower_bound_acceleration;
    double upper_bound_acceleration;
    double tol_acceleration;
    double lower_bound_curvature;
    double upper_bound_curvature;
    double tol_curvature;
    double lower_bound_velocity;
    double upper_bound_velocity;
    double tol_velocity;
    double lower_bound_jerk;
    double upper_bound_jerk;
    double tol_jerk;
    double lower_bound_curvature_change;
    double upper_bound_curvature_change;
    double tol_curvature_change;
  };

  struct SolverParameters {
   public:
    SolverParameters()
        : algorithm(nlopt::LD_SLSQP),
          // : algorithm(nlopt::LN_BOBYQA), // exceptions with inequality
          // constraints : algorithm(nlopt::LN_NEWUOA_BOUND), // exceptions with
          // inequality constraints : algorithm(nlopt::LN_PRAXIS), // exceptions
          // with inequality constraints : 
          // : algorithm(nlopt::LN_COBYLA), // works but converges poorly : algorithm(nlopt::LD_MMA), // no convergence
          // : algorithm(nlopt::LD_AUGLAG), // no convergence, but a lot of
          // settings possible : algorithm(nlopt::GN_ISRES), // no convergence,
          x_tol_rel(1e-6),
          x_tol_abs(1e-6),
          ineq_const_tol(1e-4),
          eq_const_tol(1e-4),
          max_num_evals(1000),
          max_time(0.15) {}

    // algorithm to use for optimization. check NLOPT Documentation
    // http://ab-initio.mit.edu/wiki/index.php/NLopt_Algorithms
    nlopt::algorithm algorithm;
    // tolerance in relative (scaled by parameter value) change of the
    // parameters. relative tolerance has problems when optimal parameters are
    // close to zero
    double x_tol_rel;
    // tolerance in absolute change of the parameters
    double x_tol_abs;
    // tolerance for each inequality constraint
    double ineq_const_tol;
    // tolerance for each equality constraint
    double eq_const_tol;
    // maximum number of function evaluations
    size_t max_num_evals;
    // maximum time
    double max_time;
  };

  typedef Eigen::Matrix<double, 6, 1> Vector6d;
  typedef Eigen::Matrix<double, 6, 6> Matrix6d;

  explicit TrajectorySmootherNLOpt(const char logdir[],
                                   const double pts_offset_x,
                                   const double pts_offset_y);
  explicit TrajectorySmootherNLOpt(const char logdir[]);
  virtual ~TrajectorySmootherNLOpt() = default;

  int Optimize();

  void InitializeProblem(const int subsampling,
                         const DiscretizedTrajectory& input_trajectory,
                         const common::TrajectoryPoint& planning_init_point);

  DiscretizedTrajectory GetOptimizedTrajectory();

  // has to be public due to the function pointer wrapper
  double ObjectiveFunction(unsigned n, const double* x, double* grad);

  // bounds on acc and steering
  void InequalityConstraintFunction(unsigned m, double* result, unsigned n,
                                    const double* x, double* grad);
  // no equality constraints for now
  void EqualityConstraintFunction(unsigned m, double* result, unsigned n,
                                  const double* x, double* grad);

  std::vector<double>& GetInputVector() { return u_; }

  // Heuns Method, fill block matrices A_
  // x0: state at t=0 -> from vehicle state
  // u: initial input vector -> chosen by optimizer
  // X: stacked state vector over time
  // dXdU: X derived by u
  void IntegrateModel(const Vector6d& x0, const Eigen::VectorXd& u,
                      const size_t num_integration_steps, const double h,
                      Eigen::VectorXd& X, Eigen::MatrixXd& dXdU);

  void CalculateCommonDataIfNecessary(const Eigen::VectorXd& u);

  void model_f(const Vector6d& x, const Eigen::Vector2d& u, const double h,
               Vector6d& x_out) const;

  void model_dfdx(const Vector6d& x, const Eigen::Vector2d& u, const double h,
                  Matrix6d& dfdx_out) const;

  void model_dfdu(const Vector6d& x, const Eigen::Vector2d& u, const double h,
                  Eigen::MatrixXd& dfdxi_out) const;

  int GetNumEvals() const { return numevals_; }

  bool CheckConstraints(const std::vector<double>& u,
                        const Eigen::VectorXd& X) const;

  bool ValidateSmoothingSolution() const;

  SolverParameters GetSolverParameters() { return solver_params_; }

  void SetSolverParameters(const SolverParameters& params) {
    solver_params_ = params;
  }

  ProblemParameters GetProblemParameters() { return params_; }

  double BoundedJerk(const double val) const;

  bool IsJerkWithinBounds(const double j) const;

  double BoundedCurvatureChange(const double val) const;

  bool IsCurvatureChangeWithinBounds(const double xi) const;

  double BoundedAcceleration(const double val) const;

  bool IsAccelerationWithinBounds(const double a) const;

  double BoundedCurvature(const double val) const;

  bool IsCurvatureWithinBounds(const double kappa) const;

  double BoundedVelocity(const double val) const;

  bool IsVelocityWithinBounds(const double kappa) const;

  void CalculateJthreshold();

  bool CheckBoundsAfterIntegration(double jerk, double dkappa, size_t steps) const;

  void SetX0(const Vector6d& x0) {
    x0_ = x0;
  }

  void SetStepsize(const double h) {
    stepsize_ = h;
  }

 private:
  // stores the positions of the reference
  Eigen::VectorXd X_ref_;
  // stores the initial state
  Vector6d x0_;

  // stores the currently integrated trajectory
  Eigen::VectorXd X_;
  Eigen::VectorXd X_ub_;
  Eigen::VectorXd X_lb_;
  Eigen::SparseMatrix<double> C_kappa_;
  Eigen::SparseMatrix<double> C_vel_;
  // stores the gradient of the trajectory w.r.t. to the inputs of the
  // optimization
  Eigen::MatrixXd dXdU_;

  Vector6d currx_;
  Matrix6d currA_;
  Eigen::MatrixXd currB_;  // dimX x dimU
  Eigen::VectorXd last_u_;

  // why is this all using vector, not eigen? -> tk: because of the nlopt api.
  std::vector<double> u_;

  double j_opt_;
  double j_threshold_;
  int status_;
  int numevals_;
  std::vector<double> lower_bound_;
  std::vector<double> upper_bound_;
  std::vector<double> ineq_constraint_tol_;
  std::vector<double> eq_constraint_tol_;

  size_t problem_size_;
  size_t num_ineq_constr_;  // TODO(@Klemens) which ones?
  size_t num_eq_constr_;    // TODO(@Klemens) which ones?
  SolverParameters solver_params_;
  ProblemParameters params_;
  bool ready_to_optimize_;
  int input_traj_size_;
  int subsampling_;
  double stepsize_;
  int nr_integration_steps_;
  double initial_time_;
  size_t precision_ = 4;

  std::string logdir_;
  double pts_offset_x_;
  double pts_offset_y_;
  DiscretizedTrajectory modified_input_trajectory_;

  // Indices and sizes of our model
  enum STATES {
    X = 0,
    Y = 1,
    THETA = 2,
    V = 3,
    A = 4,
    KAPPA = 5,
    STATES_SIZE = 6
  };

  enum INPUTS { J = 0, XI = 1, INPUTS_SIZE = 2 };
};

void SaveDiscretizedTrajectoryToFile(
    const apollo::planning::DiscretizedTrajectory& traj,
    const std::string& path_to_file, const std::string& file_name);

double BoundValue(const double v, const double vmax, const double vmin,
                  const double tol);

double InterpolateWithinBounds(int idx0, double v0, int idx1, double v1,
                               int idx);

double Round(double a, size_t p);

}  // namespace planning
}  // namespace apollo
