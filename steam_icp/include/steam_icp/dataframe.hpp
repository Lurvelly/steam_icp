#pragma once

#include <Eigen/Dense>

#include "steam/problem/cost_term/imu_super_cost_term.hpp"
#include "steam_icp/point.hpp"
#include "steam_icp/pose.hpp"

namespace steam_icp {

struct DataFrame {
  double timestamp;
  std::vector<Point3D> pointcloud;
  std::vector<steam::IMUData> imu_data_vec;
  std::vector<PoseData> pose_data_vec;
};

}  // namespace steam_icp