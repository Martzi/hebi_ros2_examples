#include "hebi_ros2_examples/omni_base.hpp"

namespace hebi {

OmniBaseTrajectory OmniBaseTrajectory::create(const Eigen::VectorXd& dest_positions, double t_now) {
  OmniBaseTrajectory base_trajectory;

  // Set up initial trajectory
  Eigen::MatrixXd positions(3, 1);
  positions.col(0) = dest_positions;
  base_trajectory.replan(t_now, positions);

  return base_trajectory;
}

void OmniBaseTrajectory::getState(
  double t_now, 
  Eigen::VectorXd& positions,
  Eigen::VectorXd& velocities,
  Eigen::VectorXd& accelerations) {

  // (Cap the effective time to the end of the trajectory)
  double t = std::min(t_now - trajectory_start_time_,
                      trajectory_->getDuration());

  trajectory_->getState(t, &positions, &velocities, &accelerations);
}

void OmniBaseTrajectory::replanVel(double t_now, const Eigen::Vector3d& target_vel) {
  Eigen::MatrixXd positions(3, 4);
  Eigen::MatrixXd velocities(3, 4);
  Eigen::MatrixXd accelerations(3, 4);
  // One second to get up to velocity, and then keep going for at least 1 second.
  Eigen::VectorXd times(4);
  times << 0, 0.25, 1, 1.25;

  // Initial state
  // Start from (0, 0, 0), as this is a relative motion.

  // Copy new waypoints
  auto nan = std::numeric_limits<double>::quiet_NaN();
  positions.col(0).setZero();
  positions.col(1).setConstant(nan);
  positions.col(2).setConstant(nan);
  positions.col(3).setConstant(nan);

  // Get last command to smoothly maintain commanded velocity
  Eigen::VectorXd p, v, a;
  p.resize(3);
  v.resize(3);
  a.resize(3);
  if (trajectory_) {
    getState(t_now, p, v, a);
  } else {
    p.setZero();
    v.setZero();
    a.setZero();
  }

  // Transform last velocity (local from trajectory start) into local frame
  double theta = p[2];
  double ctheta = std::cos(-theta);
  double stheta = std::sin(-theta);
  double dx = v[0] * ctheta - v[1] * stheta;
  double dy = v[0] * stheta + v[1] * ctheta;
  double dtheta = v[2];

  Eigen::Vector3d curr_vel;
  curr_vel << dx, dy, dtheta;

  velocities.col(0) = curr_vel;
  velocities.col(1) = velocities.col(2) = target_vel;
  velocities.col(3).setZero();

  accelerations.col(0).setZero();
  accelerations.col(1).setZero();
  accelerations.col(2).setZero();
  accelerations.col(3).setZero();

  // Create new trajectory
  trajectory_ = hebi::trajectory::Trajectory::createUnconstrainedQp(
                  times, positions, &velocities, &accelerations);
  trajectory_start_time_ = t_now;
}

// Updates the Base State by planning a trajectory to a given set of joint
// waypoints.  Uses the current trajectory/state if defined.
// NOTE: this call assumes feedback is populated.
void OmniBaseTrajectory::replan(
  double t_now,
  const Eigen::MatrixXd& new_positions,
  const Eigen::MatrixXd& new_velocities,
  const Eigen::MatrixXd& new_accelerations) {

  int num_joints = new_positions.rows();

  // Start from (0, 0, 0), as this is a relative motion.
  Eigen::VectorXd curr_pos = Eigen::VectorXd::Zero(num_joints);
  Eigen::VectorXd curr_vel = Eigen::VectorXd::Zero(num_joints);
  Eigen::VectorXd curr_accel = Eigen::VectorXd::Zero(num_joints);

  int num_waypoints = new_positions.cols() + 1;

  Eigen::MatrixXd positions(num_joints, num_waypoints);
  Eigen::MatrixXd velocities(num_joints, num_waypoints);
  Eigen::MatrixXd accelerations(num_joints, num_waypoints);

  // Initial state
  positions.col(0) = curr_pos;
  velocities.col(0) = curr_vel;
  accelerations.col(0) = curr_accel;

  // Copy new waypoints
  positions.rightCols(num_waypoints - 1) = new_positions;
  velocities.rightCols(num_waypoints - 1) = new_velocities;
  accelerations.rightCols(num_waypoints - 1) = new_accelerations;

  // Get waypoint times
  Eigen::VectorXd trajTime =
    getWaypointTimes(positions, velocities, accelerations);

  // Create new trajectory
  trajectory_ = hebi::trajectory::Trajectory::createUnconstrainedQp(
                  trajTime, positions, &velocities, &accelerations);
  trajectory_start_time_ = t_now;
}
  
// Updates the Base State by planning a trajectory to a given set of joint
// waypoints.  Uses the current trajectory/state if defined.
// NOTE: this is a wrapper around the more general replan that
// assumes zero end velocity and acceleration.
void OmniBaseTrajectory::replan(
  double t_now,
  const Eigen::MatrixXd& new_positions) {

  int num_joints = new_positions.rows();
  int num_waypoints = new_positions.cols();

  // Unconstrained velocities and accelerations during the path, but set to
  // zero at the end.
  double nan = std::numeric_limits<double>::quiet_NaN();

  Eigen::MatrixXd velocities(num_joints, num_waypoints);
  velocities.setConstant(nan);
  velocities.rightCols<1>() = Eigen::VectorXd::Zero(num_joints);

  Eigen::MatrixXd accelerations(num_joints, num_waypoints);
  accelerations.setConstant(nan);
  accelerations.rightCols<1>() = Eigen::VectorXd::Zero(num_joints);

  replan(t_now, new_positions, velocities, accelerations);
}

// Heuristic to get the timing of the waypoints. This function can be
// modified to add custom waypoint timing.
Eigen::VectorXd OmniBaseTrajectory::getWaypointTimes(
  const Eigen::MatrixXd& positions,
  const Eigen::MatrixXd& velocities,
  const Eigen::MatrixXd& accelerations) {

  // TODO: make this configurable!
  double rampTime = 4.0; // Per meter
  double dist = std::pow(positions(0, 1) - positions(0, 0), 2) + 
                std::pow(positions(1, 1) - positions(1, 0), 2);
  dist = std::sqrt(dist);

  static constexpr double base_radius = 0.245; // m (center of omni to origin of base)
  double rot_dist = std::abs(positions(2, 1) - positions(2, 0)) * base_radius;

  dist += rot_dist;

  rampTime *= dist;
  rampTime = std::max(rampTime, 1.5);

  size_t num_waypoints = positions.cols();

  Eigen::VectorXd times(num_waypoints);
  for (size_t i = 0; i < num_waypoints; ++i)
    times[i] = rampTime * (double)i;

  return times;
};    

std::unique_ptr<OmniBase> OmniBase::create(
  const std::vector<std::string>& families,
  const std::vector<std::string>& names,
  const std::string& gains_file,
  double start_time,
  std::string& error_out)
{
  // Invalid input!  Size mismatch.  An omnibase has three wheels.
  if (names.size() != 3 || (families.size() != 1 && families.size() != 3)) {
    assert(false);
    return nullptr;
  }

  // Try to find the modules on the network
  Lookup lookup;
  auto group = lookup.getGroupFromNames(families, names, 10000);
  if (!group)
    return nullptr;

  // Load the appropriate gains file.  Try to set 5 times before giving up.
  {
    hebi::GroupCommand gains_cmd(group->size());
    bool success = gains_cmd.readGains(gains_file);
    if (!success)
      error_out = "Could not load base gains file!";
    else
    {
      for (size_t i = 0; i < 5; ++i)
      {
        success = group->sendCommandWithAcknowledgement(gains_cmd);
        if (success)
          break;
      }
      if (!success)
        error_out = "Could not set base gains!";
    }
  }

  constexpr double feedback_frequency = 100;
  group->setFeedbackFrequencyHz(feedback_frequency);
  constexpr long command_lifetime = 250;
  group->setCommandLifetimeMs(command_lifetime);

  // Try to get feedback -- if we don't get a packet in the first N times,
  // something is wrong
  int num_attempts = 0;
  
  // This whole "plan initial trajectory" is a little hokey...but it's better than nothing  
  GroupFeedback feedback(group->size());
  while (!group->getNextFeedback(feedback)) {
    if (num_attempts++ > 20) {
      return nullptr;
    }
  }

  // NOTE: I don't like that start time is _before_ the "get feedback"
  // loop above...but this is only during initialization
  OmniBaseTrajectory base_trajectory = OmniBaseTrajectory::create(Eigen::Vector3d::Zero(), start_time);
  return std::unique_ptr<OmniBase>(new OmniBase(group, base_trajectory, start_time));
}

bool OmniBase::update(double time) {

  double dt = 0; 
  if (last_time_ < 0) { // Sentinal value set when we restart...
    last_time_ = time;
  } else {
    dt = time - last_time_;
  }

  if (!group_->getNextFeedback(feedback_))
    return false;

  // Update odometry
  if (dt > 0) {
    updateOdometry(feedback_.getVelocity(), dt);
  }

  // Update command from trajectory
  base_trajectory_.getState(time, pos_, vel_, accel_);

  // Convert from x/y/theta to wheel 1/2/3
  convertSE2ToWheel();

  // Integrate position using wheel velocities.
  last_wheel_pos_ += wheel_vel_ * dt;
  command_.setPosition(last_wheel_pos_);

  // Use velocity from trajectory, converted from x/y/theta into wheel velocities above.
  command_.setVelocity(wheel_vel_);

  for (int i = 0; i < 3; ++i) {
    command_[i].led().set(color_);
  }

  group_->sendCommand(command_);

  last_time_ = time;

  return true; 
}
 
double OmniBase::trajectoryPercentComplete(double time) {
  return std::min((time - base_trajectory_.getTrajStartTime()) / base_trajectory_.getTraj()->getDuration(), 1.0) * 100;
}

bool OmniBase::isTrajectoryComplete(double time) {
  return time > base_trajectory_.getTrajEndTime();
}
  
void OmniBase::resetStart(Color& color) {
  start_wheel_pos_ = feedback_.getPosition();
  last_wheel_pos_ = start_wheel_pos_;
  last_time_ = -1;
  color_ = color;
}

void OmniBase::clearColor() {
  Color c(0, 0, 0, 0);
  color_ = c;
}
  
OmniBase::OmniBase(std::shared_ptr<Group> group,
  OmniBaseTrajectory base_trajectory,
  double start_time)
  : group_(group),
    feedback_(group->size()),
    command_(group->size()),
    pos_(Eigen::VectorXd::Zero(group->size())),
    vel_(Eigen::VectorXd::Zero(group->size())),
    accel_(Eigen::VectorXd::Zero(group->size())),
    start_wheel_pos_(Eigen::VectorXd::Zero(group->size())),
    last_wheel_pos_(Eigen::VectorXd::Zero(group->size())),
    wheel_vel_(Eigen::VectorXd::Zero(group->size())),
    base_trajectory_{base_trajectory}
{}

// Converts a certain number of radians into radians that each wheel would turn
// _from theta == 0_ to obtain this rotation.
// Note: we only do this for velocities in order to combine rotations and translations without doing gnarly
// integrations.
void OmniBase::convertSE2ToWheel() {

  // Note -- this converts SE2 positions along a trajectory starting from the initial position of
  // the robot during this move, not the current local position.  This is designed to be paired
  // ith the "base_trajectory_" interpolation code!

  double theta = pos_[2];
  double dtheta = vel_[2];

  double offset = 1.0;
  double ctheta = std::cos(-theta);
  double stheta = std::sin(-theta);
  double dx = vel_[0] * ctheta - vel_[1] * stheta;
  double dy = vel_[0] * stheta + vel_[1] * ctheta;

  Eigen::Vector3d local_vel;
  local_vel << dx, dy, dtheta;

  wheel_vel_ = jacobian_ * local_vel;
}

// Updates local velocity based on wheel change in position since last time
void OmniBase::updateOdometry(const Eigen::Vector3d& wheel_vel, double dt) { 
  // Get local velocities
  local_vel_ = jacobian_inv_ * wheel_vel;

  // Get global velocity:
  auto c = std::cos(global_pose_[2]);
  auto s = std::sin(global_pose_[2]);
  global_vel_[0] = c * local_vel_[0] - s * local_vel_[1];
  global_vel_[1] = s * local_vel_[0] + c * local_vel_[1];
  // Theta transforms directly
  global_vel_[2] = local_vel_[2];

  global_pose_ += global_vel_ * dt;
}

// Helper functions for initialization of jacobians as class members, b/c
// Eigen::Matrix3d objects don't have a constructor which can be used to set
// values.

Eigen::Matrix3d OmniBase::createJacobian() {
  double a = sqrt(3)/(2 * wheel_radius_);
  double b = 1.0 / wheel_radius_;
  double c = -base_radius_ / wheel_radius_;
  Eigen::Matrix3d j;
  j << -a, -b / 2.0, c,
       a, -b / 2.0, c,
       0.0, b, c;
  return j;
}

Eigen::Matrix3d OmniBase::createJacobianInv() {
  Eigen::Matrix3d j_inv;
  double a = wheel_radius_ / sqrt(3);
  double b = wheel_radius_ / 3.0;
  double c = -b / base_radius_;
  j_inv << -a, a, 0,
           -b, -b, 2.0 * b,
           c, c, c;
  return j_inv;
}

const Eigen::Matrix3d OmniBase::jacobian_ = createJacobian();

// Wheel velocities to local (x,y,theta)
const Eigen::Matrix3d OmniBase::jacobian_inv_ = createJacobianInv();

}

