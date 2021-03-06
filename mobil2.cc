#include "drake/automotive/mobil_planner2.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>
#include <fstream> ////////////////////// added ////////////////////////////////
// spec
#include <unistd.h>

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

// flags to help plan overtake on single lane
bool overtake_to_left = false;
bool overtake_to_right = false;
bool overtake = false;
const Lane* old_ego_lane = nullptr;
int car_id = -1;


template <typename T>
MobilPlanner2<T>::MobilPlanner2(const RoadGeometry& road, bool initial_with_s)
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
const systems::InputPortDescriptor<T>& MobilPlanner2<T>::ego_pose_input() const {
  return systems::System<T>::get_input_port(ego_pose_index_);
}

template <typename T>
const systems::InputPortDescriptor<T>& MobilPlanner2<T>::ego_velocity_input()
    const {
  return systems::System<T>::get_input_port(ego_velocity_index_);
}

template <typename T>
const systems::InputPortDescriptor<T>& MobilPlanner2<T>::ego_acceleration_input()
    const {
  return systems::System<T>::get_input_port(ego_acceleration_index_);
}

template <typename T>
const systems::InputPortDescriptor<T>& MobilPlanner2<T>::traffic_input() const {
  return systems::System<T>::get_input_port(traffic_index_);
}

template <typename T>
const systems::OutputPortDescriptor<T>& MobilPlanner2<T>::lane_output() const {
  return systems::System<T>::get_output_port(lane_index_);
}

template <typename T>
void MobilPlanner2<T>::DoCalcOutput(const systems::Context<T>& context,
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


void reset_overtake_flags()
{
  overtake_to_right = false;
  overtake_to_left = false;
  overtake = false;
  old_ego_lane = nullptr;
  car_id = -1;
}

template <typename T>
bool MobilPlanner2<T>::overtake_condition(const PoseVector<T>& ego_pose,
    const FrameVelocity<T>& ego_velocity,
    const PoseBundle<T>& traffic_poses, const BasicVector<T>& ego_accel_command,
    const IdmPlannerParameters<T>& idm_params,
    const MobilPlannerParameters<T>& mobil_params,
    LaneDirection* lane_direction, const RoadGeometry& road_n) const
{
  const RoadPosition traffic_position =
  pose_selector::CalcRoadPosition(road_n, traffic_poses.get_pose(car_id));
  const double& s_traffic = traffic_position.pos.s(); // other car s coor

  const RoadPosition& ego_position =
      pose_selector::CalcRoadPosition(road_n, ego_pose.get_isometry());
  const double& s_ego = ego_position.pos.s(); // ego car position s-coor

  if(s_ego > s_traffic) return true;

  return false;
}


template <typename T>
void MobilPlanner2<T>::ImplDoCalcLane(
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

    // TODO: testing nature of execution path related to this function/////////
    // std::ofstream myfile;
    // myfile.open ("~/Desktop/drake-distro/drake/automotive/z.txt");
    // myfile << "Writing this to a file.\n";
    // myfile.close();
    ///////////////////////////////////////////////////////////////////////////

    // if an old lane has been assigned, then ego has switched it's initial
    // lane and is done with half the overtake procedure
    // It then only needs to go back to it's initial lane
    if(old_ego_lane != nullptr)
    {
      if(ego_position.lane->id().id != old_ego_lane->id().id) //done switching
        if(overtake_to_right == true || overtake_to_left == true)
          overtake = true;
    }

    // Find the car directly ahead of the ego car
    // value checks ensure this computation is done only once
    // Runtime of find procedure is O(number of cars)
    if((overtake_to_left == true || overtake_to_right == true) && car_id == -1)
    {
      const double& s_ego = ego_position.pos.s(); // ego car position s-coor
      //double min = 10000; // arbitrary large value
      // finding car ahead (this work is done only once per overtake sequence)
      for (int i = 0; i < traffic_poses.get_num_poses(); ++i)
      {
         const RoadPosition traffic_position =
         pose_selector::CalcRoadPosition(road_, traffic_poses.get_pose(i));

        if (traffic_position.lane->id().id != ego_position.lane->id().id)
          continue; // since the lane can't have the car we're looking for

        const double& s_traffic = traffic_position.pos.s(); // other car
        //const double s_diff = s_traffic - s_ego;
        // if(s_diff > 0 && s_diff < min)
        // {
        //   car_id = i;
        //   min = s_diff;
        // }
          if( (traffic_position.lane->id().id == ego_position.lane->id().id) &&
              (s_ego != s_traffic) )
            car_id = i;
      }
      // // after for-loop, car_id has the car directly ahead of the ego
    }

    // decision making
    const T threshold = mobil_params.threshold();

    if(overtake == false)
    {
      if(incentives.first >= incentives.second)
      {
        if (incentives.first > threshold)  // true indicates lane change
        {
          lane = lanes.first;
          overtake_to_right = true; // went left; so to overtake, go right
          old_ego_lane = ego_position.lane; // save the old lane info
        }
        else
          lane = ego_position.lane;
      }
      else
      {
        if (incentives.second > threshold)  // true indicates lane change
        {
          lane = lanes.second;
          overtake_to_left = true; // went right; so to overtake, go left
          old_ego_lane = ego_position.lane; // save the old lane info
        }
        else
          lane = ego_position.lane;
      }
    }
    else // overtake flag is set, so let ego switch lane and then get ahead
    {
      if(car_id != -1)
      {
        if(overtake_to_right == true)
        {
            if(overtake_condition(ego_pose, ego_velocity, traffic_poses,
                                  ego_accel_command, idm_params, mobil_params,
                                  lane_direction, road_)
               == true) //go right when you can
            {
              lane = lanes.second;
              if(old_ego_lane->id().id == ego_position.lane->id().id)
                reset_overtake_flags(); // done overtaking
            }
            else // otherwise, stay in your current lane till you overtake
              lane = ego_position.lane;
        }
        else //if(overtake_to_left == true)
        {
            if(overtake_condition(ego_pose, ego_velocity, traffic_poses,
                                  ego_accel_command, idm_params, mobil_params,
                                  lane_direction, road_)
             == true) //go left when you can
            {
              lane = lanes.first;
              if(old_ego_lane->id().id == ego_position.lane->id().id)
                reset_overtake_flags(); // done overtaking
            }
            else // otherwise, stay in your current lane till you overtake
              lane = ego_position.lane;
        }
      } // if
    } // else outer
   } // end lane null ptr check

  *lane_direction = LaneDirection(lane, with_s_);

  // N.B. Assumes neighboring lanes are all confluent (i.e. with_s points in the
  // same direction).

}

template <typename T>
const std::pair<T, T> MobilPlanner2<T>::ComputeIncentives(
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
void MobilPlanner2<T>::ComputeIncentiveOutOfLane(
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
const T MobilPlanner2<T>::EvaluateIdm(
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
template class MobilPlanner2<double>;

}  // namespace automotive
}  // namespace drake
