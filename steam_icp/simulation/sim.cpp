#include <filesystem>
namespace fs = std::filesystem;

#include "glog/logging.h"

#include "nav_msgs/msg/odometry.hpp"
#include "pcl_conversions/pcl_conversions.h"
#include "rclcpp/rclcpp.hpp"
#include "sensor_msgs/msg/point_cloud2.hpp"
#include "tf2/convert.h"
#include "tf2_eigen/tf2_eigen.h"
#include "tf2_ros/static_transform_broadcaster.h"
#include "tf2_ros/transform_broadcaster.h"

#define PCL_NO_PRECOMPILE
#include <pcl/io/pcd_io.h>
#include <pcl/pcl_macros.h>
#include <pcl/point_cloud.h>
#include <pcl/point_types.h>

#include "lgmath.hpp"

#include "steam_icp/point.hpp"

#include <unistd.h>
#include <random>
#include <unsupported/Eigen/MatrixFunctions>

const uint64_t VLS128_CHANNEL_TDURATION_NS = 2665;
const uint64_t VLS128_SEQ_TDURATION_NS = 53300;
const uint64_t VLS128_FIRING_SEQUENCE_PER_REV = 1876;
const double AZIMUTH_STEP = 2 * M_PI / VLS128_FIRING_SEQUENCE_PER_REV;
const double INTER_AZM_STEP = AZIMUTH_STEP / 20;

// using namespace steam_icp;

namespace simulation {

struct Point3D {
  Eigen::Vector3d raw_pt;  // Raw point read from the sensor
  Eigen::Vector3d pt;      // Corrected point taking into account the motion of the sensor during frame acquisition
  double radial_velocity = 0.0;  // Radial velocity of the point
  double alpha_timestamp = 0.0;  // Relative timestamp in the frame in [0.0, 1.0]
  double timestamp = 0.0;        // The absolute timestamp (if applicable)
  int beam_id = -1;              // The beam id of the point
};

#define PCL_ADD_FLEXIBLE     \
  union EIGEN_ALIGN16 {      \
    __uint128_t raw_flex1;   \
    float data_flex1[4];     \
    struct {                 \
      float flex11;          \
      float flex12;          \
      float flex13;          \
      float flex14;          \
    };                       \
    struct {                 \
      float alpha_timestamp; \
      float timestamp;       \
      float radial_velocity; \
    };                       \
  };

struct EIGEN_ALIGN16 PCLPoint3D {
  PCL_ADD_POINT4D;
  PCL_ADD_FLEXIBLE;
  PCL_MAKE_ALIGNED_OPERATOR_NEW

  inline PCLPoint3D() {
    x = y = z = 0.0f;
    data[3] = 1.0f;
    raw_flex1 = 0;
  }

  inline PCLPoint3D(const PCLPoint3D &p) {
    x = p.x;
    y = p.y;
    z = p.z;
    data[3] = 1.0f;
    raw_flex1 = p.raw_flex1;
  }

  inline PCLPoint3D(const Point3D &p) {
    x = (float)p.pt[0];
    y = (float)p.pt[1];
    z = (float)p.pt[2];
    data[3] = 1.0f;
    alpha_timestamp = p.alpha_timestamp;
    timestamp = p.timestamp;
    radial_velocity = p.radial_velocity;
  }

