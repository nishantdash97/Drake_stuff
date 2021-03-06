#include "drake/automotive/mobil_planner3.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

#include "drake/automotive/maliput/api/junction.h"
#include "drake/automotive/maliput/api/segment.h"
#include "drake/common/cond.h"
#include "drake/common/drake_assert.h"
#include "drake/common/symbolic_formula.h"
#include "drake/math/saturate.h"

namespace drake {

using maliput::api::GeoPosition;
using maliput::api::Lane;
using maliput::api::LanePosition;
using maliput::api::RoadGeometry;
using maliput::api::RoadPosition;
using math::saturate;
using automotive::pose_selector::RoadOdometry;
using systems::BasicVector;
using systems::rendering::FrameVelocity;
using systems::rendering::PoseBundle;
using systems::rendering::PoseVector;

namespace automotive {

namespace {

static constexpr int kIdmParamsIndex{0};
static constexpr int kMobilParamsIndex{1};
static constexpr double kDefaultLargeAccel{1e6};  // m/s^2

}  // namespace

template <typename T>
MobilPlanner3<T>::MobilPlanner3(const RoadGeometry& road, bool initial_with_s)
    : road_(road),
      with_s_(initial_with_s),
      ego_pose_index_{
          this->DeclareVectorInputPort(PoseVector<T>()).get_index()},
      ego_velocity_index_{
          this->DeclareVectorInputPort(FrameVelocity<T>()).get_index()},
      ego_acceleration_index_{
          this->DeclareVectorInputPort(BasicVector<T>(1)).get_index()},
      traffic_index_{this->DeclareAbstractInputPort().get_index()},
      lane_index_{this->DeclareAbstractOutputPort(
                          systems::Value<LaneDirection>(LaneDirection()))
                      .get_index()} {
  // Validate the provided RoadGeometry.
  DRAKE_DEMAND(road_.num_junctions() > 0);
  DRAKE_DEMAND(road_.junction(0)->num_segments() > 0);
  DRAKE_DEMAND(road_.junction(0)->segment(0)->num_lanes() > 0);
  this->DeclareNumericParameter(IdmPlannerParameters<T>());
  this->DeclareNumericParameter(MobilPlannerParameters<T>());
}

template <typename T>
const systems::InputPortDescriptor<T>& MobilPlanner3<T>::ego_pose_input() const {
  return systems::System<T>::get_input_port(ego_pose_index_);
}

template <typename T>
const systems::InputPortDescriptor<T>& MobilPlanner3<T>::ego_velocity_input()
    const {
  return systems::System<T>::get_input_port(ego_velocity_index_);
}

template <typename T>
const systems::InputPortDescriptor<T>& MobilPlanner3<T>::ego_acceleration_input()
    const {
  return systems::System<T>::get_input_port(ego_acceleration_index_);
}

template <typename T>
const systems::InputPortDescriptor<T>& MobilPlanner3<T>::traffic_input() const {
  return systems::System<T>::get_input_port(traffic_index_);
}

template <typename T>
const systems::OutputPortDescriptor<T>& MobilPlanner3<T>::lane_output() const {
  return systems::System<T>::get_output_port(lane_index_);
}

template <typename T>
void MobilPlanner3<T>::DoCalcOutput(const systems::Context<T>& context,
                                   systems::SystemOutput<T>* output) const {
  // Obtain the parameters.
  const IdmPlannerParameters<T>& idm_params =
      this->template GetNumericParameter<IdmPlannerParameters>(context,
                                                               kIdmParamsIndex);
  const MobilPlannerParameters<T>& mobil_params =
      this->template GetNumericParameter<MobilPlannerParameters>(
          context, kMobilParamsIndex);

  // Obtain the input/output data structures.
  const PoseVector<T>* const ego_pose =
      this->template EvalVectorInput<PoseVector>(context, ego_pose_index_);
  DRAKE_ASSERT(ego_pose != nullptr);

  const FrameVelocity<T>* const ego_velocity =
      this->template EvalVectorInput<FrameVelocity>(context,
                                                    ego_velocity_index_);
  DRAKE_ASSERT(ego_velocity != nullptr);

  const BasicVector<T>* const ego_accel_command =
      this->template EvalVectorInput<BasicVector>(context,
                                                  ego_acceleration_index_);
  DRAKE_ASSERT(ego_accel_command != nullptr);

  const PoseBundle<T>* const traffic_poses =
      this->template EvalInputValue<PoseBundle<T>>(context, traffic_index_);
  DRAKE_ASSERT(traffic_poses != nullptr);

  LaneDirection* lane_direction =
      &output->GetMutableData(lane_index_)
           ->template GetMutableValue<LaneDirection>();
  DRAKE_ASSERT(lane_direction != nullptr);

  ImplDoCalcLane(*ego_pose, *ego_velocity, *traffic_poses, *ego_accel_command,
                 idm_params, mobil_params, lane_direction);
}

template <typename T>
void MobilPlanner3<T>::ImplDoCalcLane(
    const PoseVector<T>& ego_pose, const FrameVelocity<T>& ego_velocity,
    const PoseBundle<T>& traffic_poses, const BasicVector<T>& ego_accel_command,
    const IdmPlannerParameters<T>& idm_params,
    const MobilPlannerParameters<T>& mobil_params,
    LaneDirection* lane_direction) const {
  DRAKE_DEMAND(idm_params.IsValid());
  DRAKE_DEMAND(mobil_params.IsValid());

  const RoadPosition& ego_position =
      pose_selector::CalcRoadPosition(road_, ego_pose.get_isometry());
  // Prepare a list of (possibly nullptr) Lanes to evaluate.
  std::pair<const Lane*, const Lane*> lanes = std::make_pair(
      ego_position.lane->to_left(), ego_position.lane->to_right());

  const Lane* lane = ego_position.lane;
  if (lanes.first != nullptr || lanes.second != nullptr) {
    const std::pair<T, T> incentives =
        ComputeIncentives(lanes, idm_params, mobil_params, ego_pose,
                          ego_velocity, traffic_poses, ego_accel_command[0]);
    // Switch to the lane with the highest incentive score greater than zero,
    // staying in the same lane if under the threshold.
    const T threshold = mobil_params.threshold();

    if (incentives.first >= incentives.second)
      lane = (incentives.first > threshold) ? lanes.first : ego_position.lane;
    else
      lane = (incentives.second > threshold) ? lanes.second : ego_position.lane;

    // try to switch to right lane
    if(true) lane = ego_position.lane;
  }
  *lane_direction = LaneDirection(lane, with_s_);
  // N.B. Assumes neighboring lanes are all confluent (i.e. with_s points in the
  // same direction).
}

template <typename T>
const std::pair<T, T> MobilPlanner3<T>::ComputeIncentives(
    const std::pair<const Lane*, const Lane*> lanes,
    const IdmPlannerParameters<T>& idm_params,
    const MobilPlannerParameters<T>& mobil_params,
    const PoseVector<T>& ego_pose, const FrameVelocity<T>& ego_velocity,
    const PoseBundle<T>& traffic_poses, const T& ego_acceleration) const {
  // Initially disincentivize both neighboring lane options.  N.B. The first and
  // second elements correspond to the left and right lanes, respectively.
  std::pair<T, T> incentives(-kDefaultLargeAccel, -kDefaultLargeAccel);

  const RoadPosition& ego_position =
      pose_selector::CalcRoadPosition(road_, ego_pose.get_isometry());
  DRAKE_DEMAND(ego_position.lane != nullptr);
  RoadOdometry<T> leading_odometry{};
  RoadOdometry<T> trailing_odometry{};
  std::tie(leading_odometry, trailing_odometry) =
      pose_selector::FindClosestPair(road_, ego_pose, traffic_poses);

  // Current acceleration of the ego car.
  const RoadOdometry<T>& ego_odometry =
      RoadOdometry<T>(ego_position, ego_velocity);
  // Current acceleration of the trailing car.
  const T trailing_this_old_accel =
      EvaluateIdm(idm_params, trailing_odometry, ego_odometry);
  // New acceleration of the trailing car if the ego were to change lanes.
  const T trailing_this_new_accel =
      EvaluateIdm(idm_params, trailing_odometry, leading_odometry);
  // Acceleration delta of the trailing car in the ego car's current lane.
  const T trailing_delta_accel_this =
      trailing_this_new_accel - trailing_this_old_accel;
  // Compute the incentive for the left lane.
  if (lanes.first != nullptr) {
    const OdometryPair& odometries = pose_selector::FindClosestPair(
        road_, ego_pose, traffic_poses, lanes.first);
    ComputeIncentiveOutOfLane(idm_params, mobil_params, odometries,
                              ego_odometry, ego_acceleration,
                              trailing_delta_accel_this, &incentives.first);
  }
  // Compute the incentive for the right lane.
  if (lanes.second != nullptr) {
    const OdometryPair& odometries = pose_selector::FindClosestPair(
        road_, ego_pose, traffic_poses, lanes.second);
    ComputeIncentiveOutOfLane(idm_params, mobil_params, odometries,
                              ego_odometry, ego_acceleration,
                              trailing_delta_accel_this, &incentives.second);
  }
  return incentives;
}

template <typename T>
void MobilPlanner3<T>::ComputeIncentiveOutOfLane(
    const IdmPlannerParameters<T>& idm_params,
    const MobilPlannerParameters<T>& mobil_params,
    const OdometryPair& odometries, const RoadOdometry<T>& ego_odometry,
    const T& ego_old_accel, const T& trailing_delta_accel_this,
    T* incentive) const {
  RoadOdometry<T> leading_odometry{};
  RoadOdometry<T> trailing_odometry{};
  std::tie(leading_odometry, trailing_odometry) = odometries;
  // Acceleration of the ego car if it were to move to the neighboring lane.
  const T ego_new_accel =
      EvaluateIdm(idm_params, ego_odometry, leading_odometry);
  // Original acceleration of the trailing car in the neighboring lane.
  const T trailing_old_accel =
      EvaluateIdm(idm_params, trailing_odometry, leading_odometry);
  // Acceleration of the trailing car in the neighboring lane if the ego moves
  // here.
  const T trailing_new_accel =
      EvaluateIdm(idm_params, trailing_odometry, ego_odometry);
  // Acceleration delta of the trailing car in the neighboring (other) lane.
  const T trailing_delta_accel_other = trailing_new_accel - trailing_old_accel;
  const T ego_delta_accel = ego_new_accel - ego_old_accel;

  // Do not switch to this lane if it discomforts the trailing car too much.
  if (trailing_new_accel < -mobil_params.max_deceleration()) return;

  // Compute the incentive as a weighted sum of the net accelerations for
  // the ego and each immediate neighbor.
  *incentive = ego_delta_accel +
               mobil_params.p() *
                   (trailing_delta_accel_other + trailing_delta_accel_this);
}

template <typename T>
const T MobilPlanner3<T>::EvaluateIdm(
    const IdmPlannerParameters<T>& idm_params,
    const RoadOdometry<T>& ego_odometry,
    const RoadOdometry<T>& lead_car_odometry) const {
  const T& s_ego = ego_odometry.pos.s();
  const T& s_dot_ego = pose_selector::GetSVelocity(ego_odometry);
  const T& s_lead = lead_car_odometry.pos.s();
  const T& s_dot_lead = pose_selector::GetSVelocity(lead_car_odometry);

  const T delta = s_lead - s_ego;
  // Saturate the net_distance at distance_lower_bound away from the ego car to
  // prevent the IDM equation from producing near-singular solutions.
  // TODO(jadecastro): Move this to IdmPlanner::Evaluate().
  const T net_distance =
      cond(delta >= T(0.), std::max(delta - idm_params.bloat_diameter(),
                                    idm_params.distance_lower_limit()),
           std::min(delta + idm_params.bloat_diameter(),
                    -idm_params.distance_lower_limit()));
  DRAKE_DEMAND(std::abs(net_distance) >= idm_params.distance_lower_limit());
  const T closing_velocity = s_dot_ego - s_dot_lead;

  return IdmPlanner<T>::Evaluate(idm_params, s_dot_ego, net_distance,
                                 closing_velocity);
}

// These instantiations must match the API documentation in mobil_planner.h.
template class MobilPlanner3<double>;

}  // namespace automotive
}  // namespace drake
