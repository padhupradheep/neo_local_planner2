/*********************************************************************
 * Software License Agreement (BSD License)
 *
 *  Copyright (c) 2016, Neobotix GmbH
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *   * Neither the name of the Neobotix nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 *  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 *  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 *  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *********************************************************************/

#include "../include/NeoLocalPlanner.h"

#include <tf2/utils.h>
#include "nav2_util/node_utils.hpp"
#include <tf2_sensor_msgs/tf2_sensor_msgs.h>
#include <tf2_geometry_msgs/tf2_geometry_msgs.h>
#include <vector>
#include "nav2_util/line_iterator.hpp"
#include "nav2_core/goal_checker.hpp"
#include "pluginlib/class_list_macros.hpp"
#include <algorithm>
#include <nav_2d_utils/tf_help.hpp>
#include <tf2_eigen/tf2_eigen.h>


namespace neo_local_planner {

tf2::Quaternion createQuaternionFromYaw(double yaw)
{
	tf2::Quaternion q;
	q.setRPY(0, 0, yaw);
	return q;
}

std::vector<tf2::Transform>::const_iterator find_closest_point(	std::vector<tf2::Transform>::const_iterator begin,
															std::vector<tf2::Transform>::const_iterator end,
															const tf2::Vector3& pos,
															double* actual_dist = 0)
{
	auto iter_short = begin;
	double dist_short = std::numeric_limits<double>::infinity();

	for(auto iter = iter_short; iter != end; ++iter)
	{
		const double dist = (iter->getOrigin() - pos).length();
		if(dist < dist_short)
		{
			dist_short = dist;
			iter_short = iter;
		}
	}
	if(actual_dist) {
		*actual_dist = dist_short;
	}
	return iter_short;
}

std::vector<tf2::Transform>::const_iterator move_along_path(	std::vector<tf2::Transform>::const_iterator begin,
														std::vector<tf2::Transform>::const_iterator end,
														const double dist, double* actual_dist = 0)
{
	auto iter = begin;
	auto iter_prev = iter;
	double dist_left = dist;

	while(iter != end)
	{
		const double dist = (iter->getOrigin() - iter_prev->getOrigin()).length();
		dist_left -= dist;
		if(dist_left <= 0) {
			break;
		}
		iter_prev = iter;
		iter++;
	}
	if(iter == end) {
		iter = iter_prev;		// targeting final pose
	}
	if(actual_dist) {
		*actual_dist = dist - dist_left;
	}
	return iter;
}

std::vector<std::pair <int,int> > get_line_cells(
								nav2_costmap_2d::Costmap2D* cost_map,
								const tf2::Vector3& world_pos_0,
								const tf2::Vector3& world_pos_1)
{
	int coords[2][2] = {};
	cost_map->worldToMapEnforceBounds(world_pos_0.x(), world_pos_0.y(), coords[0][0], coords[0][1]);
	cost_map->worldToMapEnforceBounds(world_pos_1.x(), world_pos_1.y(), coords[1][0], coords[1][1]);

	// Creating a vector for storing the value of the cells
	
	std::vector< std::pair <int,int> > cells;

	// Line iterator for determining the cells between two points
 	for (nav2_util::LineIterator line(coords[0][0], coords[0][1], coords[1][0], coords[1][1]); line.isValid(); line.advance())
 	{
        cells.push_back( std::make_pair(line.getX(),line.getY()) );
    }
    // Mandatory reversal
    // std::reverse( cells.begin(), cells.end() );

	return cells;
}

double get_cost(nav2_costmap_2d::Costmap2D* cost_map_, const tf2::Vector3& world_pos)
{


	int coords[2] = {};
	cost_map_->worldToMapEnforceBounds(world_pos.x(), world_pos.y(), coords[0], coords[1]);

	return cost_map_->getCost(coords[0], coords[1]) / 255.;

}

double compute_avg_line_cost(	nav2_costmap_2d::Costmap2D* cost_map_,
								const tf2::Vector3& world_pos_0,
								const tf2::Vector3& world_pos_1)
{
	const std::vector< std::pair<int, int> > cells = get_line_cells(cost_map_, world_pos_0, world_pos_1);

	double avg_cost = 0;
	for(auto cell : cells) {

		avg_cost += (double)cost_map_->getCost(cell.first, cell.second) / 255.;
	}

	return avg_cost / cells.size();
}

double compute_max_line_cost(	nav2_costmap_2d::Costmap2D* cost_map_,
								const tf2::Vector3& world_pos_0,
								const tf2::Vector3& world_pos_1)
{
	const std::vector< std::pair<int, int> > cells = get_line_cells(cost_map_, world_pos_0, world_pos_1);

	int max_cost = 0;
	for(auto cell : cells) {
		max_cost = std::max(max_cost, int(cost_map_->getCost(cell.first, cell.second)));
	}
	return max_cost / 255.;
}

geometry_msgs::msg::TwistStamped NeoLocalPlanner::computeVelocityCommands(
  const geometry_msgs::msg::PoseStamped & position,
  const geometry_msgs::msg::Twist & speed)
{
	boost::mutex::scoped_lock lock(m_odometry_mutex);
	geometry_msgs::msg::Twist cmd_vel;

	if(m_global_plan.poses.empty())
	{
		// ROS_WARN_NAMED("NeoLocalPlanner", "Global plan is empty!");
		// return false;
	}

	// compute delta time
	const rclcpp::Time time_now = rclcpp::Clock().now();
	const double dt = fmax(fmin((time_now - m_last_time).seconds(), 0.1), 0);

	// get latest global to local transform (map to odom)
	tf2::Stamped<tf2::Transform> global_to_local;
	try {
		geometry_msgs::msg::TransformStamped msg = tf_->lookupTransform(m_local_frame, m_global_frame, tf2::TimePointZero);
		tf2::fromMsg(msg, global_to_local);
	} catch(...) {
		// ROS_WARN_NAMED("NeoLocalPlanner", "lookupTransform(m_local_frame, m_global_frame) failed");
		// return false;
	std::cout<<"lookupTransform(m_local_frame, m_global_frame) failed"<<std::endl;

	}

	// transform plan to local frame (odom)
	std::vector<tf2::Transform> local_plan;
	for(const auto& pose : m_global_plan.poses)
	{
		tf2::Transform pose_;
		tf2::fromMsg(pose.pose, pose_);
		local_plan.push_back(global_to_local * pose_);
	}

	// get latest local pose
	tf2::Transform local_pose;
	tf2::fromMsg(position.pose, local_pose);

	const double start_yaw = tf2::getYaw(local_pose.getRotation());
	const double start_vel_x = speed.linear.x;
	const double start_vel_y = speed.linear.y;
	const double start_yawrate = speed.angular.z;

	// calc dynamic lookahead distances
	const double lookahead_dist = m_lookahead_dist + fmax(start_vel_x, 0) * lookahead_time;
	const double cost_y_lookahead_dist = m_cost_y_lookahead_dist + fmax(start_vel_x, 0) * cost_y_lookahead_time;

	// predict future pose (using second order midpoint method)
	tf2::Vector3 actual_pos;
	double actual_yaw = 0;
	{
		const double midpoint_yaw = start_yaw + start_yawrate * lookahead_time / 2;
		actual_pos = local_pose.getOrigin() + tf2::Matrix3x3(createQuaternionFromYaw(midpoint_yaw))
												* tf2::Vector3(start_vel_x, start_vel_y, 0) * lookahead_time;
		actual_yaw = start_yaw + start_yawrate * lookahead_time;
	}


	const tf2::Transform actual_pose = tf2::Transform(createQuaternionFromYaw(actual_yaw), actual_pos);


	// compute cost gradients
	const double delta_x = 0.3;
	const double delta_y = 0.2;
	const double delta_yaw = 0.1;


	const double center_cost = get_cost(costmap_, actual_pos);
	const double delta_cost_x = (
		compute_avg_line_cost(costmap_, actual_pos, actual_pose * tf2::Vector3(delta_x, 0, 0)) -
		compute_avg_line_cost(costmap_, actual_pos, actual_pose * tf2::Vector3(-delta_x, 0, 0)))
		/ delta_x;

	const double delta_cost_y = (
		compute_avg_line_cost(costmap_, actual_pos, actual_pose * tf2::Vector3(cost_y_lookahead_dist, delta_y, 0)) -
		compute_avg_line_cost(costmap_, actual_pos, actual_pose * tf2::Vector3(cost_y_lookahead_dist, -delta_y, 0)))
		/ delta_y;

	const double delta_cost_yaw = (
		(
			compute_avg_line_cost(costmap_,	actual_pose * (tf2::Matrix3x3(createQuaternionFromYaw(delta_yaw)) * tf2::Vector3(delta_x, 0, 0)),
												actual_pose * (tf2::Matrix3x3(createQuaternionFromYaw(delta_yaw)) * tf2::Vector3(-delta_x, 0, 0)))
		) - (
			compute_avg_line_cost(costmap_,	actual_pose * (tf2::Matrix3x3(createQuaternionFromYaw(-delta_yaw)) * tf2::Vector3(delta_x, 0, 0)),
												actual_pose * (tf2::Matrix3x3(createQuaternionFromYaw(-delta_yaw)) * tf2::Vector3(-delta_x, 0, 0)))
		)) / (2 * delta_yaw);

	// fill local plan later
	nav_msgs::msg::Path local_path;
	local_path.header.frame_id = m_local_frame;
	local_path.header.stamp = m_odometry->header.stamp;

	// compute obstacle distance
	bool have_obstacle = false;
	double obstacle_dist = 0;
	double obstacle_cost = 0;
	{
		const double delta_move = 0.05;
		const double delta_time = start_vel_x > trans_stopped_vel ? (delta_move / start_vel_x) : 0;

		tf2::Transform pose = actual_pose;
		tf2::Transform last_pose = pose;

		while(obstacle_dist < 10)
		{
			const double cost = compute_max_line_cost(costmap_, last_pose.getOrigin(), pose.getOrigin());

			bool is_contained = false;
			{
				unsigned int dummy[2] = {};
				is_contained = costmap_->worldToMap(pose.getOrigin().x(), pose.getOrigin().y(), dummy[0], dummy[1]);
			}
			have_obstacle = cost >= max_cost;
			obstacle_cost = fmax(obstacle_cost, cost);

			{
				geometry_msgs::msg::PoseStamped tmp;
				auto tmp1 = tf2::toMsg(pose);
				tmp.header = position.header;
				tmp.pose.position.x = tmp1.translation.x;
				tmp.pose.position.y = tmp1.translation.y;
				tmp.pose.position.z = tmp1.translation.z;
				tmp.pose.orientation.x = tmp1.rotation.x;
				tmp.pose.orientation.y = tmp1.rotation.y;
				tmp.pose.orientation.z = tmp1.rotation.z;
				tmp.pose.orientation.w = tmp1.rotation.w;
				local_path.poses.push_back(tmp);
			}
			if(!is_contained || have_obstacle) {
				break;
			}

			last_pose = pose;
			pose = tf2::Transform(createQuaternionFromYaw(tf2::getYaw(pose.getRotation()) + start_yawrate * delta_time),
							pose * tf2::Vector3(delta_move, 0, 0));

			obstacle_dist += delta_move;
		}
	}
	m_local_plan_pub->publish(local_path);

	obstacle_dist -= min_stop_dist;

	// publish local plan

	// compute situational max velocities
	const double max_trans_vel = fmax(max_vel_trans * (max_cost - center_cost) / max_cost, min_vel_trans);
	const double max_rot_vel = fmax(max_vel_theta * (max_cost - center_cost) / max_cost, min_vel_theta);

	// find closest point on path to future position
	auto iter_target = find_closest_point(local_plan.cbegin(), local_plan.cend(), actual_pos);

	// check if goal target
	bool is_goal_target = false;
	{
		// check if goal is within reach
		auto iter_next = move_along_path(iter_target, local_plan.cend(), max_goal_dist);
		is_goal_target = iter_next + 1 >= local_plan.cend();

		if(is_goal_target)
		{
			// go straight to goal
			iter_target = iter_next;
		}
	}
	// figure out target orientation
	double target_yaw = 0;

	if(is_goal_target)
	{
		// take goal orientation
		target_yaw = tf2::getYaw(iter_target->getRotation());
	}
	else
	{
		// compute path based target orientation
		auto iter_next = move_along_path(iter_target, local_plan.cend(), lookahead_dist);
		target_yaw = ::atan2(	iter_next->getOrigin().y() - iter_target->getOrigin().y(),
								iter_next->getOrigin().x() - iter_target->getOrigin().x());
	}

	// get target position
	const tf2::Vector3 target_pos = iter_target->getOrigin();

	// compute errors
	const double goal_dist = (local_plan.back().getOrigin() - actual_pos).length();
	const double yaw_error = angles::shortest_angular_distance(actual_yaw, target_yaw);
	const tf2::Vector3 pos_error = tf2::Transform(createQuaternionFromYaw(actual_yaw), actual_pos).inverse() * target_pos;

	// compute control values
	bool is_emergency_brake = false;
	double control_vel_x = 0;
	double control_vel_y = 0;
	double control_yawrate = 0;

	if(is_goal_target)
	{
		// use term for final stopping position
		control_vel_x = pos_error.x() * pos_x_gain;
	}
	else
	{
		control_vel_x = max_trans_vel;

		// wait to start moving
		if(m_state != state_t::STATE_TRANSLATING && fabs(yaw_error) > start_yaw_error)
		{
			control_vel_x = 0;
		}

		// limit curve velocity
		{
			const double max_vel_x = max_curve_vel * (lookahead_dist / fabs(yaw_error));
			control_vel_x = fmin(control_vel_x, max_vel_x);
		}

		// limit velocity when approaching goal position
		if(start_vel_x > 0)
		{
			const double stop_accel = 0.8 * acc_lim_x;
			const double stop_time = sqrt(2 * fmax(goal_dist, 0) / stop_accel);
			const double max_vel_x = fmax(stop_accel * stop_time, min_vel_trans);

			control_vel_x = fmin(control_vel_x, max_vel_x);
		}

		// limit velocity when approaching an obstacle
		if(have_obstacle && start_vel_x > 0)
		{
			const double stop_accel = 0.9 * acc_lim_x;
			const double stop_time = sqrt(2 * fmax(obstacle_dist, 0) / stop_accel);
			const double max_vel_x = stop_accel * stop_time;

			// check if it's much lower than current velocity
			if(max_vel_x < 0.5 * start_vel_x) {
				is_emergency_brake = true;
			}

			control_vel_x = fmin(control_vel_x, max_vel_x);
		}

		// stop before hitting obstacle
		if(have_obstacle && obstacle_dist <= 0)
		{
			control_vel_x = 0;
		}

		// only allow forward velocity in this branch
		control_vel_x = fmax(control_vel_x, 0);
	}
	// limit backing up
	if(is_goal_target && max_backup_dist > 0
		&& pos_error.x() < (m_state == state_t::STATE_TURNING ? 0 : -1 * max_backup_dist))
	{
		control_vel_x = 0;
		m_state = state_t::STATE_TURNING;
	}
	else if(m_state == state_t::STATE_TURNING)
	{
		m_state = state_t::STATE_IDLE;
	}

	if(differential_drive)
	{
		if(fabs(start_vel_x) > (m_state == state_t::STATE_TRANSLATING ?
								trans_stopped_vel : 2 * trans_stopped_vel))
		{
			// we are translating, use term for lane keeping
			control_yawrate = pos_error.y() / start_vel_x * pos_y_yaw_gain;

			if(!is_goal_target)
			{
				// additional term for lane keeping
				control_yawrate += yaw_error * yaw_gain;

				// add cost terms
				control_yawrate -= delta_cost_y / start_vel_x * cost_y_yaw_gain;
				control_yawrate -= delta_cost_yaw * cost_yaw_gain;
			}

			m_state = state_t::STATE_TRANSLATING;
		}
		else if(m_state == state_t::STATE_TURNING)
		{
			// continue on current yawrate
			control_yawrate = (start_yawrate > 0 ? 1 : -1) * max_rot_vel;
		}
		else if(is_goal_target
				&& (m_state == state_t::STATE_ADJUSTING || fabs(yaw_error) < M_PI / 6)
				&& fabs(pos_error.y()) > (m_state == state_t::STATE_ADJUSTING ?
					0.25 * xy_goal_tolerance : 0.5 * xy_goal_tolerance))
		{
			// we are not translating, but we have too large y error
			control_yawrate = (pos_error.y() > 0 ? 1 : -1) * max_rot_vel;

			m_state = state_t::STATE_ADJUSTING;
		}
		else
		{
			// use term for static target orientation
			control_yawrate = yaw_error * static_yaw_gain;

			m_state = state_t::STATE_ROTATING;
		}
	}
	else
	{
		// simply correct y with holonomic drive
		control_vel_y = pos_error.y() * pos_y_gain;

		if(m_state == state_t::STATE_TURNING)
		{
			// continue on current yawrate
			control_yawrate = (start_yawrate > 0 ? 1 : -1) * max_rot_vel;
		}
		else
		{
			// use term for static target orientation
			control_yawrate = yaw_error * static_yaw_gain;

			if(fabs(start_vel_x) > trans_stopped_vel) {
				m_state = state_t::STATE_TRANSLATING;
			} else {
				m_state = state_t::STATE_ROTATING;
			}
		}

		// apply x cost term only when rotating
		if(m_state == state_t::STATE_ROTATING && fabs(yaw_error) > M_PI / 6)
		{
			control_vel_x -= delta_cost_x * cost_x_gain;
		}

		// apply y cost term when not approaching goal or if we are rotating
		if(!is_goal_target || (m_state == state_t::STATE_ROTATING && fabs(yaw_error) > M_PI / 6))
		{
			control_vel_y -= delta_cost_y * cost_y_gain;
		}

		// apply yaw cost term when not approaching goal
		if(!is_goal_target)
		{
			control_yawrate -= delta_cost_yaw * cost_yaw_gain;
		}
	}
	// check if we are stuck
	if(have_obstacle && obstacle_dist <= 0 && delta_cost_x > 0
		&& m_state == state_t::STATE_ROTATING && fabs(yaw_error) < M_PI / 6)
	{
		// we are stuck
		m_state = state_t::STATE_STUCK;

		std::cout<<"We are stuck"<<std::endl;
		// ROS_WARN_NAMED("NeoLocalPlanner", "We are stuck: yaw_error=%f, obstacle_dist=%f, obstacle_cost=%f, delta_cost_x=%f",
						// yaw_error, obstacle_dist, obstacle_cost, delta_cost_x);
		// return false;
		geometry_msgs::msg::TwistStamped cmd_vel_stuck;
  	cmd_vel_stuck.header.stamp = clock_->now();
		cmd_vel_stuck.header.frame_id = position.header.frame_id;
  	cmd_vel_stuck.twist.linear.x = 0;
  	cmd_vel_stuck.twist.angular.z = 0;

  return cmd_vel_stuck;
	}

	// logic check
	is_emergency_brake = is_emergency_brake && control_vel_x >= 0;

	// apply low pass filter
	control_vel_x = control_vel_x * low_pass_gain + m_last_control_values[0] * (1 - low_pass_gain);
	control_vel_y = control_vel_y * low_pass_gain + m_last_control_values[1] * (1 - low_pass_gain);
	control_yawrate = control_yawrate * low_pass_gain + m_last_control_values[2] * (1 - low_pass_gain);

	// apply acceleration limits
	control_vel_x = fmax(fmin(control_vel_x, m_last_cmd_vel.linear.x + acc_lim_x * dt),
							m_last_cmd_vel.linear.x - (is_emergency_brake ? emergency_acc_lim_x : acc_lim_x) * dt);
	control_vel_y = fmax(fmin(control_vel_y, m_last_cmd_vel.linear.y + acc_lim_y * dt),
								m_last_cmd_vel.linear.y - acc_lim_y * dt);

	control_yawrate = fmax(fmin(control_yawrate, m_last_cmd_vel.angular.z + acc_lim_theta * dt),
									m_last_cmd_vel.angular.z - acc_lim_theta * dt);

	// constrain velocity after goal reached
	if(constrain_final && m_is_goal_reached)
	{
		tf2::Vector3 direction(m_last_control_values[0], m_last_control_values[1], m_last_control_values[2]);
		if(direction.length() != 0)
		{
			direction.normalize();
			const double dist = direction.dot(tf2::Vector3(control_vel_x, control_vel_y, control_yawrate));
			const auto control = direction * dist;
			control_vel_x = control[0];
			control_vel_y = control[1];
			control_yawrate = control[2];
		}
	}

	// fill return data

	cmd_vel.linear.x = fmin(fmax(control_vel_x, min_vel_x), max_vel_x);
	cmd_vel.linear.y = fmin(fmax(control_vel_y, min_vel_y), max_vel_y);
	cmd_vel.linear.z = 0;
	cmd_vel.angular.x = 0;
	cmd_vel.angular.y = 0;
	cmd_vel.angular.z = fmin(fmax(control_yawrate, -max_vel_theta), max_vel_theta);

	if(m_update_counter % 20 == 0) {
		// ROS_INFO_NAMED("NeoLocalPlanner", "dt=%f, pos_error=(%f, %f), yaw_error=%f, cost=%f, obstacle_dist=%f, obstacle_cost=%f, delta_cost=(%f, %f, %f), state=%d, cmd_vel=(%f, %f), cmd_yawrate=%f",
						// dt, pos_error.x(), pos_error.y(), yaw_error, center_cost, obstacle_dist, obstacle_cost, delta_cost_x, delta_cost_y, delta_cost_yaw, m_state, control_vel_x, control_vel_y, control_yawrate);
	}
	m_last_time = time_now;
	m_last_control_values[0] = control_vel_x;
	m_last_control_values[1] = control_vel_y;
	m_last_control_values[2] = control_yawrate;
	m_last_cmd_vel = cmd_vel;

	m_update_counter++;
	geometry_msgs::msg::TwistStamped cmd_vel_final;
  	cmd_vel_final.header.stamp = clock_->now();
	cmd_vel_final.header.frame_id = position.header.frame_id;
  	cmd_vel_final.twist.linear = cmd_vel.linear;
  	cmd_vel_final.twist.angular = cmd_vel.angular;

  return cmd_vel_final;
}

void NeoLocalPlanner::cleanup()
{
	m_local_plan_pub.reset();
}

void NeoLocalPlanner::activate()
{
	m_local_plan_pub->on_activate();
}

void NeoLocalPlanner::deactivate()
{
	m_local_plan_pub->on_deactivate();
}

bool NeoLocalPlanner::isGoalReached()
{
	boost::mutex::scoped_lock lock(m_odometry_mutex);

	nav2_core::GoalChecker *goal_checker; 

	geometry_msgs::msg::Pose current_pose; 
	geometry_msgs::msg::Twist current_twist;

	if(!m_odometry)
	{
		std::cout<< "Waiting for Odometry" << std::endl;
		return false;
	}
	if(m_global_plan.poses.empty())
	{
		std::cout<< "Global Plan is empty" << std::endl;
		return true;
	}

	tf2::Stamped<tf2::Transform> global_to_local;
	try {
		auto msg = tf_->lookupTransform(m_local_frame, m_global_frame,  tf2::TimePointZero);
		tf2::fromMsg(msg, global_to_local);
	} catch(...) {
		std::cout<< "lookupTransform Failed!" << std::endl;
		return false;
	}

	tf2::Stamped<tf2::Transform> goal_pose_global;
	tf2::fromMsg(m_global_plan.poses.back(), goal_pose_global);
	// geometry_msgs::msg::Transform goal_pose_global_check;
	geometry_msgs::msg::Pose goal_pose_global_check1;

	auto goal_pose_global_check = tf2::toMsg(goal_pose_global);
	goal_pose_global_check1.position.x = goal_pose_global_check.transform.translation.x;
	goal_pose_global_check1.position.y = goal_pose_global_check.transform.translation.y;
	goal_pose_global_check1.position.z = goal_pose_global_check.transform.translation.z;
	goal_pose_global_check1.orientation.x = goal_pose_global_check.transform.rotation.x;
	goal_pose_global_check1.orientation.y = goal_pose_global_check.transform.rotation.y;
	goal_pose_global_check1.orientation.z = goal_pose_global_check.transform.rotation.z;
	goal_pose_global_check1.orientation.w = goal_pose_global_check.transform.rotation.w;
	const auto goal_pose_local = global_to_local * goal_pose_global;

	// Checking is goal_reached
	current_pose.position = m_odometry->pose.pose.position;
	current_pose.orientation = m_odometry->pose.pose.orientation;
	current_twist.linear = m_odometry->twist.twist.linear;
	current_twist.angular = m_odometry->twist.twist.angular;

	const bool is_reached = goal_checker->isGoalReached(goal_pose_global_check1, current_pose, current_twist);

	const double xy_error = ::hypot(m_odometry->pose.pose.position.x - goal_pose_local.getOrigin().x(),
									m_odometry->pose.pose.position.y - goal_pose_local.getOrigin().y());
	
	const double yaw_error = fabs(angles::shortest_angular_distance(tf2::getYaw(m_odometry->pose.pose.orientation),
																	tf2::getYaw(goal_pose_local.getRotation())));

	if(!m_is_goal_reached)
	{
		if(is_reached) {
			std::cout<<"Goal reached: xy_error=" << xy_error << " [m], yaw_error=" << yaw_error << " [rad]"<<std::endl;
		}
		m_first_goal_reached_time =  rclcpp::Clock().now();
	}
	m_is_goal_reached = is_reached;
	return is_reached && ( rclcpp::Clock().now() - m_first_goal_reached_time).seconds() >= goal_tune_time;
}

void NeoLocalPlanner::setPlan(const nav_msgs::msg::Path & plan)
{
	m_global_plan = plan;
}

void NeoLocalPlanner::configure(const rclcpp_lifecycle::LifecycleNode::SharedPtr & parent,  std::string name, const std::shared_ptr<tf2_ros::Buffer> & tf,  const std::shared_ptr<nav2_costmap_2d::Costmap2DROS> & costmap_ros)
{
	plugin_name_ = name;
	clock_ = parent->get_clock();
	nav2_util::declare_parameter_if_not_declared(parent,plugin_name_ + ".acc_lim_x",rclcpp::ParameterValue(0.2));
	nav2_util::declare_parameter_if_not_declared(parent,plugin_name_ + ".acc_lim_y",rclcpp::ParameterValue(0.2));
	nav2_util::declare_parameter_if_not_declared(parent,plugin_name_ + ".acc_lim_theta",rclcpp::ParameterValue(0.2));
	nav2_util::declare_parameter_if_not_declared(parent,plugin_name_ + ".acc_limit_trans",rclcpp::ParameterValue(0.2));
	nav2_util::declare_parameter_if_not_declared(parent,plugin_name_ + ".min_vel_x",rclcpp::ParameterValue(0.2));
	nav2_util::declare_parameter_if_not_declared(parent,plugin_name_ + ".max_vel_x",rclcpp::ParameterValue(0.2));
	nav2_util::declare_parameter_if_not_declared(parent,plugin_name_ + ".min_vel_y",rclcpp::ParameterValue(0.2));
	nav2_util::declare_parameter_if_not_declared(parent,plugin_name_ + ".max_vel_y",rclcpp::ParameterValue(0.2));
	nav2_util::declare_parameter_if_not_declared(parent,plugin_name_ + ".min_rot_vel",rclcpp::ParameterValue(0.2));
	nav2_util::declare_parameter_if_not_declared(parent,plugin_name_ + ".max_rot_vel",rclcpp::ParameterValue(0.2));
	nav2_util::declare_parameter_if_not_declared(parent,plugin_name_ + ".min_vel_trans",rclcpp::ParameterValue(0.2));
	nav2_util::declare_parameter_if_not_declared(parent,plugin_name_ + ".max_vel_trans",rclcpp::ParameterValue(0.2));
	nav2_util::declare_parameter_if_not_declared(parent,plugin_name_ + ".rot_stopped_vel", rclcpp::ParameterValue(0.2));
	nav2_util::declare_parameter_if_not_declared(parent,plugin_name_ + ".trans_stopped_vel",rclcpp::ParameterValue(0.2));
	nav2_util::declare_parameter_if_not_declared(parent,plugin_name_ + ".yaw_goal_tolerance",rclcpp::ParameterValue(0.2));
	nav2_util::declare_parameter_if_not_declared(parent,plugin_name_ + ".xy_goal_tolerance",rclcpp::ParameterValue(0.2));

	nav2_util::declare_parameter_if_not_declared(parent,plugin_name_ + ".goal_tune_time",rclcpp::ParameterValue(0.2));
	nav2_util::declare_parameter_if_not_declared(parent,plugin_name_ + ".lookahead_time",rclcpp::ParameterValue(0.2));
	nav2_util::declare_parameter_if_not_declared(parent,plugin_name_ + ".lookahead_dist",rclcpp::ParameterValue(0.2));
	nav2_util::declare_parameter_if_not_declared(parent,plugin_name_ + ".start_yaw_error",rclcpp::ParameterValue(0.2));
	nav2_util::declare_parameter_if_not_declared(parent,plugin_name_ + ".pos_x_gain",rclcpp::ParameterValue(0.2));
	nav2_util::declare_parameter_if_not_declared(parent,plugin_name_ + ".pos_y_gain",rclcpp::ParameterValue(0.2));
	nav2_util::declare_parameter_if_not_declared(parent,plugin_name_ + ".pos_y_yaw_gain",rclcpp::ParameterValue(0.2));
	nav2_util::declare_parameter_if_not_declared(parent,plugin_name_ + ".yaw_gain",rclcpp::ParameterValue(0.2));
	nav2_util::declare_parameter_if_not_declared(parent,plugin_name_ + ".static_yaw_gain",rclcpp::ParameterValue(0.2));
	nav2_util::declare_parameter_if_not_declared(parent,plugin_name_ + ".cost_x_gain",rclcpp::ParameterValue(0.2));
	nav2_util::declare_parameter_if_not_declared(parent,plugin_name_ + ".cost_y_gain",rclcpp::ParameterValue(0.2));
	nav2_util::declare_parameter_if_not_declared(parent,plugin_name_ + ".cost_y_yaw_gain",rclcpp::ParameterValue(0.2));
	nav2_util::declare_parameter_if_not_declared(parent,plugin_name_ + ".cost_y_lookahead_dist", rclcpp::ParameterValue(0.2));
	nav2_util::declare_parameter_if_not_declared(parent,plugin_name_ + ".cost_y_lookahead_time",rclcpp::ParameterValue(0.2));
	nav2_util::declare_parameter_if_not_declared(parent,plugin_name_ + ".cost_yaw_gain",rclcpp::ParameterValue(0.2));
	nav2_util::declare_parameter_if_not_declared(parent,plugin_name_ + ".low_pass_gain",rclcpp::ParameterValue(0.2));

	nav2_util::declare_parameter_if_not_declared(parent,plugin_name_ + ".max_cost",rclcpp::ParameterValue(0.2));
	nav2_util::declare_parameter_if_not_declared(parent,plugin_name_ + ".max_curve_vel",rclcpp::ParameterValue(0.2));
	nav2_util::declare_parameter_if_not_declared(parent,plugin_name_ + ".max_goal_dist",rclcpp::ParameterValue(0.2));
	nav2_util::declare_parameter_if_not_declared(parent,plugin_name_ + ".max_backup_dist", rclcpp::ParameterValue(0.2));
	nav2_util::declare_parameter_if_not_declared(parent,plugin_name_ + ".min_stop_dist",rclcpp::ParameterValue(0.2));
	nav2_util::declare_parameter_if_not_declared(parent,plugin_name_ + ".emergency_acc_lim_x",rclcpp::ParameterValue(0.2));
	nav2_util::declare_parameter_if_not_declared(parent,plugin_name_ + ".differential_drive", rclcpp::ParameterValue(true));
	nav2_util::declare_parameter_if_not_declared(parent,plugin_name_ + ".constrain_final", rclcpp::ParameterValue(false));

	parent->get_parameter_or(plugin_name_ + ".acc_lim_x", acc_lim_x, 0.5);
	parent->get_parameter_or(plugin_name_ + ".acc_lim_y", acc_lim_y, 0.5);
	parent->get_parameter_or(plugin_name_ + ".acc_lim_theta", acc_lim_theta, 0.5);
	parent->get_parameter_or(plugin_name_ + ".acc_limit_trans", acc_lim_trans, 0.5);
	parent->get_parameter_or(plugin_name_ + ".min_vel_x", min_vel_x, -0.1);
	parent->get_parameter_or(plugin_name_ + ".max_vel_x", max_vel_x, 0.5);
	parent->get_parameter_or(plugin_name_ + ".min_vel_y", min_vel_y, -0.5);
	parent->get_parameter_or(plugin_name_ + ".max_vel_y", max_vel_y, 0.5);
	parent->get_parameter_or(plugin_name_ + ".min_rot_vel", min_vel_theta, 0.1);
	parent->get_parameter_or(plugin_name_ + ".max_rot_vel", max_vel_theta, 0.5);
	parent->get_parameter_or(plugin_name_ + ".min_trans_vel", min_vel_trans, 0.1);
	parent->get_parameter_or(plugin_name_ + ".max_trans_vel", max_vel_trans, 0.5);
	parent->get_parameter_or(plugin_name_ + ".rot_stopped_vel", theta_stopped_vel, 0.05);
	parent->get_parameter_or(plugin_name_ + ".trans_stopped_vel", trans_stopped_vel, 0.05);
	parent->get_parameter_or(plugin_name_ + ".yaw_goal_tolerance", yaw_goal_tolerance, 0.02);
	parent->get_parameter_or(plugin_name_ + ".xy_goal_tolerance", xy_goal_tolerance, 0.1);

	parent->get_parameter_or(plugin_name_ + ".goal_tune_time", goal_tune_time, 0.5);
	parent->get_parameter_or(plugin_name_ + ".lookahead_time", lookahead_time, 0.5);
	parent->get_parameter_or(plugin_name_ + ".lookahead_dist", m_lookahead_dist, 0.5);
	parent->get_parameter_or(plugin_name_ + ".start_yaw_error", start_yaw_error, 0.2);
	parent->get_parameter_or(plugin_name_ + ".pos_x_gain", pos_x_gain, 1.0);
	parent->get_parameter_or(plugin_name_ + ".pos_y_gain", pos_y_gain, 1.0);
	parent->get_parameter_or(plugin_name_ + ".pos_y_yaw_gain", pos_y_yaw_gain, 1.0);
	parent->get_parameter_or(plugin_name_ + ".yaw_gain", yaw_gain, 1.0);
	parent->get_parameter_or(plugin_name_ + ".static_yaw_gain", static_yaw_gain, 3.0);
	parent->get_parameter_or(plugin_name_ + ".cost_x_gain", cost_x_gain, 0.1);
	parent->get_parameter_or(plugin_name_ + ".cost_y_gain", cost_y_gain, 0.1);
	parent->get_parameter_or(plugin_name_ + ".cost_y_yaw_gain", cost_y_yaw_gain, 0.1);
	parent->get_parameter_or(plugin_name_ + ".cost_y_lookahead_dist", m_cost_y_lookahead_dist, 0.0);
	parent->get_parameter_or(plugin_name_ + ".cost_y_lookahead_time", cost_y_lookahead_time, 1.0);
	parent->get_parameter_or(plugin_name_ + ".cost_yaw_gain", cost_yaw_gain, 1.0);
	parent->get_parameter_or(plugin_name_ + ".low_pass_gain", low_pass_gain, 0.5);

	parent->get_parameter_or(plugin_name_ + ".max_cost", max_cost, 0.9);
	parent->get_parameter_or(plugin_name_ + ".max_curve_vel", max_curve_vel, 0.2);
	parent->get_parameter_or(plugin_name_ + ".max_goal_dist", max_goal_dist, 0.5);
	parent->get_parameter_or(plugin_name_ + ".max_backup_dist", max_backup_dist, 0.5);
	parent->get_parameter_or(plugin_name_ + ".min_stop_dist", min_stop_dist, 0.5);
	parent->get_parameter_or(plugin_name_ + ".emergency_acc_lim_x", emergency_acc_lim_x, 0.5);
	parent->get_parameter_or(plugin_name_ + ".differential_drive", differential_drive, true);
	parent->get_parameter_or(plugin_name_ + ".constrain_final", constrain_final, false);

	// Variable manipulation
	acc_lim_trans = acc_lim_x;
	max_vel_trans = max_vel_x;
	trans_stopped_vel = 0.5 * min_vel_trans;

	// Setting up the costmap variables
	costmap_ros_ = costmap_ros;
	costmap_ = costmap_ros_->getCostmap();
	tf_ = tf;
	plugin_name_ = name;
	logger_ = parent->get_logger();

	m_base_frame = costmap_ros->getBaseFrameID();

	// Creating odometery subscriber and local plan publisher
	m_odom_sub = parent->create_subscription<nav_msgs::msg::Odometry>("/odom",  rclcpp::SystemDefaultsQoS(), std::bind(&NeoLocalPlanner::odomCallback,this,std::placeholders::_1));
	m_local_plan_pub = parent->create_publisher<nav_msgs::msg::Path>("/local_plan", 1);

}

void NeoLocalPlanner::odomCallback(const nav_msgs::msg::Odometry::SharedPtr msg)
{
	boost::mutex::scoped_lock lock(m_odometry_mutex);
	m_odometry = msg;
}

}

// neo_local_planner

PLUGINLIB_EXPORT_CLASS(neo_local_planner::NeoLocalPlanner, nav2_core::Controller)