  inline PCLPoint3D(const Eigen::Vector3d &p) {
    x = (float)p[0];
    y = (float)p[1];
    z = (float)p[2];
    data[3] = 1.0f;
  }
};

struct SimulationOptions {
  std::string output_dir = "/sim_output";  // output path (relative or absolute) to save simulation data
  std::string root_path = "";
  std::string sequence = "";
  std::string lidar_config = "";
  Eigen::Matrix4d T_sr = Eigen::Matrix4d::Identity();
  int num_threads = 20;
  bool verbose = false;
  double imu_rate = 200.0;
  double offset_imu = 0.0025;  // offset bewteen first imu meas and first lidar meas
  double min_dist_sensor_center = 0.1;
  double max_dist_sensor_center = 200.0;
  bool noisy_measurements = false;
  double sim_length = 5.0;
  // approximate from spec sheet of velodyne
  double lidar_range_std = 0.02;
  Eigen::Matrix<double, 3, 1> r_accel = Eigen::Matrix<double, 3, 1>::Ones();
  Eigen::Matrix<double, 3, 1> r_gyro = Eigen::Matrix<double, 3, 1>::Ones();
  double gravity = -9.8042;
  // todo: simulate bias P0 and Qc
  double p0_bias = 0.01;
  double q_bias = 0.01;
  // learned from Boreas data
  Eigen::Matrix<double, 6, 1> qc_diag = Eigen::Matrix<double, 6, 1>::Ones();
  Eigen::Matrix<double, 6, 1> ad_diag = Eigen::Matrix<double, 6, 1>::Ones();
  Eigen::Matrix<double, 18, 1> x0 = Eigen::Matrix<double, 18, 1>::Zero();
  double accel_ramp_time = 0.3;
  std::vector<double> walls = {-100.0, 100.0, -100.0, 100.0, 0.0, 4.0};
  std::vector<double> intensities = {0.15, 0.30, 0.45, 0.60, 0.75, 0.90};
  double sleep_delay = 1.0;
  std::vector<double> v_freqs;
  std::vector<double> v_amps;
  double ax = 2.0;
  std::vector<double> biases;
  double pose_meas_trans_sigma = 0.1;
  double pose_meas_rot_sigma_degs = 5.0;
  double pose_rate = 10.0;
  Eigen::Matrix<double, 3, 1> xi_ig = Eigen::Matrix<double, 3, 1>::Zero();
};

#define ROS2_PARAM_NO_LOG(node, receiver, prefix, param, type) \
  receiver = node->declare_parameter<type>(prefix + #param, receiver);
#define ROS2_PARAM(node, receiver, prefix, param, type)   \
  ROS2_PARAM_NO_LOG(node, receiver, prefix, param, type); \
  LOG(WARNING) << "Parameter " << prefix + #param << " = " << receiver << std::endl;
#define ROS2_PARAM_CLAUSE(node, config, prefix, param, type)                   \
  config.param = node->declare_parameter<type>(prefix + #param, config.param); \
  LOG(WARNING) << "Parameter " << prefix + #param << " = " << config.param << std::endl;

SimulationOptions loadOptions(const rclcpp::Node::SharedPtr &node) {
  SimulationOptions options;
  std::string prefix = "";
  ROS2_PARAM_CLAUSE(node, options, prefix, output_dir, std::string);
  if (!options.output_dir.empty() && options.output_dir[options.output_dir.size() - 1] != '/')
    options.output_dir += '/';
  ROS2_PARAM_CLAUSE(node, options, prefix, root_path, std::string);
  ROS2_PARAM_CLAUSE(node, options, prefix, sequence, std::string);
  ROS2_PARAM_CLAUSE(node, options, prefix, lidar_config, std::string);
  ROS2_PARAM_CLAUSE(node, options, prefix, num_threads, int);
  ROS2_PARAM_CLAUSE(node, options, prefix, verbose, bool);
  ROS2_PARAM_CLAUSE(node, options, prefix, imu_rate, double);
  ROS2_PARAM_CLAUSE(node, options, prefix, offset_imu, double);
  ROS2_PARAM_CLAUSE(node, options, prefix, min_dist_sensor_center, double);
  ROS2_PARAM_CLAUSE(node, options, prefix, max_dist_sensor_center, double);
  ROS2_PARAM_CLAUSE(node, options, prefix, noisy_measurements, bool);
  ROS2_PARAM_CLAUSE(node, options, prefix, sim_length, double);
  ROS2_PARAM_CLAUSE(node, options, prefix, lidar_range_std, double);
  ROS2_PARAM_CLAUSE(node, options, prefix, gravity, double);
  ROS2_PARAM_CLAUSE(node, options, prefix, p0_bias, double);
  ROS2_PARAM_CLAUSE(node, options, prefix, q_bias, double);
  ROS2_PARAM_CLAUSE(node, options, prefix, sleep_delay, double);
  ROS2_PARAM_CLAUSE(node, options, prefix, accel_ramp_time, double);
  ROS2_PARAM_CLAUSE(node, options, prefix, ax, double);
  ROS2_PARAM_CLAUSE(node, options, prefix, pose_meas_trans_sigma, double);
  ROS2_PARAM_CLAUSE(node, options, prefix, pose_meas_rot_sigma_degs, double);
  ROS2_PARAM_CLAUSE(node, options, prefix, pose_rate, double);

  std::vector<double> r_accel;
  ROS2_PARAM_NO_LOG(node, r_accel, prefix, r_accel, std::vector<double>);
  if ((r_accel.size() != 3) && (r_accel.size() != 0))
    throw std::invalid_argument{"r_accel malformed. Must be 3 elements!"};
  if (r_accel.size() == 3) options.r_accel << r_accel[0], r_accel[1], r_accel[2];
  LOG(WARNING) << "Parameter " << prefix + "r_accel"
               << " = " << options.r_accel.transpose() << std::endl;

  std::vector<double> r_gyro;
  ROS2_PARAM_NO_LOG(node, r_gyro, prefix, r_gyro, std::vector<double>);
  if ((r_gyro.size() != 3) && (r_gyro.size() != 0))
    throw std::invalid_argument{"r_gyro malformed. Must be 3 elements!"};
  if (r_gyro.size() == 3) options.r_gyro << r_gyro[0], r_gyro[1], r_gyro[2];
  LOG(WARNING) << "Parameter " << prefix + "r_gyro"
               << " = " << options.r_gyro.transpose() << std::endl;

  std::vector<double> qc_diag;
  ROS2_PARAM_NO_LOG(node, qc_diag, prefix, qc_diag, std::vector<double>);
  if ((qc_diag.size() != 6) && (qc_diag.size() != 0))
    throw std::invalid_argument{"qc_diag malformed. Must be 6 elements!"};
  if (qc_diag.size() == 6) options.qc_diag << qc_diag[0], qc_diag[1], qc_diag[2], qc_diag[3], qc_diag[4], qc_diag[5];
  LOG(WARNING) << "Parameter " << prefix + "qc_diag"
               << " = " << options.qc_diag.transpose() << std::endl;

  std::vector<double> ad_diag;
  ROS2_PARAM_NO_LOG(node, ad_diag, prefix, ad_diag, std::vector<double>);
  if ((ad_diag.size() != 6) && (ad_diag.size() != 0))
    throw std::invalid_argument{"ad_diag malformed. Must be 6 elements!"};
  if (ad_diag.size() == 6) options.ad_diag << ad_diag[0], ad_diag[1], ad_diag[2], ad_diag[3], ad_diag[4], ad_diag[5];
  LOG(WARNING) << "Parameter " << prefix + "ad_diag"
               << " = " << options.ad_diag.transpose() << std::endl;

  Eigen::Matrix4d yfwd2xfwd;
  yfwd2xfwd << 0, 1, 0, 0, -1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1;
  fs::path root_path{options.root_path};
  std::ifstream ifs(root_path / options.sequence / "calib" / "T_applanix_lidar.txt", std::ios::in);
  Eigen::Matrix4d T_applanix_lidar_mat;
  for (size_t row = 0; row < 4; row++)
    for (size_t col = 0; col < 4; col++) ifs >> T_applanix_lidar_mat(row, col);
  options.T_sr = (yfwd2xfwd * T_applanix_lidar_mat).inverse();
  LOG(WARNING) << "(BOREAS)Parameter T_sr = " << std::endl << options.T_sr << std::endl;

  std::vector<double> x0;
  ROS2_PARAM_NO_LOG(node, x0, prefix, x0, std::vector<double>);
  if ((x0.size() != 18) && (x0.size() != 0)) throw std::invalid_argument{"x0 malformed. Must be 18 elements!"};
  if (x0.size() == 18)
    options.x0 << x0[0], x0[1], x0[2], x0[3], x0[4], x0[5], x0[6], x0[7], x0[8], x0[9], x0[10], x0[11], x0[12], x0[13],
        x0[14], x0[15], x0[16], x0[17];
  LOG(WARNING) << "Parameter " << prefix + "x0"
               << " = " << options.x0.transpose() << std::endl;

  std::vector<double> walls;
  ROS2_PARAM_NO_LOG(node, walls, prefix, walls, std::vector<double>);
  if ((walls.size() != 6) && (walls.size() != 0)) throw std::invalid_argument{"walls malformed. Must be 6 elements!"};
  if (walls.size() == 6) options.walls = {walls[0], walls[1], walls[2], walls[3], walls[4], walls[5]};
  LOG(WARNING) << "Parameter " << prefix + "walls"
               << " = " << walls[0] << walls[1] << walls[2] << walls[3] << walls[4] << walls[5] << std::endl;

  std::vector<double> v_freqs;
  ROS2_PARAM_NO_LOG(node, v_freqs, prefix, v_freqs, std::vector<double>);
  if ((v_freqs.size() != 6) && (v_freqs.size() != 0))
    throw std::invalid_argument{"v_freqs malformed. Must be 6 elements!"};
  if (v_freqs.size() == 6) options.v_freqs = {v_freqs[0], v_freqs[1], v_freqs[2], v_freqs[3], v_freqs[4], v_freqs[5]};
  LOG(WARNING) << "Parameter " << prefix + "v_freqs"
               << " = " << v_freqs[0] << v_freqs[1] << v_freqs[2] << v_freqs[3] << v_freqs[4] << v_freqs[5]
               << std::endl;

  std::vector<double> v_amps;
  ROS2_PARAM_NO_LOG(node, v_amps, prefix, v_amps, std::vector<double>);
  if ((v_amps.size() != 6) && (v_amps.size() != 0))
    throw std::invalid_argument{"v_amps malformed. Must be 6 elements!"};
  if (v_amps.size() == 6) options.v_amps = {v_amps[0], v_amps[1], v_amps[2], v_amps[3], v_amps[4], v_amps[5]};
  LOG(WARNING) << "Parameter " << prefix + "v_amps"
               << " = " << v_amps[0] << v_amps[1] << v_amps[2] << v_amps[3] << v_amps[4] << v_amps[5] << std::endl;

  std::vector<double> biases;
  ROS2_PARAM_NO_LOG(node, biases, prefix, biases, std::vector<double>);
  if ((biases.size() != 6) && (biases.size() != 0))
    throw std::invalid_argument{"biases malformed. Must be 6 elements!"};
  if (biases.size() == 6) options.biases = {biases[0], biases[1], biases[2], biases[3], biases[4], biases[5]};
  LOG(WARNING) << "Parameter " << prefix + "biases"
               << " = " << biases[0] << biases[1] << biases[2] << biases[3] << biases[4] << biases[5] << std::endl;

  std::vector<double> xi_ig;
  ROS2_PARAM_NO_LOG(node, xi_ig, prefix, xi_ig, std::vector<double>);
  if ((xi_ig.size() != 3) && (xi_ig.size() != 0)) throw std::invalid_argument{"xi_ig malformed. Must be 3 elements!"};
  if (xi_ig.size() == 3) options.xi_ig << xi_ig[0], xi_ig[1], xi_ig[2];
  LOG(WARNING) << "Parameter " << prefix + "xi_ig"
               << " = " << xi_ig[0] << xi_ig[1] << xi_ig[2] << std::endl;

  return options;
}

Eigen::Matrix<double, 128, 3> loadVLS128Config(const std::string &file_path) {
  Eigen::Matrix<double, 128, 3> output = Eigen::Matrix<double, 128, 3>::Zero();
  std::ifstream calib_file(file_path);
  if (calib_file.is_open()) {
    std::string line;
    std::getline(calib_file, line);  // header
    int k = 0;
    for (; std::getline(calib_file, line);) {
      if (line.empty()) continue;
      if (k >= 128) break;
      std::stringstream ss(line);

      double rot_correction = 0;
      double vert_correction = 0;
      int laser_id = 0;

      for (int i = 0; i < 9; ++i) {
        std::string value;
        std::getline(ss, value, ',');
        if (i == 1)
          rot_correction = std::stod(value);
        else if (i == 2)
          vert_correction = std::stod(value);
        else if (i == 7)
          laser_id = std::stol(value);
      }
      output(k, 0) = laser_id;
      output(k, 1) = rot_correction;
      output(k, 2) = vert_correction;
      k++;
    }
  }
  return output;
}

Eigen::Vector3d rot_to_yaw_pitch_roll(Eigen::Matrix3d C) {
  int i = 2, j = 1, k = 0;
  double c_y = std::sqrt(C(i, i) * C(i, i) + C(j, i) * C(j, i));
  double r = 0, p = 0, y = 0;
  if (c_y > 1.0e-14) {
    r = std::atan2(C(j, i), C(i, i));
    p = std::atan2(-C(k, i), c_y);
    y = std::atan2(C(k, j), C(k, k));
  } else {
    r = 0;
    p = std::atan2(-C(k, i), c_y);
    y = std::atan2(-C(j, k), C(j, j));
  }
  Eigen::Vector3d ypr;
  ypr << y, p, r;
  return ypr;
}

}  // namespace simulation

// clang-format off
POINT_CLOUD_REGISTER_POINT_STRUCT(
    simulation::PCLPoint3D,
    // cartesian coordinates
    (float, x, x)
    (float, y, y)
    (float, z, z)
    // random stuff
    (float, flex11, flex11)
    (float, flex12, flex12)
    (float, flex13, flex13)
    (float, flex14, flex14))
// clang-format on

int main(int argc, char **argv) {
  using namespace simulation;

  rclcpp::init(argc, argv);
  auto node = rclcpp::Node::make_shared("simulation");
  auto odometry_publisher = node->create_publisher<nav_msgs::msg::Odometry>("/simulation_odometry", 10);
  auto tf_static_bc = std::make_shared<tf2_ros::StaticTransformBroadcaster>(node);
  auto tf_bc = std::make_shared<tf2_ros::TransformBroadcaster>(node);
  auto raw_points_publisher = node->create_publisher<sensor_msgs::msg::PointCloud2>("/simulation_raw", 2);

  auto to_pc2_msg = [](const auto &points, const std::string &frame_id = "map") {
    pcl::PointCloud<PCLPoint3D> points_pcl;
    points_pcl.reserve(points.size());
    for (auto &pt : points) points_pcl.emplace_back(pt);
    sensor_msgs::msg::PointCloud2 points_msg;
    pcl::toROSMsg(points_pcl, points_msg);
    points_msg.header.frame_id = frame_id;
    // points_msg.header.stamp = rclcpp::Time(stamp);
    return points_msg;
  };

  // Logging
  FLAGS_log_dir = node->declare_parameter<std::string>("log_dir", "/tmp");
  FLAGS_alsologtostderr = 1;
  fs::create_directories(FLAGS_log_dir);
  google::InitGoogleLogging(argv[0]);
  LOG(WARNING) << "Logging to " << FLAGS_log_dir;

  // Read parameters
  auto options = loadOptions(node);
  std::mt19937 rg;
  std::normal_distribution n_lidar{0.0, options.lidar_range_std};
  std::normal_distribution n_accel{0.0, options.r_accel(0, 0)};
  std::normal_distribution n_gyro{0.0, options.r_gyro(0, 0)};
  std::normal_distribution n_pose_trans{0.0, options.pose_meas_trans_sigma};
  std::normal_distribution n_pose_rot{0.0, options.pose_meas_rot_sigma_degs * M_PI / 180.0};

  // Publish sensor vehicle transformations
  auto T_rs_msg = tf2::eigenToTransform(Eigen::Affine3d(options.T_sr.inverse()));
  T_rs_msg.header.frame_id = "vehicle";
  T_rs_msg.child_frame_id = "sensor";
  tf_static_bc->sendTransform(T_rs_msg);

  // Build the Output_dir
  LOG(WARNING) << "Creating directory " << options.output_dir << std::endl;
  fs::path output_path{options.output_dir};
  fs::create_directories(output_path);
  fs::create_directories(output_path / "lidar");
  fs::create_directories(output_path / "applanix");
  fs::create_directories(output_path / "calib");
  fs::path root_path{options.root_path};
  const auto copyOptions = fs::copy_options::update_existing;
  fs::copy(root_path / options.sequence / "calib" / "T_applanix_lidar.txt", output_path / "calib" / "T_applanix_lidar.txt", copyOptions);

  // Load VLS128 Config
  const auto lidar_config = loadVLS128Config(options.lidar_config);
  LOG(WARNING) << "lidar config" << std::endl << lidar_config << std::endl;

  const uint64_t sim_length_ns = options.sim_length * 1.0e9;

  // TODO: create a sine wave on each axis in body frame
  const auto &v_freqs = options.v_freqs;
  const auto &v_amps = options.v_amps;
  const double &ax = options.ax;

  // Pick a linear and angular jerk, and then step through simulation
  uint64_t tns = 0;
  Eigen::Matrix4d T_ri =
      lgmath::se3::Transformation(Eigen::Matrix<double, 6, 1>(options.x0.block<6, 1>(0, 0))).matrix();
  const uint64_t delta_ns = VLS128_FIRING_SEQUENCE_PER_REV * VLS128_SEQ_TDURATION_NS;
  const double delta_s = delta_ns * 1.0e-9;
  Eigen::Matrix<double, 6, 1> w = Eigen::Matrix<double, 6, 1>::Zero();
  Eigen::Matrix<double, 6, 1> dw = Eigen::Matrix<double, 6, 1>::Zero();

  const double wall_delta = 1.0e-6;
  const uint64_t t0_us = 1695166988000000;  // just pick a random epoch time to conform to the expected format
  
  std::ofstream lidar_pose_out(output_path / "applanix" / "lidar_poses.csv", std::ios::out);
  std::ofstream lidar_pose_tum(output_path / "applanix" / "lidar_poses_tum.txt", std::ios::out);
  std::ofstream lidar_pose_meas(output_path / "applanix" / "lidar_pose_meas.csv", std::ios::out);
  lidar_pose_out << std::setprecision(std::numeric_limits<long double>::digits10 + 1);
  lidar_pose_tum << std::setprecision(std::numeric_limits<long double>::digits10 + 1);
  lidar_pose_meas << std::setprecision(std::numeric_limits<long double>::digits10 + 1);
  lidar_pose_out
      << "GPSTime,easting,northing,altitude,vel_east,vel_north,vel_up,roll,pitch,heading,angvel_z,angvel_y,angvel_x"
      << std::endl;
  lidar_pose_meas << "GPSTime,T00,T01,T02,T03,T10,T11,T12,T13,T20,T21,T22,T23" << std::endl;
  LOG(INFO) << "starting simulation..." << std::endl;
  int pc_index = 0;
  while (tns < sim_length_ns) {
    std::vector<Point3D> points;
    points.reserve(VLS128_FIRING_SEQUENCE_PER_REV * 128);
    LOG(INFO) << "simulation time: " << tns * 1.0e-9 << std::endl;
    const double t_mid_s = (tns + delta_ns / 2) * 1.0e-9;
    const double t_end_s = (tns + delta_ns) * 1.0e-9;
    // #pragma omp declare reduction(merge_points : std::vector<Point3D> : omp_out.insert( \
//         omp_out.end(), omp_in.begin(), omp_in.end()))
    // #pragma omp parallel for num_threads(options.num_threads) reduction(merge_points : points)
    Eigen::Matrix4d T_ri_local = T_ri;
    uint64_t sensor_tns_prev = tns;
    double min_diff_t_mid_s = std::numeric_limits<double>::max();
    uint64_t t_mid_min_ns = 0;
    Eigen::Matrix4d T_ri_mid_min = T_ri;
    double min_diff_t_end_s = std::numeric_limits<double>::max();
    uint64_t t_end_min_ns = 0;
    Eigen::Matrix4d T_ri_end_min = T_ri;
    for (uint64_t seq_index = 0; seq_index < VLS128_FIRING_SEQUENCE_PER_REV; seq_index++) {
      for (int group = 0; group < 16; group++) {
        uint64_t sensor_tns = tns + seq_index * VLS128_SEQ_TDURATION_NS + group * VLS128_CHANNEL_TDURATION_NS;
        double sensor_azimuth = seq_index * AZIMUTH_STEP + group * INTER_AZM_STEP;
        if (group >= 8) {
          sensor_tns += VLS128_CHANNEL_TDURATION_NS;
          sensor_azimuth += INTER_AZM_STEP;
        }
        double sensor_s = sensor_tns * 1.0e-9;

        if (sensor_tns > 2 * delta_ns) {
          for (int j = 0; j < 6; ++j) {
            // if (j == 0) {
            //   w(j, 0) = -ax * (sensor_s - 2 * delta_s);
            //   dw(j, 0) = -ax;
            // } else {
            w(j, 0) = -v_amps[j] * std::sin(v_freqs[j] * (sensor_s - 2 * delta_s) * (2 * M_PI));
            dw(j, 0) =
                (-v_amps[j] * v_freqs[j] * (2 * M_PI)) * std::cos(v_freqs[j] * (sensor_s - 2 * delta_s) * (2 * M_PI));
            // }
          }
          w += options.x0.block<6, 1>(6, 0);
        }
        const double dtg = (sensor_tns - sensor_tns_prev) * 1.0e-9;
        T_ri_local = lgmath::se3::Transformation(Eigen::Matrix<double, 6, 1>(w * dtg + 0.5 * dtg * dtg * dw)).matrix() *
                     T_ri_local;
        // check orthogonality of C_ri:
        Eigen::Matrix3d C_ri = T_ri_local.block<3, 3>(0, 0);
        const double orthog_error = fabs((C_ri * C_ri.transpose()).squaredNorm() - 3.0);
        if (orthog_error > 1e-6) {
          // Reproject onto SO(3) if we stray too far
          LOG(INFO) << "reprojecting, error: " << orthog_error << std::endl;
          T_ri_local.block<3, 3>(0, 0) =
              Eigen::Matrix3d(Eigen::Matrix3d((C_ri * C_ri.transpose()).inverse().sqrt()) * C_ri);
        }

        if (fabs(sensor_s - t_mid_s) < min_diff_t_mid_s) {
          min_diff_t_mid_s = fabs(sensor_s - t_mid_s);
          t_mid_min_ns = sensor_tns;
          T_ri_mid_min = T_ri_local;
        }

        if (fabs(sensor_s - t_end_s) < min_diff_t_end_s) {
          min_diff_t_end_s = fabs(sensor_s - t_end_s);
          t_end_min_ns = sensor_tns;
          T_ri_end_min = T_ri_local;
        }

        const Eigen::Matrix4d T_si_local = options.T_sr * T_ri_local;
        const Eigen::Matrix4d T_is_local = T_si_local.inverse();
        const Eigen::Matrix3d C_is_local = T_is_local.block<3, 3>(0, 0);
        const Eigen::Vector3d r_si_in_i = T_is_local.block<3, 1>(0, 3);

        for (int beam_id = group * 8; beam_id < group * 8 + 8; beam_id++) {
          const double beam_azimuth = sensor_azimuth - lidar_config(beam_id, 1);
          const double beam_elevation = lidar_config(beam_id, 2);
          Eigen::Vector3d n_s;
          n_s << std::cos(beam_elevation) * std::cos(beam_azimuth), std::cos(beam_elevation) * std::sin(beam_azimuth),
              std::sin(beam_elevation);
          n_s.normalize();
          Eigen::Vector3d n_i = C_is_local * n_s;
          n_i.normalize();

          double rmin = std::numeric_limits<double>::max();
          double xmin = 0, ymin = 0, zmin = 0;
          double imin = 0;

          for (int wall = 0; wall < 6; wall++) {
            double t = 0;
            if (wall < 2) {
              if (fabs(n_i(0, 0)) < wall_delta) continue;
              t = (options.walls[wall] - r_si_in_i(0, 0)) / n_i(0, 0);
            } else if (wall >= 2 && wall < 4) {
              if (fabs(n_i(1, 0)) < wall_delta) continue;
              t = (options.walls[wall] - r_si_in_i(1, 0)) / n_i(1, 0);
            } else {
              if (fabs(n_i(2, 0)) < wall_delta) continue;
              t = (options.walls[wall] - r_si_in_i(2, 0)) / n_i(2, 0);
            }
            if (t < 0) continue;
            const double xw = r_si_in_i(0, 0) + n_i(0, 0) * t;
            const double yw = r_si_in_i(1, 0) + n_i(1, 0) * t;
            const double zw = r_si_in_i(2, 0) + n_i(2, 0) * t;
            // check that point is inside bounds of cube.
            if (xw < (options.walls[0] - 0.1) || xw > (options.walls[1] + 0.1) || yw < (options.walls[2] - 0.1) ||
                yw > (options.walls[3] + 0.1) || zw < (options.walls[4] - 0.1) || zw > (options.walls[5] + 0.1))
              continue;

            const double dx = xw - r_si_in_i(0, 0);
            const double dy = yw - r_si_in_i(1, 0);
            const double dz = zw - r_si_in_i(2, 0);
            const double r = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (r < rmin) {
              rmin = r;
              Eigen::Matrix<double, 4, 1> x_i;
              x_i << xw, yw, zw, 1.0;
              const Eigen::Matrix<double, 4, 1> x_s = T_si_local * x_i;
              xmin = x_s(0, 0);
              ymin = x_s(1, 0);
              zmin = x_s(2, 0);
              imin = options.intensities[wall];
            }
          }
          if (xmin != 0 && ymin != 0 && zmin != 0) {
            Point3D p;
            p.raw_pt << xmin, ymin, zmin;
            p.pt << xmin, ymin, zmin;
            if (options.noisy_measurements) {
              p.raw_pt += n_s * n_lidar(rg);
            }
            p.radial_velocity = imin;
            p.alpha_timestamp = 0.0;
            p.timestamp = sensor_s - t_mid_s;
            points.emplace_back(p);
          }
        }
        sensor_tns_prev = sensor_tns;
      }
    }
    points.shrink_to_fit();

    // write the pointcloud to file as a binary
    const uint64_t t_mid_us = (tns + delta_ns / 2) / 1000 + t0_us;
    std::string fname = std::to_string(t_mid_us) + ".bin";
    std::ofstream fout(output_path / "lidar" / fname, std::ios::out | std::ios::binary);
    const size_t fsize = 4;
    const float dummy = 0.;
    for (auto &p : points) {
      const float x = p.pt(0, 0);
      const float y = p.pt(1, 0);
      const float z = p.pt(2, 0);
      const float intensity = p.radial_velocity;
      const float t = p.timestamp;
      fout.write((char *)&x, fsize);
      fout.write((char *)&y, fsize);
      fout.write((char *)&z, fsize);
      fout.write((char *)&intensity, fsize);
      fout.write((char *)&dummy, fsize);
      fout.write((char *)&t, fsize);
    }
    fout.close();

    // publish pointcloud
    auto raw_points_msg = to_pc2_msg(points, "sensor");
    raw_points_publisher->publish(raw_points_msg);

    if (min_diff_t_mid_s != 0) {
      const int64_t dtns = tns + (delta_ns / 2) - t_mid_min_ns;
      if (t_mid_min_ns > 2 * delta_ns) {
        for (int j = 0; j < 6; ++j) {
          // if (j == 0) {
          //   w(j, 0) = -ax * (t_mid_min_ns * 1.0e-9 - 2 * delta_s);
          //   dw(j, 0) = -ax;
          // } else {
          w(j, 0) = -v_amps[j] * std::sin(v_freqs[j] * (t_mid_min_ns * 1.0e-9 - 2 * delta_s) * (2 * M_PI));
          dw(j, 0) = (-v_amps[j] * v_freqs[j] * (2 * M_PI)) *
                     std::cos(v_freqs[j] * (t_mid_min_ns * 1.0e-9 - 2 * delta_s) * (2 * M_PI));
          // }
        }
        w += options.x0.block<6, 1>(6, 0);
      }
      double dtg = dtns * 1.0e-9;
      T_ri = lgmath::se3::Transformation(Eigen::Matrix<double, 6, 1>(w * dtg + 0.5 * dtg * dtg * dw)).matrix() *
             T_ri_mid_min;
    } else {
      T_ri = T_ri_mid_min;
    }
    tns += (delta_ns / 2);

    if (!rclcpp::ok()) {
      LOG(WARNING) << "Shutting down due to ctrl-c." << std::endl;
      return 0;
    }
    LOG(INFO) << "T_ri:" << T_ri << std::endl;
    LOG(INFO) << "w:" << w.transpose() << std::endl;

    const Eigen::Matrix4d T_ir = T_ri.inverse();
    const Eigen::Matrix4d T_si = options.T_sr * T_ri;
    const Eigen::Matrix4d T_is = T_si.inverse();
    if (t_mid_s > 2 * delta_s) {
      for (int j = 0; j < 6; ++j) {
        // if (j == 0) {
        //   w(j, 0) = -ax * (t_mid_s - 2 * delta_s);
        //   dw(j, 0) = -ax;
        // } else {
        w(j, 0) = -v_amps[j] * std::sin(v_freqs[j] * (t_mid_s - 2 * delta_s) * (2 * M_PI));
        dw(j, 0) = (-v_amps[j] * v_freqs[j] * (2 * M_PI)) * std::cos(v_freqs[j] * (t_mid_s - 2 * delta_s) * (2 * M_PI));
        // }
      }
      w += options.x0.block<6, 1>(6, 0);
    }
    const Eigen::Vector3d v_ri_in_i = T_ir.block<3, 3>(0, 0) * -1 * w.block<3, 1>(0, 0);
    const Eigen::Vector3d w_si_in_s = options.T_sr.block<3, 3>(0, 0) * -1 * w.block<3, 1>(3, 0);
    const Eigen::Vector3d ypr = rot_to_yaw_pitch_roll(T_is.block<3, 3>(0, 0));
    // write pose to groundtruth file:
    lidar_pose_out << t_mid_us << "," << T_is(0, 3) << "," << T_is(1, 3) << "," << T_is(2, 3) << "," << v_ri_in_i(0, 0)
                   << "," << v_ri_in_i(1, 0) << "," << v_ri_in_i(2, 0) << "," << ypr(2, 0) << "," << ypr(1, 0) << ","
                   << ypr(0, 0) << "," << w_si_in_s(2, 0) << "," << w_si_in_s(1, 0) << "," << w_si_in_s(0, 0)
                   << std::endl;
    {               
      const uint64_t sec = t_mid_us / uint64_t(1000000);
      const std::string sec_str = std::to_string(sec);
      const uint64_t nsec = (t_mid_us % 1000000) * (1000);
      const std::string nsec_str = std::to_string(nsec);
      int n_zero = 9;
      const auto nsec_str2 = std::string(n_zero - std::min(n_zero, int(nsec_str.length())), '0') + nsec_str;
      const Eigen::Quaterniond q(Eigen::Matrix3d(T_is.block<3, 3>(0, 0)));
      lidar_pose_tum << sec_str << "." << nsec_str2 << " " << T_is(0, 3) << " " << T_is(1, 3) << " " << T_is(2, 3) << " " << q.x()
               << " " << q.y() << " " << q.z() << " " << q.w() << std::endl;
    }

    nav_msgs::msg::Odometry odometry;
    odometry.header.frame_id = "map";
    odometry.pose.pose = tf2::toMsg(Eigen::Affine3d(T_ir));
    odometry_publisher->publish(odometry);
    auto T_wr_msg = tf2::eigenToTransform(Eigen::Affine3d(T_ir));
    T_wr_msg.header.frame_id = "map";
    T_wr_msg.child_frame_id = "vehicle";
    tf_bc->sendTransform(T_wr_msg);

    if (min_diff_t_end_s != 0) {
      const int64_t dtns = tns + (delta_ns / 2) - t_end_min_ns;
      if (t_end_min_ns > 2 * delta_ns) {
        for (int j = 0; j < 6; ++j) {
          // if (j == 0) {
          //   w(j, 0) = -ax * (t_end_min_ns * 1.0e-9 - 2 * delta_s);
          //   dw(j, 0) = -ax;
          // } else {
          w(j, 0) = -v_amps[j] * std::sin(v_freqs[j] * (t_end_min_ns * 1.0e-9 - 2 * delta_s) * (2 * M_PI));
          dw(j, 0) = (-v_amps[j] * v_freqs[j] * (2 * M_PI)) *
                     std::cos(v_freqs[j] * (t_end_min_ns * 1.0e-9 - 2 * delta_s) * (2 * M_PI));
          // }
        }
        w += options.x0.block<6, 1>(6, 0);
      }
      double dtg = dtns * 1.0e-9;
      T_ri = lgmath::se3::Transformation(Eigen::Matrix<double, 6, 1>(w * dtg + 0.5 * dtg * dtg * dw)).matrix() *
             T_ri_end_min;
    } else {
      T_ri = T_ri_end_min;
    }
    tns += (delta_ns / 2);
    // T_ri = lgmath::se3::Transformation(Eigen::Matrix<double, 6, 1>(w * (delta_s / 2) + 0.5 * pow((delta_s / 2), 2) *
    // dw)).matrix() * T_ri; T_ri = lgmath::se3::Transformation(Eigen::Matrix<double, 6, 1>(w * dt2 + 0.5 * dw * dt2_2 +
    // (1 / 12) * lgmath::se3::curly_hat(dw) * w * dt2_3 + (1 / 240) * lgmath::se3::curly_hat(dw) *
    // lgmath::se3::curly_hat(dw) * w * dt2_5)).matrix() * T_ri;

    // w += (delta_s / 2) * dw;
    // ramp up the acceleration using constant jerk at the beginning
    // if (pc_index > 2) {
    //   const auto dw_desired = options.x0.block<6, 1>(12, 0);
    //   for (int dim = 0; dim < 6; ++dim) {
    //     if (fabs(dw(dim, 0)) < fabs(dw_desired(dim, 0))) {
    //       dw(dim, 0) += dw_desired(dim, 0) * ((delta_s / 2) / options.accel_ramp_time);
    //       if (dw_desired(dim, 0) <= 0) {
    //         dw(dim, 0) = std::max(dw(dim, 0), dw_desired(dim, 0));
    //       } else {
    //         dw(dim, 0) = std::min(dw(dim, 0), dw_desired(dim, 0));
    //       }
    //     }
    //   }
    // }

    sleep(options.sleep_delay);
    pc_index++;
  }

  lidar_pose_out.close();
  lidar_pose_tum.close();

  std::ofstream imu_raw_out(output_path / "applanix" / "imu_raw.csv", std::ios::out);
  std::ofstream imu_out(output_path / "applanix" / "imu.csv", std::ios::out);
  std::ofstream accel_raw_out(output_path / "applanix" / "accel_raw_minus_gravity.csv", std::ios::out);
  std::ofstream gps_out(output_path / "applanix" / "gps_post_process.csv", std::ios::out);
  imu_raw_out << std::setprecision(std::numeric_limits<long double>::digits10 + 1);
  imu_out << std::setprecision(std::numeric_limits<long double>::digits10 + 1);
  accel_raw_out << std::setprecision(std::numeric_limits<long double>::digits10 + 1);
  gps_out << std::setprecision(std::numeric_limits<long double>::digits10 + 1);
  // note: IMU raw needs to be in the body frame.
  imu_raw_out << "GPSTime,angvel_z,angvel_y,angvel_x,accelz,accely,accelx" << std::endl;
  accel_raw_out << "GPSTime,accelx,accely,accelz" << std::endl;
  gps_out << "GPSTime,easting,northing,altitude,vel_east,vel_north,vel_up,roll,pitch,heading,angvel_z,angvel_y,angvel_"
             "x,accelz,accely,accelx,latitude,longitude"
          << std::endl;

  Eigen::Matrix3d imu_body_raw_to_applanix, yfwd2xfwd, xfwd2yfwd;
  imu_body_raw_to_applanix << 0, -1, 0, -1, 0, 0, 0, 0, -1;
  yfwd2xfwd << 0, 1, 0, -1, 0, 0, 0, 0, 1;
  xfwd2yfwd = yfwd2xfwd.inverse();
  Eigen::Matrix4d T_robot_applanix = Eigen::Matrix4d::Identity();
  T_robot_applanix.block<3, 3>(0, 0) = yfwd2xfwd;
  const Eigen::Matrix3d C_robot_body = yfwd2xfwd * imu_body_raw_to_applanix;
  const Eigen::Matrix3d C_body_robot = C_robot_body.inverse();

  // Step through simulation for IMU measurements
  // Rotate gravity vector into the sensor frame.
  const uint64_t delta_imu_ns = 1000000000 / options.imu_rate;
  const double delta_imu_s = delta_imu_ns * 1.0e-9;
  const Eigen::Matrix3d C_ig = lgmath::so3::Rotation(options.xi_ig).matrix();
  LOG(INFO) << "C_ig: " << C_ig << std::endl;
  T_ri = lgmath::se3::Transformation(Eigen::Matrix<double, 6, 1>(options.x0.block<6, 1>(0, 0))).matrix();
  tns = 0;
  w = Eigen::Matrix<double, 6, 1>::Zero();
  dw = Eigen::Matrix<double, 6, 1>::Zero();

  T_ri = lgmath::se3::Transformation(
             Eigen::Matrix<double, 6, 1>(w * options.offset_imu + 0.5 * pow(options.offset_imu, 2) * dw))
             .matrix() *
         T_ri;

  const uint64_t t0_ns = t0_us * 1000;
  Eigen::Vector3d g;
  g << 0, 0, -9.8042;
  tns = options.offset_imu * 1.0e9;
  while (tns < sim_length_ns) {
    if (tns > 2 * delta_ns) {
      for (int j = 0; j < 6; ++j) {
        // if (j == 0) {
        //   w(j, 0) = -ax * (tns * 1.0e-9 - 2 * delta_s);
        //   dw(j, 0) = -ax;
        // } else {
        w(j, 0) = -v_amps[j] * std::sin(v_freqs[j] * (tns * 1.0e-9 - 2 * delta_s) * (2 * M_PI));
        dw(j, 0) =
            (-v_amps[j] * v_freqs[j] * (2 * M_PI)) * std::cos(v_freqs[j] * (tns * 1.0e-9 - 2 * delta_s) * (2 * M_PI));
        // }
      }
      w += options.x0.block<6, 1>(6, 0);
    }
    // simulate fake IMU measurements
    const double ts = (tns + t0_ns) * 1.0e-9;

    Eigen::Vector3d accel_raw = dw.block<3, 1>(0, 0) * -1;
    Eigen::Vector3d gyro_raw = w.block<3, 1>(3, 0) * -1;
    if (options.noisy_measurements) {
      for (int j = 0; j < 3; ++j) {
        accel_raw(j, 0) += n_accel(rg);
        gyro_raw(j, 0) += n_gyro(rg);
      }
    }
    for (int j = 0; j < 3; ++j) {
      accel_raw(j, 0) += options.biases[j];
      gyro_raw(j, 0) += options.biases[j + 3];
    }

    // accel raw without gravity (in robot frame)
    Eigen::Vector3d accel_body = accel_raw;
    accel_raw_out << ts << "," << accel_body(0, 0) << "," << accel_body(1, 0) << "," << accel_body(2, 0) << std::endl;
    // accel raw with gravity (in body frame)
    accel_body = C_body_robot * (accel_raw - T_ri.block<3, 3>(0, 0) * C_ig * g);
    Eigen::Vector3d gyro_body = C_body_robot * gyro_raw;
    imu_raw_out << ts << "," << gyro_body(2, 0) << "," << gyro_body(1, 0) << "," << gyro_body(0, 0) << ","
                << accel_body(2, 0) << "," << accel_body(1, 0) << "," << accel_body(0, 0) << std::endl;

    const Eigen::Vector3d accel_app = xfwd2yfwd * dw.block<3, 1>(0, 0) * -1;
    const Eigen::Vector3d gyro_app = xfwd2yfwd * w.block<3, 1>(3, 0) * -1;

    imu_out << ts << "," << gyro_app(2, 0) << "," << gyro_app(1, 0) << "," << gyro_app(0, 0) << "," << accel_app(2, 0)
            << "," << accel_app(1, 0) << "," << accel_app(0, 0) << std::endl;

    const Eigen::Matrix4d T_ir = T_ri.inverse();
    const Eigen::Vector3d v_ri_in_i = T_ir.block<3, 3>(0, 0) * -1 * w.block<3, 1>(0, 0);
    const Eigen::Matrix4d T_ia = T_ir * T_robot_applanix;
    const Eigen::Vector3d ypr = rot_to_yaw_pitch_roll(T_ia.block<3, 3>(0, 0));
    // write pose to groundtruth file:
    gps_out << ts << "," << T_ia(0, 3) << "," << T_ia(1, 3) << "," << T_ia(2, 3) << "," << v_ri_in_i(0, 0) << ","
            << v_ri_in_i(1, 0) << "," << v_ri_in_i(2, 0) << "," << ypr(2, 0) << "," << ypr(1, 0) << "," << ypr(0, 0)
            << "," << gyro_app(2, 0) << "," << gyro_app(1, 0) << "," << gyro_app(0, 0) << "," << accel_app(2, 0) << ","
            << accel_app(1, 0) << "," << accel_app(0, 0) << std::endl;

    T_ri = lgmath::se3::Transformation(Eigen::Matrix<double, 6, 1>(w * delta_imu_s + 0.5 * pow(delta_imu_s, 2) * dw))
               .matrix() *
           T_ri;
    // check orthogonality of C_ri:
    Eigen::Matrix3d C_ri = T_ri.block<3, 3>(0, 0);
    const double orthog_error = fabs((C_ri * C_ri.transpose()).squaredNorm() - 3.0);
    if (orthog_error > 1e-6) {
      // Reproject onto SO(3) if we stray too far
      LOG(INFO) << "reprojecting, error: " << orthog_error << std::endl;
      T_ri.block<3, 3>(0, 0) = Eigen::Matrix3d(Eigen::Matrix3d((C_ri * C_ri.transpose()).inverse().sqrt()) * C_ri);
    }

    // w += delta_imu_s * dw;

    // if (tns >= 3 * delta_ns) {
    //   const auto dw_desired = options.x0.block<6, 1>(12, 0);
    //   for (int dim = 0; dim < 6; ++dim) {
    //     if (fabs(dw(dim, 0)) < fabs(dw_desired(dim, 0))) {
    //       dw(dim, 0) += dw_desired(dim, 0) * (delta_imu_s / options.accel_ramp_time);
    //       if (dw_desired(dim, 0) <= 0) {
    //         dw(dim, 0) = std::max(dw(dim, 0), dw_desired(dim, 0));
    //       } else {
    //         dw(dim, 0) = std::min(dw(dim, 0), dw_desired(dim, 0));
    //       }
    //     }
    //   }
    // }
    tns += delta_imu_ns;
  }

  tns = 0;
  w = Eigen::Matrix<double, 6, 1>::Zero();
  dw = Eigen::Matrix<double, 6, 1>::Zero();
  T_ri = lgmath::se3::Transformation(Eigen::Matrix<double, 6, 1>(options.x0.block<6, 1>(0, 0))).matrix();
  const uint64_t delta_pose_ns = 1000000000 / options.pose_rate;
  const double delta_pose_s = delta_pose_ns * 1.0e-9;
  while (tns < sim_length_ns) {
    if (tns > 2 * delta_ns) {
      for (int j = 0; j < 6; ++j) {
        // if (j == 0) {
        //   w(j, 0) = -ax * (tns * 1.0e-9 - 2 * delta_s);
        //   dw(j, 0) = -ax;
        // } else {
        w(j, 0) = -v_amps[j] * std::sin(v_freqs[j] * (tns * 1.0e-9 - 2 * delta_s) * (2 * M_PI));
        dw(j, 0) =
            (-v_amps[j] * v_freqs[j] * (2 * M_PI)) * std::cos(v_freqs[j] * (tns * 1.0e-9 - 2 * delta_s) * (2 * M_PI));
        // }
      }
      w += options.x0.block<6, 1>(6, 0);
    }

    const double ts = (tns + t0_ns) * 1.0e-9;

    Eigen::Matrix4d T_si = options.T_sr * T_ri;

    if (options.noisy_measurements) {
      Eigen::Matrix<double, 6, 1> xi_noise;
      xi_noise << n_pose_trans(rg), n_pose_trans(rg), n_pose_trans(rg), n_pose_rot(rg), n_pose_rot(rg), n_pose_rot(rg);
      T_si = options.T_sr * lgmath::se3::Transformation(xi_noise).matrix() * T_ri;
    }

    lidar_pose_meas << ts << "," << T_si(0, 0) << "," << T_si(0, 1) << "," << T_si(0, 2) << "," << T_si(0, 3) << ","
                    << T_si(1, 0) << "," << T_si(1, 1) << "," << T_si(1, 2) << "," << T_si(1, 3) << "," << T_si(2, 0)
                    << "," << T_si(2, 1) << "," << T_si(2, 2) << "," << T_si(2, 3) << std::endl;

    T_ri = lgmath::se3::Transformation(Eigen::Matrix<double, 6, 1>(w * delta_pose_s + 0.5 * pow(delta_pose_s, 2) * dw))
               .matrix() *
           T_ri;

    // check orthogonality of C_ri:
    Eigen::Matrix3d C_ri = T_ri.block<3, 3>(0, 0);
    const double orthog_error = fabs((C_ri * C_ri.transpose()).squaredNorm() - 3.0);
    if (orthog_error > 1e-6) {
      // Reproject onto SO(3) if we stray too far
      LOG(INFO) << "reprojecting, error: " << orthog_error << std::endl;
      T_ri.block<3, 3>(0, 0) = Eigen::Matrix3d(Eigen::Matrix3d((C_ri * C_ri.transpose()).inverse().sqrt()) * C_ri);
    }

    tns += delta_pose_ns;
  }

  accel_raw_out.close();
  imu_raw_out.close();
  gps_out.close();
  imu_out.close();
  lidar_pose_meas.close();

  // [x]: parameters for singer prior
  // [x]: load VLS128 config parameters
  // [x]: load extrinsics from yaml files
  // [x]: step through simulation, generating pointclouds and IMU measurements
  // [x]: save pointclouds as .bin files and imu measurements in same formatted csv file
  // [ ]: add options for adding Gaussian noise to the lidar and IMU measurements

  return 0;
}