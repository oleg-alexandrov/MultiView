/* Copyright (c) 2021, United States Government, as represented by the
 * Administrator of the National Aeronautics and Space Administration.
 *
 * All rights reserved.
 *
 * The "ISAAC - Integrated System for Autonomous and Adaptive Caretaking
 * platform" software is licensed under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with the
 * License. You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 * License for the specific language governing permissions and limitations
 * under the License.
 */

// The algorithm:

// We assume our camera rig has n camera types. Each can be image or
// depth + image. Just one camera must be the reference camera. In
// this code that will be nav_cam.

// We assume we know the precise time every camera image is acquired.
// Every non-ref camera will be bracketed by two ref cameras very
// close in time. Hence, given the two bracketing ref camera poses,
// the ref cam pose will be interpolated at the time a non-ref camera
// is measured. This allows one to model the transform between
// the ref camera and every other camera on the rig.

// The variables to be optimized will be the pose of each ref camera,
// and the transforms from the ref camera to every other camera type
// (the extrinsics), with these transforms independent of time as the
// rig is rigid. Also optimized are the intrinsics of each camera, and
// the transform from each depth camera's cloud coordinates to its
// image coordinates (it is a transform very close to the identity but
// not quite, and a scale factor may be present).

// One component of the cost function to minimize measures the
// reprojection error in each camera, from each triangulated point in
// world coordinates. A second one measures the error between a
// triangulated point and corresponding depth measurement at that
// pixel, when applicable, with appropriate transforms applied to
// bring the depth measurement to world coordinates. This second
// error's strength is controlled by depth_tri_weight.

// Optionally, one can constrain that the triangulated points
// intersect close to a preexisting mesh, representing the surface
// being scanned with the rig given a previous estimation of all
// the camera poses. That mesh is computed using the geometry mapper.
// One also can control how close the depth camera clouds are to this
// mesh. The flags for this are mesh_tri_weight and depth_tri_weight,
// and can be set to 0 if desired not to use them.

// These mesh constraints bring in additional information,
// particularly for the cameras lacking depth, and help get the focal
// lengths correctly.

// If different camera sensors are on different CPUs, and a time
// offset exists among their clocks, this program can model that,
// and also float those offsets, if desired.

// The initial ref camera poses are computed using SfM, with the
// Astrobee build_map tool. The obtained "sparse map" of poses must
// be registered to world coordinates, to get the world scale correctly.
// The sparse map can be fixed or further refined in this tool.

// The initial extrinsics are assumed known, and are refined by this
// tool. Likely SfM can be used to get an initial value of the
// extrinsics, but for that may need to use the Theia tool which can
// do SfM with cameras acquired with different sensors.

// Every camera object (struct cameraImage) can look up its type,
// timestamp, timestamps and indices of bracketing cameras, image topic,
// depth topic (if present), ref_to_cam_timestamp_offset, and
// ref_to_cam_transform (extrinsics). A camera object also stores its
// image and depth cloud.

// For every instance of a reference camera its
// ref_to_cam_timestamp_offset is 0 and kept fixed,
// ref_to_cam_transform (extrinsics) is the identity and kept fixed,
// and the indices pointing to the left and right ref bracketing
// cameras are identical.

#include <ceres/ceres.h>
#include <ceres/rotation.h>
#include <ceres/problem.h>
#include <ceres/solver.h>
#include <ceres/cost_function.h>
#include <ceres/loss_function.h>
#include <ceres/dynamic_numeric_diff_cost_function.h>
#include <ceres/numeric_diff_cost_function.h>
#include <ceres/autodiff_cost_function.h>

#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/core/utility.hpp>

// Conditional dependence on Astrobee, ROS, Lua, protobuf, and
// SparseMap
#define HAVE_ASTROBEE 0
#if HAVE_ASTROBEE
#include <ff_common/utils.h>
#include <sparse_mapping/sparse_map.h>
#include <sparse_mapping/tensor.h>
#include <sparse_map_utils.h>
#include <dense_map_ros_utils.h>
#include <dense_map_lua_config.h>
#else
#include <rig_calibrator/dense_map_noros_utils.h>
#endif

#include <gflags/gflags.h>
#include <glog/logging.h>

#include <Eigen/Geometry>
#include <Eigen/Core>

#include <rig_calibrator/dense_map_utils.h>
#include <rig_calibrator/system_utils.h>
#include <rig_calibrator/interest_point.h>
#include <rig_calibrator/texture_processing.h>
#include <rig_calibrator/camera_image.h>

#include <string>
#include <map>
#include <iostream>
#include <fstream>

#if HAVE_ASTROBEE

DEFINE_string(ros_bag, "", "A ROS bag with recorded nav_cam, haz_cam intensity, "
              "full-resolution sci_cam, and haz_cam depth clouds.");

DEFINE_string(sparse_map, "",
              "A registered SURF sparse map made with some of the ROS bag data, "
              "including nav cam images closely bracketing the sci cam images.");

DEFINE_string(output_map, "", "Output file containing the updated map.");

DEFINE_string(nav_cam_topic, "/mgt/img_sampler/nav_cam/image_record",
              "The nav cam topic in the bag file.");

DEFINE_string(haz_cam_points_topic, "/hw/depth_haz/points",
              "The depth point cloud topic in the bag file.");

DEFINE_string(haz_cam_intensity_topic, "/hw/depth_haz/extended/amplitude_int",
              "The depth camera intensity topic in the bag file.");

DEFINE_string(sci_cam_topic, "/hw/cam_sci/compressed", "The sci cam topic in the bag file.");

#endif

DEFINE_int32(num_overlaps, 10, "How many images (of all camera types) close and forward in "
             "time to match to given image.");

DEFINE_double(max_haz_cam_image_to_depth_timestamp_diff, 0.2,
              "Use a haz cam depth cloud only if it is within this distance in time "
              "from the nearest haz cam intensity image.");

DEFINE_double(robust_threshold, 3.0,
              "Residual pixel errors and 3D point residuals (the latter multiplied "
              "by corresponding weight) much larger than this will be "
              "exponentially attenuated to affect less the cost function.\n");

DEFINE_int32(num_iterations, 20, "How many solver iterations to perform in calibration.");

DEFINE_double(bracket_len, 0.6,
              "Lookup sci and haz cam images only between consecutive nav cam images "
              "whose distance in time is no more than this (in seconds), after adjusting "
              "for the timestamp offset between these cameras. It is assumed the robot "
              "moves slowly and uniformly during this time. A large value here will "
              "make the refiner compute a poor solution but a small value will prevent "
              "enough sci_cam images being bracketed.");

DEFINE_string(nav_cam_intrinsics_to_float, "",
              "Refine given nav_cam intrinsics. Specify as a quoted list. "
              "For example: 'focal_length optical_center distortion'.");

DEFINE_string(haz_cam_intrinsics_to_float, "",
              "Refine given haz_cam intrinsics. Specify as a quoted list. "
              "For example: 'focal_length optical_center distortion'.");

DEFINE_string(sci_cam_intrinsics_to_float, "",
              "Refine given sci_cam intrinsics. Specify as a quoted list. "
              "For example: 'focal_length optical_center distortion'.");

DEFINE_string(extrinsics_to_float, "haz_cam sci_cam depth_to_image",
              "Specify the cameras whose extrinsics, relative to nav_cam, to optimize. Also "
              "consider if to float the haz_cam depth_to_image transform.");

DEFINE_bool(float_scale, false,
            "If to optimize the scale of the clouds, part of haz_cam depth_to_image transform "
            "(use it if the sparse map is kept fixed, or else rescaling and registration "
            "of the map and extrinsics is needed). This parameter should not be used with "
            "--affine_depth_to_image when the transform is affine, rather than rigid and a scale."
            "See also --extrinsics_to_float.");

DEFINE_bool(float_sparse_map, false,
            "Optimize the sparse map. This should be avoided as it can invalidate the scales "
            "of the extrinsics and the registration. It should at least be used with big mesh "
            "weights to attenuate those effects. See also: --registration.");

DEFINE_bool(float_timestamp_offsets, false,
            "If to optimize the timestamp offsets among the cameras.");

// TODO(oalexan1): This must be enabled for all sensor with a per-sensor exclusion
// distance.
DEFINE_int32(nav_cam_num_exclude_boundary_pixels, 0,
             "Flag as outliers nav cam pixels closer than this to the boundary, and ignore "
             "that boundary region when texturing with the --out_texture_dir option. "
             "This may improve the calibration accuracy, especially if switching from fisheye "
             "to radtan distortion for nav_cam. See also the geometry_mapper "
             "--undistorted_crop_wins option.");

DEFINE_double(timestamp_offsets_max_change, 1.0,
              "If floating the timestamp offsets, do not let them change by more than this "
              "(measured in seconds). Existing image bracketing acts as an additional constraint.");

DEFINE_double(nav_cam_to_sci_cam_offset_override_value,
              std::numeric_limits<double>::quiet_NaN(),
              "Override the value of nav_cam_to_sci_cam_timestamp_offset from the robot config "
              "file with this value.");

DEFINE_double(depth_tri_weight, 1000.0,
              "The weight to give to the constraint that depth measurements agree with "
              "triangulated points. Use a bigger number as depth errors are usually on the "
              "order of 0.01 meters while reprojection errors are on the order of 1 pixel.");

DEFINE_string(mesh, "",
              "Use this geometry mapper mesh from a previous geometry mapper run to help constrain "
              "the calibration (e.g., use fused_mesh.ply).");

DEFINE_double(mesh_tri_weight, 0.0,
              "A larger value will give more weight to the constraint that triangulated points "
              "stay close to a preexisting mesh. Not suggested by default.");

DEFINE_double(depth_mesh_weight, 0.0,
              "A larger value will give more weight to the constraint that the depth clouds "
              "stay close to the mesh. Not suggested by default.");

DEFINE_bool(affine_depth_to_image, false, "Assume that the depth_to_image_transform "
            "for each depth + image camera is an arbitrary affine transform rather than a "
            "rotation times a scale.");

DEFINE_int32(refiner_num_passes, 2, "How many passes of optimization to do. Outliers will be "
             "removed after every pass. Each pass will start with the previously optimized "
             "solution as an initial guess. Mesh intersections (if applicable) and ray "
             "triangulation will be recomputed before each pass.");

DEFINE_double(initial_max_reprojection_error, 300.0, "If filtering outliers, remove interest "
              "points for which the reprojection error, in pixels, is larger than this. This "
              "filtering happens when matches are created, before cameras are optimized, and "
              "a big value should be used if the initial cameras are not trusted.");

DEFINE_double(max_reprojection_error, 25.0, "If filtering outliers, remove interest points for "
              "which the reprojection error, in pixels, is larger than this. This filtering "
              "happens after each optimization pass finishes, unless disabled. It is better to not "
              "filter too aggressively unless confident of the solution.");

DEFINE_double(refiner_min_angle, 0.5, "If filtering outliers, remove triangulated points "
              "for which all rays converging to it make an angle (in degrees) less than this."
              "Note that some cameras in the rig may be very close to each other relative to "
              "the triangulated points, so care is needed here.");

DEFINE_string(out_texture_dir, "", "If non-empty and if an input mesh was provided, "
              "project the camera images using the optimized poses onto the mesh "
              "and write the obtained .obj files in the given directory.");

DEFINE_double(min_ray_dist, 0.0, "The minimum search distance from a starting point along a ray "
              "when intersecting the ray with a mesh, in meters (if applicable).");

DEFINE_double(max_ray_dist, 100.0, "The maximum search distance from a starting point along a ray "
              "when intersecting the ray with a mesh, in meters (if applicable).");

DEFINE_bool(registration, false,
            "If true, and registration control points for the sparse map exist and are specified "
            "by --hugin_file and --xyz_file, re-register the sparse map at the end. All the "
            "extrinsics, including depth_to_image_transform, will be scaled accordingly."
            "This is not needed if the nav_cam intrinsics and the sparse map do not change.");

DEFINE_string(hugin_file, "", "The path to the hugin .pto file used for sparse map registration.");

DEFINE_string(xyz_file, "", "The path to the xyz file used for sparse map registration.");

DEFINE_double(parameter_tolerance, 1e-12, "Stop when the optimization variables change by "
              "less than this.");

DEFINE_int32(num_opt_threads, 16, "How many threads to use in the optimization.");

DEFINE_int32(num_match_threads, 8, "How many threads to use in feature detection/matching. "
             "A large number can use a lot of memory.");
DEFINE_string(sci_cam_timestamps, "",
              "Use only these sci cam timestamps. Must be a file with one timestamp per line.");

DEFINE_bool(no_extrinsics, false,
            "Do not model the extrinsics between cameras. Hence the pose of any "
            "camera of any type may vary on its own with no restriction that the cameras "
            "be on a rig. Model however depth_to_image_transform. See also "
            "--float_nonref_cameras.");
DEFINE_bool(float_nonref_cameras, false, "To use in conjunction with --no_extrinsics "
            "to float non-reference camera poses. Use --float_sparse map to float the "
            "reference cameras.");

DEFINE_string(out_dir, "",
              "Save in this directory the camera intrinsics and extrinsics. "
              "See also --save-images_and_depth_clouds, --save-matches, --verbose, and --in_dir.");

DEFINE_bool(save_images_and_depth_clouds, false,
            "Save the images and point clouds used in processing. Implies that --out_dir is set.");

DEFINE_string(rig_config, "",
              "Read the rig configuration from this plain text file rather than from "
              "robot's lua-based config file. The same format is used as for when this tool "
              "saves the outputs with the --save_images_and_depth_clouds option.");

DEFINE_string(image_list, "",
              "Read images and camera poses from this list, rather than a sparse map and "
              "bag file. The same format is used as for when this tool saves the outputs "
              "with the --save_images_and_depth_clouds option.");

DEFINE_bool(save_matches, false,
            "Save the interest point matches. Stereo Pipeline's viewer can be used for "
            "visualizing these. Implies that --out_dir is set.");

DEFINE_bool(verbose, false,
            "Print a lot of verbose information about how matching goes.");

namespace dense_map {

// Calculate interpolated world to camera transform. Use the
// convention that if beg_ref_stamp == end_ref_stamp, then this is the
// reference camera, and then only beg_world_to_ref_t is used, while
// end_world_to_ref_t is undefined. For the reference camera it is
// also expected that ref_to_cam_aff is the identity. This saves some
// code duplication later as the ref cam need not be treated
// separately.
Eigen::Affine3d calc_world_to_cam_trans(const double* beg_world_to_ref_t,
                                        const double* end_world_to_ref_t,
                                        const double* ref_to_cam_trans,
                                        double beg_ref_stamp,
                                        double end_ref_stamp,
                                        double ref_to_cam_offset,
                                        double cam_stamp) {
  Eigen::Affine3d beg_world_to_ref_aff;
  array_to_rigid_transform(beg_world_to_ref_aff, beg_world_to_ref_t);

  Eigen::Affine3d interp_world_to_cam_aff;
  if (beg_ref_stamp == end_ref_stamp) {
    interp_world_to_cam_aff = beg_world_to_ref_aff;
  } else {
    Eigen::Affine3d end_world_to_ref_aff;
    array_to_rigid_transform(end_world_to_ref_aff, end_world_to_ref_t);

    Eigen::Affine3d ref_to_cam_aff;
    array_to_rigid_transform(ref_to_cam_aff, ref_to_cam_trans);

    // Covert from cam time to ref time and normalize. It is very
    // important that below we subtract the big numbers from each
    // other first, which are the timestamps, then subtract whatever
    // else is necessary. Otherwise we get problems with numerical
    // precision with CERES.
    // Note how the denominator never becomes 0.
    double alpha = ((cam_stamp - beg_ref_stamp) - ref_to_cam_offset)
      / (end_ref_stamp - beg_ref_stamp);

    if (alpha < 0.0 || alpha > 1.0) LOG(FATAL) << "Out of bounds in interpolation.\n";

    // Interpolate at desired time
    Eigen::Affine3d interp_world_to_ref_aff = dense_map::linearInterp(alpha, beg_world_to_ref_aff,
                                                                      end_world_to_ref_aff);

    interp_world_to_cam_aff = ref_to_cam_aff * interp_world_to_ref_aff;
  }

  return interp_world_to_cam_aff;
}

ceres::LossFunction* GetLossFunction(std::string cost_fun, double th) {
  // Convert to lower-case
  std::transform(cost_fun.begin(), cost_fun.end(), cost_fun.begin(), ::tolower);

  ceres::LossFunction* loss_function = NULL;
  if (cost_fun == "l2")
    loss_function = NULL;
  else if (cost_fun == "huber")
    loss_function = new ceres::HuberLoss(th);
  else if (cost_fun == "cauchy")
    loss_function = new ceres::CauchyLoss(th);
  else if (cost_fun == "l1")
    loss_function = new ceres::SoftLOneLoss(th);
  else
    LOG(FATAL) << "Unknown cost function: " + cost_fun;

  return loss_function;
}

// An error function minimizing the error of projecting
// an xyz point into a camera that is bracketed by
// two reference cameras. The precise timestamp offset
// between them is also floated.
struct BracketedCamError {
  EIGEN_MAKE_ALIGNED_OPERATOR_NEW
  BracketedCamError(Eigen::Vector2d const& meas_dist_pix,
                    double left_ref_stamp, double right_ref_stamp, double cam_stamp,
                    std::vector<int> const& block_sizes,
                    camera::CameraParameters const& cam_params):
    m_meas_dist_pix(meas_dist_pix),
    m_left_ref_stamp(left_ref_stamp),
    m_right_ref_stamp(right_ref_stamp),
    m_cam_stamp(cam_stamp),
    m_block_sizes(block_sizes),
    m_cam_params(cam_params),
    m_num_focal_lengths(1) {
    // Sanity check
    if (m_block_sizes.size() != 8 || m_block_sizes[0] != NUM_RIGID_PARAMS ||
        m_block_sizes[1] != NUM_RIGID_PARAMS || m_block_sizes[2] != NUM_RIGID_PARAMS ||
        m_block_sizes[3] != NUM_XYZ_PARAMS || m_block_sizes[4] != NUM_SCALAR_PARAMS ||
        m_block_sizes[5] != m_num_focal_lengths || m_block_sizes[6] != NUM_OPT_CTR_PARAMS ||
        m_block_sizes[7] != 1  // This will be overwritten shortly
    ) {
      LOG(FATAL) << "BracketedCamError: The block sizes were not set up properly.\n";
    }

    // Set the correct distortion size. This cannot be done in the interface for now.
    m_block_sizes[7] = m_cam_params.GetDistortion().size();
  }

  // Call to work with ceres::DynamicNumericDiffCostFunction.
  bool operator()(double const* const* parameters, double* residuals) const {
    Eigen::Affine3d world_to_cam_trans =
      calc_world_to_cam_trans(parameters[0],  // beg_world_to_ref_t
                              parameters[1],  // end_world_to_ref_t
                              parameters[2],  // ref_to_cam_trans
                              m_left_ref_stamp, m_right_ref_stamp,
                              parameters[4][0],  // ref_to_cam_offset
                              m_cam_stamp);

    // World point
    Eigen::Vector3d X(parameters[3][0], parameters[3][1], parameters[3][2]);

    // Make a deep copy which we will modify
    camera::CameraParameters cam_params = m_cam_params;
    Eigen::Vector2d focal_vector = Eigen::Vector2d(parameters[5][0], parameters[5][0]);
    Eigen::Vector2d optical_center(parameters[6][0], parameters[6][1]);
    Eigen::VectorXd distortion(m_block_sizes[7]);
    for (int i = 0; i < m_block_sizes[7]; i++) distortion[i] = parameters[7][i];
    cam_params.SetFocalLength(focal_vector);
    cam_params.SetOpticalOffset(optical_center);
    cam_params.SetDistortion(distortion);

    // Convert world point to given cam coordinates
    X = world_to_cam_trans * X;

    // Project into the image
    Eigen::Vector2d undist_pix = cam_params.GetFocalVector().cwiseProduct(X.hnormalized());
    Eigen::Vector2d curr_dist_pix;
    cam_params.Convert<camera::UNDISTORTED_C, camera::DISTORTED>(undist_pix, &curr_dist_pix);

    // Compute the residuals
    residuals[0] = curr_dist_pix[0] - m_meas_dist_pix[0];
    residuals[1] = curr_dist_pix[1] - m_meas_dist_pix[1];

    return true;
  }

  // Factory to hide the construction of the CostFunction object from the client code.
  static ceres::CostFunction*
  Create(Eigen::Vector2d const& meas_dist_pix, double left_ref_stamp, double right_ref_stamp,
         double cam_stamp, std::vector<int> const& block_sizes,
         camera::CameraParameters const& cam_params) {
    ceres::DynamicNumericDiffCostFunction<BracketedCamError>* cost_function =
      new ceres::DynamicNumericDiffCostFunction<BracketedCamError>
      (new BracketedCamError(meas_dist_pix, left_ref_stamp, right_ref_stamp,
                             cam_stamp, block_sizes, cam_params));

    cost_function->SetNumResiduals(NUM_PIX_PARAMS);

    // The camera wrapper knows all of the block sizes to add, except
    // for distortion, which is last
    for (size_t i = 0; i + 1 < block_sizes.size(); i++)  // note the i + 1
      cost_function->AddParameterBlock(block_sizes[i]);

    // The distortion block size is added separately as it is variable
    cost_function->AddParameterBlock(cam_params.GetDistortion().size());

    return cost_function;
  }

 private:
  Eigen::Vector2d m_meas_dist_pix;             // Measured distorted current camera pixel
  double m_left_ref_stamp, m_right_ref_stamp;  // left and right ref cam timestamps
  double m_cam_stamp;                          // Current cam timestamp
  std::vector<int> m_block_sizes;
  camera::CameraParameters m_cam_params;
  int m_num_focal_lengths;
};  // End class BracketedCamError

// An error function minimizing the product of a given weight and the
// error between a triangulated point and a measured depth point. The
// depth point needs to be transformed to world coordinates first. For
// that one has to do pose interpolation.
struct BracketedDepthError {
  EIGEN_MAKE_ALIGNED_OPERATOR_NEW
  BracketedDepthError(double weight, Eigen::Vector3d const& meas_depth_xyz,
                      double left_ref_stamp, double right_ref_stamp, double cam_stamp,
                      std::vector<int> const& block_sizes):
    m_weight(weight),
    m_meas_depth_xyz(meas_depth_xyz),
    m_left_ref_stamp(left_ref_stamp),
    m_right_ref_stamp(right_ref_stamp),
    m_cam_stamp(cam_stamp),
    m_block_sizes(block_sizes) {
    // Sanity check
    if (m_block_sizes.size() != 7 ||
        m_block_sizes[0] != NUM_RIGID_PARAMS  ||
        m_block_sizes[1] != NUM_RIGID_PARAMS  ||
        m_block_sizes[2] != NUM_RIGID_PARAMS  ||
        (m_block_sizes[3] != NUM_RIGID_PARAMS  && m_block_sizes[3] != NUM_AFFINE_PARAMS) ||
        m_block_sizes[4] != NUM_SCALAR_PARAMS ||
        m_block_sizes[5] != NUM_XYZ_PARAMS    ||
        m_block_sizes[6] != NUM_SCALAR_PARAMS) {
      LOG(FATAL) << "BracketedDepthError: The block sizes were not set up properly.\n";
    }
  }

  // Call to work with ceres::DynamicNumericDiffCostFunction.
  bool operator()(double const* const* parameters, double* residuals) const {
    // Current world to camera transform
    Eigen::Affine3d world_to_cam_trans =
      calc_world_to_cam_trans(parameters[0],  // beg_world_to_ref_t
                              parameters[1],  // end_world_to_ref_t
                              parameters[2],  // ref_to_cam_trans
                              m_left_ref_stamp, m_right_ref_stamp,
                              parameters[6][0],  // ref_to_cam_offset
                              m_cam_stamp);

    // The current transform from the depth point cloud to the camera image
    Eigen::Affine3d depth_to_image;
    if (m_block_sizes[3] == NUM_AFFINE_PARAMS)
      array_to_affine_transform(depth_to_image, parameters[3]);
    else
      array_to_rigid_transform(depth_to_image, parameters[3]);

    // Apply the scale
    double depth_to_image_scale = parameters[4][0];
    depth_to_image.linear() *= depth_to_image_scale;

    // Convert from depth cloud coordinates to cam coordinates
    Eigen::Vector3d M = depth_to_image * m_meas_depth_xyz;

    // Convert to world coordinates
    M = world_to_cam_trans.inverse() * M;

    // Triangulated world point
    Eigen::Vector3d X(parameters[5][0], parameters[5][1], parameters[5][2]);

    // Compute the residuals
    for (size_t it = 0; it < NUM_XYZ_PARAMS; it++) {
      residuals[it] = m_weight * (X[it] - M[it]);
    }

    return true;
  }

  // Factory to hide the construction of the CostFunction object from the client code.
  static ceres::CostFunction* Create(double weight, Eigen::Vector3d const& meas_depth_xyz,
                                     double left_ref_stamp, double right_ref_stamp,
                                     double cam_stamp, std::vector<int> const& block_sizes) {
    ceres::DynamicNumericDiffCostFunction<BracketedDepthError>* cost_function =
      new ceres::DynamicNumericDiffCostFunction<BracketedDepthError>
      (new BracketedDepthError(weight, meas_depth_xyz, left_ref_stamp, right_ref_stamp,
                             cam_stamp, block_sizes));

    // The residual size is always the same.
    cost_function->SetNumResiduals(NUM_XYZ_PARAMS);

    for (size_t i = 0; i < block_sizes.size(); i++)
      cost_function->AddParameterBlock(block_sizes[i]);

    return cost_function;
  }

 private:
  double m_weight;                             // How much weight to give to this constraint
  Eigen::Vector3d m_meas_depth_xyz;            // Measured depth measurement
  double m_left_ref_stamp, m_right_ref_stamp;  // left and right ref cam timestamps
  double m_cam_stamp;                          // Current cam timestamp
  std::vector<int> m_block_sizes;
};  // End class BracketedDepthError

// An error function minimizing the product of a given weight and the
// error between a mesh point and a transformed measured depth point. The
// depth point needs to be transformed to world coordinates first. For
// that one has to do pose interpolation.
struct BracketedDepthMeshError {
  EIGEN_MAKE_ALIGNED_OPERATOR_NEW
  BracketedDepthMeshError(double weight,
                          Eigen::Vector3d const& meas_depth_xyz,
                          Eigen::Vector3d const& mesh_xyz,
                          double left_ref_stamp, double right_ref_stamp, double cam_stamp,
                          std::vector<int> const& block_sizes):
    m_weight(weight),
    m_meas_depth_xyz(meas_depth_xyz),
    m_mesh_xyz(mesh_xyz),
    m_left_ref_stamp(left_ref_stamp),
    m_right_ref_stamp(right_ref_stamp),
    m_cam_stamp(cam_stamp),
    m_block_sizes(block_sizes) {
    // Sanity check
    if (m_block_sizes.size() != 6 ||
        m_block_sizes[0] != NUM_RIGID_PARAMS  ||
        m_block_sizes[1] != NUM_RIGID_PARAMS  ||
        m_block_sizes[2] != NUM_RIGID_PARAMS  ||
        (m_block_sizes[3] != NUM_RIGID_PARAMS  && m_block_sizes[3] != NUM_AFFINE_PARAMS) ||
        m_block_sizes[4] != NUM_SCALAR_PARAMS ||
        m_block_sizes[5] != NUM_SCALAR_PARAMS) {
      LOG(FATAL) << "BracketedDepthMeshError: The block sizes were not set up properly.\n";
    }
  }

  // Call to work with ceres::DynamicNumericDiffCostFunction.
  bool operator()(double const* const* parameters, double* residuals) const {
    // Current world to camera transform
    Eigen::Affine3d world_to_cam_trans =
      calc_world_to_cam_trans(parameters[0],  // beg_world_to_ref_t
                              parameters[1],  // end_world_to_ref_t
                              parameters[2],  // ref_to_cam_trans
                              m_left_ref_stamp, m_right_ref_stamp,
                              parameters[5][0],  // ref_to_cam_offset
                              m_cam_stamp);

    // The current transform from the depth point cloud to the camera image
    Eigen::Affine3d depth_to_image;
    if (m_block_sizes[3] == NUM_AFFINE_PARAMS)
      array_to_affine_transform(depth_to_image, parameters[3]);
    else
      array_to_rigid_transform(depth_to_image, parameters[3]);

    // Apply the scale
    double depth_to_image_scale = parameters[4][0];
    depth_to_image.linear() *= depth_to_image_scale;

    // Convert from depth cloud coordinates to cam coordinates
    Eigen::Vector3d M = depth_to_image * m_meas_depth_xyz;

    // Convert to world coordinates
    M = world_to_cam_trans.inverse() * M;

    // Compute the residuals
    for (size_t it = 0; it < NUM_XYZ_PARAMS; it++) {
      residuals[it] = m_weight * (m_mesh_xyz[it] - M[it]);
    }

    return true;
  }

  // Factory to hide the construction of the CostFunction object from the client code.
  static ceres::CostFunction* Create(double weight,
                                     Eigen::Vector3d const& meas_depth_xyz,
                                     Eigen::Vector3d const& mesh_xyz,
                                     double left_ref_stamp, double right_ref_stamp,
                                     double cam_stamp, std::vector<int> const& block_sizes) {
    ceres::DynamicNumericDiffCostFunction<BracketedDepthMeshError>* cost_function =
      new ceres::DynamicNumericDiffCostFunction<BracketedDepthMeshError>
      (new BracketedDepthMeshError(weight, meas_depth_xyz, mesh_xyz,
                                   left_ref_stamp, right_ref_stamp,
                                   cam_stamp, block_sizes));

    // The residual size is always the same.
    cost_function->SetNumResiduals(NUM_XYZ_PARAMS);

    for (size_t i = 0; i < block_sizes.size(); i++)
      cost_function->AddParameterBlock(block_sizes[i]);

    return cost_function;
  }

 private:
  double m_weight;                             // How much weight to give to this constraint
  Eigen::Vector3d m_meas_depth_xyz;            // Measured depth measurement
  Eigen::Vector3d m_mesh_xyz;                  // Point on preexisting mesh
  double m_left_ref_stamp, m_right_ref_stamp;  // left and right ref cam timestamps
  double m_cam_stamp;                          // Current cam timestamp
  std::vector<int> m_block_sizes;
};  // End class BracketedDepthMeshError

// An error function minimizing a weight times the distance from a
// variable xyz point to a fixed reference xyz point.
struct XYZError {
  EIGEN_MAKE_ALIGNED_OPERATOR_NEW
  XYZError(Eigen::Vector3d const& ref_xyz, std::vector<int> const& block_sizes, double weight)
      : m_ref_xyz(ref_xyz), m_block_sizes(block_sizes), m_weight(weight) {
    // Sanity check
    if (m_block_sizes.size() != 1 || m_block_sizes[0] != NUM_XYZ_PARAMS)
      LOG(FATAL) << "XYZError: The block sizes were not set up properly.\n";
  }

  // Call to work with ceres::DynamicNumericDiffCostFunction.
  // Takes array of arrays as parameters.
  // TODO(oalexan1): May want to use the analytical Ceres cost function
  bool operator()(double const* const* parameters, double* residuals) const {
    // Compute the residuals
    for (int it = 0; it < NUM_XYZ_PARAMS; it++)
      residuals[it] = m_weight * (parameters[0][it] - m_ref_xyz[it]);

    return true;
  }

  // Factory to hide the construction of the CostFunction object from the client code.
  static ceres::CostFunction* Create(Eigen::Vector3d const& ref_xyz,
                                     std::vector<int> const& block_sizes,
                                     double weight) {
    ceres::DynamicNumericDiffCostFunction<XYZError>* cost_function =
      new ceres::DynamicNumericDiffCostFunction<XYZError>
      (new XYZError(ref_xyz, block_sizes, weight));

    // The residual size is always the same
    cost_function->SetNumResiduals(NUM_XYZ_PARAMS);

    // The camera wrapper knows all of the block sizes to add.
    for (size_t i = 0; i < block_sizes.size(); i++) {
      cost_function->AddParameterBlock(block_sizes[i]);
    }
    return cost_function;
  }

 private:
  Eigen::Vector3d m_ref_xyz;  // reference xyz
  std::vector<int> m_block_sizes;
  double m_weight;
};  // End class XYZError

void calc_residuals_stats(std::vector<double> const& residuals,
                          std::vector<std::string> const& residual_names,
                          std::string const& tag) {
  size_t num = residuals.size();

  if (num != residual_names.size())
    LOG(FATAL) << "There must be as many residuals as residual names.";

  std::map<std::string, std::vector<double>> stats;
  for (size_t it = 0; it < residuals.size(); it++)
    stats[residual_names[it]] = std::vector<double>();  // initialize

  for (size_t it = 0; it < residuals.size(); it++)
    stats[residual_names[it]].push_back(std::abs(residuals[it]));

  std::cout << "The 25, 50, 75, and 100th percentile residual stats " << tag << std::endl;
  for (auto it = stats.begin(); it != stats.end(); it++) {
    std::string const& name = it->first;
    std::vector<double> vals = stats[name];  // make a copy
    std::sort(vals.begin(), vals.end());

    int len = vals.size();

    int it1 = static_cast<int>(0.25 * len);
    int it2 = static_cast<int>(0.50 * len);
    int it3 = static_cast<int>(0.75 * len);
    int it4 = static_cast<int>(len - 1);

    if (len == 0)
      std::cout << name << ": " << "none";
    else
      std::cout << std::setprecision(5)
                << name << ": " << vals[it1] << ' ' << vals[it2] << ' '
                << vals[it3] << ' ' << vals[it4];
    std::cout << " (" << len << " residuals)" << std::endl;
  }
}

// Sort by timestamps adjusted to be relative to the ref camera clock
bool timestampLess(cameraImage i, cameraImage j) {
  return (i.ref_timestamp < j.ref_timestamp);
}

// Find the haz cam depth measurement. Use nearest neighbor interpolation
// to look into the depth cloud.
bool depthValue(  // Inputs
                cv::Mat const& depth_cloud, Eigen::Vector2d const& dist_ip,
                // Output
                Eigen::Vector3d& depth_xyz) {
  depth_xyz = Eigen::Vector3d(0, 0, 0);  // initialize

  if (depth_cloud.cols == 0 && depth_cloud.rows == 0) return false;  // empty cloud

  int col = round(dist_ip[0]);
  int row = round(dist_ip[1]);

  if (col < 0 || row < 0 || col > depth_cloud.cols || row > depth_cloud.rows)
    LOG(FATAL) << "Out of range in depth cloud.";

  // After rounding one may hit the bound
  if (col == depth_cloud.cols || row == depth_cloud.rows)
    return false;

  cv::Vec3f cv_depth_xyz = depth_cloud.at<cv::Vec3f>(row, col);

  // Skip invalid measurements
  if (cv_depth_xyz == cv::Vec3f(0, 0, 0))
    return false;

  depth_xyz = Eigen::Vector3d(cv_depth_xyz[0], cv_depth_xyz[1], cv_depth_xyz[2]);

  return true;
}

// Project given images with optimized cameras onto mesh. It is
// assumed that the most up-to-date cameras were copied/interpolated
// form the optimizer structures into the world_to_cam vector.
void meshProjectCameras(std::vector<std::string> const& cam_names,
                        std::vector<camera::CameraParameters> const& cam_params,
                        std::vector<dense_map::cameraImage> const& cam_images,
                        std::vector<Eigen::Affine3d> const& world_to_cam,
                        mve::TriangleMesh::Ptr const& mesh,
                        std::shared_ptr<BVHTree> const& bvh_tree,
                        int ref_camera_type,
                        int nav_cam_num_exclude_boundary_pixels,
                        std::string const& out_dir) {
  if (cam_names.size() != cam_params.size())
    LOG(FATAL) << "There must be as many camera names as sets of camera parameters.\n";
  if (cam_images.size() != world_to_cam.size())
    LOG(FATAL) << "There must be as many camera images as camera poses.\n";
  if (out_dir.empty())
    LOG(FATAL) << "The output directory is empty.\n";

  char filename_buffer[1000];

  for (size_t cid = 0; cid < cam_images.size(); cid++) {
    double timestamp = cam_images[cid].timestamp;
    int cam_type = cam_images[cid].camera_type;

    int num_exclude_boundary_pixels = 0;
    if (cam_type == ref_camera_type)
      num_exclude_boundary_pixels = nav_cam_num_exclude_boundary_pixels;

    // Must use the 10.7f format for the timestamp as everywhere else in the code
    snprintf(filename_buffer, sizeof(filename_buffer), "%s/%10.7f_%s",
             out_dir.c_str(), timestamp, cam_names[cam_type].c_str());
    std::string out_prefix = filename_buffer;  // convert to string

    std::cout << "Creating texture for: " << out_prefix << std::endl;
    meshProject(mesh, bvh_tree, cam_images[cid].image, world_to_cam[cid], cam_params[cam_type],
                num_exclude_boundary_pixels, out_prefix);
  }
}

// Compute the transforms from the world to every camera, using pose interpolation
// if necessary.
void calc_world_to_cam_transforms(  // Inputs
                                  std::vector<dense_map::cameraImage> const& cams,
                                  std::vector<double> const& world_to_ref_vec,
                                  std::vector<double> const& ref_timestamps,
                                  std::vector<double> const& ref_to_cam_vec,
                                  std::vector<double> const& ref_to_cam_timestamp_offsets,
                                  // Output
                                  std::vector<Eigen::Affine3d>& world_to_cam) {
  if (ref_to_cam_vec.size() / dense_map::NUM_RIGID_PARAMS != ref_to_cam_timestamp_offsets.size())
    LOG(FATAL) << "Must have as many transforms to reference as timestamp offsets.\n";
  if (world_to_ref_vec.size() / dense_map::NUM_RIGID_PARAMS != ref_timestamps.size())
    LOG(FATAL) << "Must have as many reference timestamps as reference cameras.\n";

  world_to_cam.resize(cams.size());

  for (size_t it = 0; it < cams.size(); it++) {
    int beg_index = cams[it].beg_ref_index;
    int end_index = cams[it].end_ref_index;
    int cam_type = cams[it].camera_type;
    world_to_cam[it] = dense_map::calc_world_to_cam_trans
      (&world_to_ref_vec[dense_map::NUM_RIGID_PARAMS * beg_index],
       &world_to_ref_vec[dense_map::NUM_RIGID_PARAMS * end_index],
       &ref_to_cam_vec[dense_map::NUM_RIGID_PARAMS * cam_type],
       ref_timestamps[beg_index], ref_timestamps[end_index],
       ref_to_cam_timestamp_offsets[cam_type],
       cams[it].timestamp);
  }
  return;
}

// Calculate world_to_cam transforms from their representation in a
// vector, rather than using reference cameras, extrinsics and
// timestamp interpolation. Only for use with --no_extrinsics, when
// each camera varies independently.
void calc_world_to_cam_no_extrinsics(  // Inputs
  std::vector<dense_map::cameraImage> const& cams, std::vector<double> const& world_to_cam_vec,
  // Output
  std::vector<Eigen::Affine3d>& world_to_cam) {
  if (world_to_cam_vec.size() != cams.size() * dense_map::NUM_RIGID_PARAMS)
    LOG(FATAL) << "Incorrect size for world_to_cam_vec.\n";

  for (size_t cid = 0; cid < cams.size(); cid++)
    dense_map::array_to_rigid_transform(world_to_cam[cid],  // output
                                        &world_to_cam_vec[dense_map::NUM_RIGID_PARAMS * cid]);
}

// Use one of the two implementations above. Care is needed as when there are no extrinsics,
// each camera is on its own, so the input is in world_to_cam_vec and not in world_to_ref_vec
void calc_world_to_cam_transforms(  // Inputs
  bool no_extrinsics, std::vector<dense_map::cameraImage> const& cams,
  std::vector<double> const& world_to_ref_vec, std::vector<double> const& ref_timestamps,
  std::vector<double> const& ref_to_cam_vec, std::vector<double> const& world_to_cam_vec,
  std::vector<double> const& ref_to_cam_timestamp_offsets,
  // Output
  std::vector<Eigen::Affine3d>& world_to_cam) {
  if (!no_extrinsics)
    calc_world_to_cam_transforms(  // Inputs
                                 cams, world_to_ref_vec, ref_timestamps, ref_to_cam_vec,
                                 ref_to_cam_timestamp_offsets,
                                 // Output
                                 world_to_cam);
  else
    calc_world_to_cam_no_extrinsics(  // Inputs
      cams, world_to_cam_vec,
      // Output
      world_to_cam);

  return;
}

// Look up a map value and throw an error when not found
template<class A, class B>
B mapVal(std::map<A, B> const& map, A const& a) {
  auto it = map.find(a);
  if (it == map.end())
    LOG(FATAL) << "Cannot look up expected map value.\n";

  return it->second;
}

// Get a map value while being const-correct and also checking that the value exists.
template <class T>
T getMapValue(std::vector<std::map<int, std::map<int, T>>> const& pid_cid_fid, size_t pid, int cid,
              int fid) {
  if (pid_cid_fid.size() <= pid)
    LOG(FATAL) << "Current pid is out of range.\n";

  auto& cid_fid_map = pid_cid_fid[pid];  // alias
  auto cid_it = cid_fid_map.find(cid);
  if (cid_it == cid_fid_map.end()) LOG(FATAL) << "Current cid it out of range.\n";

  auto& fid_map = cid_it->second;  // alias
  auto fid_it = fid_map.find(fid);
  if (fid_it == fid_map.end()) LOG(FATAL) << "Current fid is out of range.\n";

  return fid_it->second;
}

// Set a map value while taking care that the place for it exists.
void setMapValue(std::vector<std::map<int, std::map<int, int>>> & pid_cid_fid,
                 size_t pid, int cid, int fid, int val) {
  if (pid_cid_fid.size() <= pid)
    LOG(FATAL) << "Current pid is out of range.\n";

  auto& cid_fid_map = pid_cid_fid[pid];  // alias
  auto cid_it = cid_fid_map.find(cid);
  if (cid_it == cid_fid_map.end()) LOG(FATAL) << "Current cid it out of range.\n";

  auto& fid_map = cid_it->second;  // alias
  auto fid_it = fid_map.find(fid);
  if (fid_it == fid_map.end()) LOG(FATAL) << "Current fid is out of range.\n";

  fid_it->second = val;
}

void parameterValidation() {
#if HAVE_ASTROBEE

  if (FLAGS_ros_bag.empty())
    LOG(FATAL) << "The bag file was not specified.";
  if (FLAGS_sparse_map.empty())
    LOG(FATAL) << "The input sparse map was not specified.";

  if (FLAGS_output_map.empty())
    LOG(FATAL) << "The output sparse map was not specified.";

#endif

  if (FLAGS_robust_threshold <= 0.0)
    LOG(FATAL) << "The robust threshold must be positive.\n";

  if (FLAGS_bracket_len <= 0.0) LOG(FATAL) << "Bracket length must be positive.";

  if (FLAGS_num_overlaps < 1) LOG(FATAL) << "Number of overlaps must be positive.";

  if (FLAGS_timestamp_offsets_max_change < 0)
    LOG(FATAL) << "The timestamp offsets must be non-negative.";

  if (FLAGS_refiner_min_angle <= 0.0)
    LOG(FATAL) << "The min triangulation angle must be positive.\n";

  if (FLAGS_depth_tri_weight < 0.0)
    LOG(FATAL) << "The depth weight must non-negative.\n";

  if (FLAGS_mesh_tri_weight < 0.0)
    LOG(FATAL) << "The mesh weight must non-negative.\n";

  if (FLAGS_depth_mesh_weight < 0.0)
    LOG(FATAL) << "The depth mesh weight must non-negative.\n";

  if (FLAGS_nav_cam_num_exclude_boundary_pixels < 0)
    LOG(FATAL) << "Must have a non-negative value for --nav_cam_num_exclude_boundary_pixels.\n";

  if (FLAGS_registration && (FLAGS_xyz_file.empty() || FLAGS_hugin_file.empty()))
    LOG(FATAL) << "In order to register the map, the hugin and xyz file must be specified.";

  if (FLAGS_float_scale && FLAGS_affine_depth_to_image)
    LOG(FATAL) << "The options --float_scale and --affine_depth_to_image should not be used "
               << "together. If the latter is used, the scale is always floated.\n";

  if (FLAGS_float_nonref_cameras && !FLAGS_no_extrinsics)
    LOG(FATAL) << "The option --float_nonref_cameras must be used only with --no_extrinsics.\n";

  if (FLAGS_no_extrinsics && FLAGS_float_timestamp_offsets)
      LOG(FATAL) << "Cannot float timestamps with option --no_extrinsics.\n";

  if (FLAGS_save_images_and_depth_clouds && FLAGS_out_dir == "")
    LOG(FATAL) << "Cannot save images and clouds if no output directory was provided.\n";

  if (FLAGS_save_matches && FLAGS_out_dir == "")
    LOG(FATAL) << "Cannot save matches if no output directory was provided.\n";

#if !HAVE_ASTROBEE
  if (FLAGS_rig_config == "")
    LOG(FATAL) << "Must specify the initial rig configuration via --rig_config.\n";

  if (FLAGS_image_list == "")
    LOG(FATAL) << "Must specify the image list via --image_list.\n";
#endif

  return;
}

void set_up_block_sizes(int num_depth_params,
                        std::vector<int> & bracketed_cam_block_sizes,
                        std::vector<int> & bracketed_depth_block_sizes,
                        std::vector<int> & bracketed_depth_mesh_block_sizes,
                        std::vector<int> & mesh_block_sizes) {
  // Wipe the outputs
  bracketed_cam_block_sizes.clear();
  bracketed_depth_block_sizes.clear();
  bracketed_depth_mesh_block_sizes.clear();
  mesh_block_sizes.clear();

  int num_focal_lengths = 1;      // The x and y focal length are assumed to be the same
  int num_distortion_params = 1;  // will be overwritten later

  // Set up the variable blocks to optimize for BracketedCamError

  bracketed_cam_block_sizes.push_back(dense_map::NUM_RIGID_PARAMS);
  bracketed_cam_block_sizes.push_back(dense_map::NUM_RIGID_PARAMS);
  bracketed_cam_block_sizes.push_back(dense_map::NUM_RIGID_PARAMS);
  bracketed_cam_block_sizes.push_back(dense_map::NUM_XYZ_PARAMS);
  bracketed_cam_block_sizes.push_back(dense_map::NUM_SCALAR_PARAMS);
  bracketed_cam_block_sizes.push_back(num_focal_lengths);
  bracketed_cam_block_sizes.push_back(dense_map::NUM_OPT_CTR_PARAMS);
  bracketed_cam_block_sizes.push_back(num_distortion_params);

  // Set up variable blocks to optimize for BracketedDepthError
  bracketed_depth_block_sizes.push_back(dense_map::NUM_RIGID_PARAMS);
  bracketed_depth_block_sizes.push_back(dense_map::NUM_RIGID_PARAMS);
  bracketed_depth_block_sizes.push_back(dense_map::NUM_RIGID_PARAMS);
  bracketed_depth_block_sizes.push_back(num_depth_params);
  bracketed_depth_block_sizes.push_back(dense_map::NUM_SCALAR_PARAMS);
  bracketed_depth_block_sizes.push_back(dense_map::NUM_XYZ_PARAMS);
  bracketed_depth_block_sizes.push_back(dense_map::NUM_SCALAR_PARAMS);

  // Set up the variable blocks to optimize for BracketedDepthMeshError
  bracketed_depth_mesh_block_sizes.push_back(dense_map::NUM_RIGID_PARAMS);
  bracketed_depth_mesh_block_sizes.push_back(dense_map::NUM_RIGID_PARAMS);
  bracketed_depth_mesh_block_sizes.push_back(dense_map::NUM_RIGID_PARAMS);
  bracketed_depth_mesh_block_sizes.push_back(num_depth_params);
  bracketed_depth_mesh_block_sizes.push_back(dense_map::NUM_SCALAR_PARAMS);
  bracketed_depth_mesh_block_sizes.push_back(dense_map::NUM_SCALAR_PARAMS);

  // Set up the variable blocks to optimize for the mesh xyz
  mesh_block_sizes.push_back(dense_map::NUM_XYZ_PARAMS);
}

typedef std::map<std::string, std::vector<rosbag::MessageInstance>> StrToMsgMap;

// Look up each ref cam image by timestamp. In between any two ref cam timestamps,
// which are no further from each other than the bracket length, look up an image
// of each of the other camera types. If more than one choice, try to stay as close
// as possible to the midpoint of the two bracketing ref cam timestamps. This way
// there's more wiggle room later if one attempts to modify the timestamp offset.
void lookupImagesAndBrackets(  // Inputs
  int ref_cam_type, double bracket_len, double timestamp_offsets_max_change,
  double max_haz_cam_image_to_depth_timestamp_diff, std::vector<std::string> const& cam_names,
  std::vector<double> const& ref_timestamps, std::vector<std::string> const& image_topics,
  std::vector<std::string> const& depth_topics, StrToMsgMap const& bag_map,
  std::vector<std::vector<ImageMessage>> const& image_data,
  std::vector<std::vector<ImageMessage>> const& depth_data,
  std::vector<std::set<double>> const& cam_timestamps_to_use,
  std::vector<double> const& ref_to_cam_timestamp_offsets,
  // Outputs
  std::vector<dense_map::cameraImage>& cams, std::vector<double>& min_timestamp_offset,
  std::vector<double>& max_timestamp_offset) {
  std::cout << "Looking up the images and bracketing the timestamps." << std::endl;

  int num_ref_cams = ref_timestamps.size();
  int num_cam_types = cam_names.size();

  // Initialize the outputs
  cams.clear();
  min_timestamp_offset.resize(num_cam_types, -1.0e+100);
  max_timestamp_offset.resize(num_cam_types,  1.0e+100);

  // A lot of care is needed with positions. This remembers how we travel in time
  // for each camera type so we have fewer messages to search.
  // But if a mistake is done below it will mess up this bookkeeping.
  std::vector<int> image_start_positions(num_cam_types, 0);
  std::vector<int> cloud_start_positions(num_cam_types, 0);

  // Populate the data for each camera image
  for (int beg_ref_it = 0; beg_ref_it < num_ref_cams; beg_ref_it++) {
    if (ref_cam_type != 0)
      LOG(FATAL) << "It is assumed that the ref cam type is 0.";

    bool save_grayscale = true;                       // for matching we will need grayscale

    // For when we have last ref timestamp and last other cam timestamp and they are equal
    int end_ref_it = beg_ref_it + 1;
    bool last_timestamp = (end_ref_it == num_ref_cams);
    if (last_timestamp) end_ref_it = beg_ref_it;

    for (int cam_type = ref_cam_type; cam_type < num_cam_types; cam_type++) {
      dense_map::cameraImage cam;
      bool success = false;

      // The ref cam does not need bracketing, but the others need to be bracketed
      // by ref cam, so there are two cases to consider.

      if (cam_type == ref_cam_type) {
        cam.camera_type   = cam_type;
        cam.timestamp     = ref_timestamps[beg_ref_it];
        cam.ref_timestamp = cam.timestamp;  // the time offset is 0 between ref and itself
        cam.beg_ref_index = beg_ref_it;
        cam.end_ref_index = beg_ref_it;  // same index for beg and end

        // Start looking up the image timestamp from this position. Some care
        // is needed here as we advance in time in image_start_positions[cam_type].
        double found_time = -1.0;
        // This has to succeed since this timestamp originally came from the bag
        bool have_lookup = false;
        if (!image_data.empty())
          have_lookup =  // Read from images/depth clouds stored in files
            dense_map::lookupImage(cam.timestamp, image_data[cam_type], cam.image,
                                   image_start_positions[cam_type],  // this will move forward
                                   found_time);

        else
          have_lookup =  // Read from bag
            dense_map::lookupImage(cam.timestamp, mapVal(bag_map, image_topics[cam_type]),
                                   save_grayscale,
                                   // outputs
                                   cam.image,
                                   image_start_positions[cam_type],  // this will move forward
                                   found_time);
        if (!have_lookup)
          LOG(FATAL) << std::fixed << std::setprecision(17)
                     << "Cannot look up camera at time " << cam.timestamp << ".\n";

        // The exact time is expected
        if (found_time != cam.timestamp)
          LOG(FATAL) << std::fixed << std::setprecision(17)
                     << "Cannot look up camera at time " << cam.timestamp << ".\n";

        success = true;

      } else {
        // Need care here since sometimes ref_cam and current cam can have
        // exactly the same timestamp, so then bracketing should succeed.

        // Convert the bracketing timestamps to current cam's time
        double ref_to_cam_offset = ref_to_cam_timestamp_offsets[cam_type];
        double beg_timestamp     = ref_timestamps[beg_ref_it] + ref_to_cam_offset;
        double end_timestamp     = ref_timestamps[end_ref_it] + ref_to_cam_offset;

        if (end_timestamp < beg_timestamp)
          LOG(FATAL) << "Ref timestamps must be in strictly increasing order.\n";

        // Allow a little exception for the last timestamp
        if (end_timestamp == beg_timestamp && !last_timestamp)
          LOG(FATAL) << "Ref timestamps must be in strictly increasing order.\n";

        if (end_timestamp - beg_timestamp > bracket_len)
          continue;  // Must respect the bracket length

        // Find the image timestamp closest to the midpoint of the brackets. This will give
        // more room to vary the timestamp later.
        double mid_timestamp = (beg_timestamp + end_timestamp)/2.0;

        // Search forward in time from image_start_positions[cam_type].
        // We will update that too later. One has to be very careful
        // with it so it does not go too far forward in time
        // so that at the next iteration we are passed what we
        // search for.
        int start_pos = image_start_positions[cam_type];  // care here
        double curr_timestamp = beg_timestamp;            // start here
        cv::Mat best_image;
        double best_dist = 1.0e+100;
        double best_time = -1.0, found_time = -1.0;
        while (1) {
          if (found_time > end_timestamp) break;  // out of range

          cv::Mat image;
          bool have_lookup = false;
          if (!image_data.empty())
            have_lookup =  // Read from images/depth clouds stored in files
              dense_map::lookupImage(curr_timestamp, image_data[cam_type], image,
                                     // care here, start_pos moves forward
                                     start_pos,
                                     // found_time will be updated now
                                     found_time);
          else
            have_lookup =                             // Read from bag
              dense_map::lookupImage(curr_timestamp,  // start looking from here
                                     mapVal(bag_map, image_topics[cam_type]), save_grayscale,
                                     // outputs
                                     image,
                                     // care here, start_pos moves forward
                                     start_pos,
                                     // found_time will be updated now
                                     found_time);

          if (!have_lookup)
            break;  // Need not succeed, but then there's no need to go on as we are at the end

          // Check if the found time is in the bracket
          bool is_in_bracket = (beg_timestamp <= found_time && found_time <= end_timestamp);
          double curr_dist = std::abs(found_time - mid_timestamp);

          if (curr_dist < best_dist && is_in_bracket) {
            best_dist = curr_dist;
            best_time = found_time;
            // Update the start position for the future only if this is a good
            // solution. Otherwise we may have moved too far.
            image_start_positions[cam_type] = start_pos;
            image.copyTo(best_image);
          }

          // Go forward in time. We count on the fact that
          // lookupImage() looks forward from given guess.
          // Careful here with the api of std::nextafter().
          curr_timestamp = std::nextafter(found_time, found_time + 1.0);
        }

        if (best_time < 0.0) continue;  // bracketing failed

        // Note how we allow best_time == beg_timestamp if there's no other choice
        if (best_time < beg_timestamp || best_time > end_timestamp) continue;  // no luck

        cam.camera_type   = cam_type;
        cam.timestamp     = best_time;
        cam.ref_timestamp = best_time - ref_to_cam_offset;
        cam.beg_ref_index = beg_ref_it;
        cam.end_ref_index = end_ref_it;
        cam.image         = best_image;

        success = true;
      }

      // See if to skip this timestamp
      if (!cam_timestamps_to_use[cam_type].empty() &&
          cam_timestamps_to_use[cam_type].find(cam.timestamp) ==
          cam_timestamps_to_use[cam_type].end()) {
        std::cout << std::setprecision(17) << "For " << cam_names[cam_type]
                  << " skipping timestamp: " << cam.timestamp << std::endl;
        continue;
      }

      if (!success) continue;

      if (cam_type != ref_cam_type) {
        double ref_to_cam_offset = ref_to_cam_timestamp_offsets[cam_type];

        // cam.timestamp was chosen as centrally as possible so that
        // ref_timestamps[beg_ref_it] + ref_to_cam_offset <= cam.timestamp
        // and
        // cam.timestamp <= ref_timestamps[end_ref_it] + ref_to_cam_offset
        // Find the range of potential future values of ref_to_cam_offset so that
        // cam.timestamp still respects these bounds.
        min_timestamp_offset[cam_type]
          = std::max(min_timestamp_offset[cam_type], cam.timestamp - ref_timestamps[end_ref_it]);
        max_timestamp_offset[cam_type]
          = std::min(max_timestamp_offset[cam_type], cam.timestamp - ref_timestamps[beg_ref_it]);
      }

      // Look up the closest cloud in time (either before or after cam.timestamp)
      // This need not succeed.
      cam.cloud_timestamp = -1.0;  // will change
      if (!depth_data.empty())     // Read from images/depth clouds stored in files
        dense_map::lookupImage(cam.timestamp,  // start looking from this time forward
                               depth_data[cam_type],
                               // Outputs
                               cam.depth_cloud,
                               cloud_start_positions[cam_type],  // this will move forward
                               cam.cloud_timestamp);             // found time
      else if (depth_topics[cam_type] != "")                     // Read from bag
        dense_map::lookupCloud(cam.timestamp,  // start looking from this time forward
                               mapVal(bag_map, depth_topics[cam_type]),
                               max_haz_cam_image_to_depth_timestamp_diff,
                               // Outputs
                               cam.depth_cloud,
                               cloud_start_positions[cam_type],  // this will move forward
                               cam.cloud_timestamp);             // found time

      cams.push_back(cam);
    }  // end loop over camera types
  }    // end loop over ref images

  // See how many timestamps we have for each camera
  std::map<int, int> num_images;
  for (int cam_type_it = 0; cam_type_it < num_cam_types; cam_type_it++) num_images[cam_type_it] = 0;
  for (size_t cam_it = 0; cam_it < cams.size(); cam_it++)
    num_images[cams[cam_it].camera_type]++;

  bool is_good = true;
  for (int cam_type_it = 0; cam_type_it < num_cam_types; cam_type_it++) {
    std::cout << "Number of found images for camera: " << cam_names[cam_type_it] << ": "
              << num_images[cam_type_it] << std::endl;

    if (num_images[cam_type_it] == 0) is_good = false;
  }

  if (!is_good)
    LOG(FATAL) << "Could not bracket all images. Cannot continue.\n";

  std::cout << "Timestamp offset allowed ranges based on current bracketing:\n";
  // Adjust for timestamp_offsets_max_change
  for (int cam_type = ref_cam_type; cam_type < num_cam_types; cam_type++) {
    if (cam_type == ref_cam_type) continue;  // bounds don't make sense here
    min_timestamp_offset[cam_type] = std::max(min_timestamp_offset[cam_type],
                                              ref_to_cam_timestamp_offsets[cam_type]
                                              - timestamp_offsets_max_change);
    max_timestamp_offset[cam_type] = std::min(max_timestamp_offset[cam_type],
                                              ref_to_cam_timestamp_offsets[cam_type]
                                              + timestamp_offsets_max_change);

    // Tighten a bit to ensure we don't exceed things when we add
    // and subtract timestamps later. Note that timestamps are
    // measured in seconds and fractions of a second since epoch and
    // can be quite large so precision loss can easily happen.
    min_timestamp_offset[cam_type] += 1.0e-5;
    max_timestamp_offset[cam_type] -= 1.0e-5;
    std::cout << std::setprecision(8) << cam_names[cam_type]
              << ": [" << min_timestamp_offset[cam_type]
              << ", " << max_timestamp_offset[cam_type] << "]\n";
  }
}

// TODO(oalexan1): Move to utils
void multiViewTriangulation(  // Inputs
  std::vector<camera::CameraParameters> const& cam_params,
  std::vector<dense_map::cameraImage> const& cams, std::vector<Eigen::Affine3d> const& world_to_cam,
  std::vector<std::map<int, int>> const& pid_to_cid_fid,
  std::vector<std::vector<std::pair<float, float>>> const& keypoint_vec,
  // Outputs
  std::vector<std::map<int, std::map<int, int>>>& pid_cid_fid_inlier,
  std::vector<Eigen::Vector3d>& xyz_vec) {
  xyz_vec.resize(pid_to_cid_fid.size());

  for (size_t pid = 0; pid < pid_to_cid_fid.size(); pid++) {
    std::vector<double> focal_length_vec;
    std::vector<Eigen::Affine3d> world_to_cam_aff_vec;
    std::vector<Eigen::Vector2d> pix_vec;

    for (auto cid_fid = pid_to_cid_fid[pid].begin(); cid_fid != pid_to_cid_fid[pid].end();
         cid_fid++) {
      int cid = cid_fid->first;
      int fid = cid_fid->second;

      // Triangulate inliers only
      if (!dense_map::getMapValue(pid_cid_fid_inlier, pid, cid, fid))
        continue;

      Eigen::Vector2d dist_ip(keypoint_vec[cid][fid].first, keypoint_vec[cid][fid].second);
      Eigen::Vector2d undist_ip;
      cam_params[cams[cid].camera_type].Convert<camera::DISTORTED, camera::UNDISTORTED_C>
        (dist_ip, &undist_ip);

      focal_length_vec.push_back(cam_params[cams[cid].camera_type].GetFocalLength());
      world_to_cam_aff_vec.push_back(world_to_cam[cid]);
      pix_vec.push_back(undist_ip);
    }

    if (pix_vec.size() < 2) {
      // If after outlier filtering less than two rays are left, can't triangulate.
      // Must set all features for this pid to outliers.
      for (auto cid_fid = pid_to_cid_fid[pid].begin(); cid_fid != pid_to_cid_fid[pid].end();
           cid_fid++) {
        int cid = cid_fid->first;
        int fid = cid_fid->second;
        dense_map::setMapValue(pid_cid_fid_inlier, pid, cid, fid, 0);
      }

      // Nothing else to do
      continue;
    }

    // Triangulate n rays emanating from given undistorted and centered pixels
    xyz_vec[pid] = dense_map::Triangulate(focal_length_vec, world_to_cam_aff_vec, pix_vec);
  }

  return;
}

// TODO(oalexan1): Move to utils
void meshTriangulations(  // Inputs
  std::vector<camera::CameraParameters> const& cam_params,
  std::vector<dense_map::cameraImage> const& cams, std::vector<Eigen::Affine3d> const& world_to_cam,
  std::vector<std::map<int, int>> const& pid_to_cid_fid,
  std::vector<std::map<int, std::map<int, int>>> const& pid_cid_fid_inlier,
  std::vector<std::vector<std::pair<float, float>>> const& keypoint_vec,
  Eigen::Vector3d const& bad_xyz, double min_ray_dist, double max_ray_dist,
  mve::TriangleMesh::Ptr const& mesh, std::shared_ptr<BVHTree> const& bvh_tree,
  // Outputs
  std::vector<std::map<int, std::map<int, Eigen::Vector3d>>>& pid_cid_fid_mesh_xyz,
  std::vector<Eigen::Vector3d>& pid_mesh_xyz) {
  // Initialize the outputs
  pid_cid_fid_mesh_xyz.resize(pid_to_cid_fid.size());
  pid_mesh_xyz.resize(pid_to_cid_fid.size());

  for (size_t pid = 0; pid < pid_to_cid_fid.size(); pid++) {
    Eigen::Vector3d avg_mesh_xyz(0, 0, 0);
    int num_intersections = 0;

    for (auto cid_fid = pid_to_cid_fid[pid].begin(); cid_fid != pid_to_cid_fid[pid].end();
         cid_fid++) {
      int cid = cid_fid->first;
      int fid = cid_fid->second;

      // Initialize this
      pid_cid_fid_mesh_xyz[pid][cid][fid] = bad_xyz;

      // Deal with inliers only
      if (!dense_map::getMapValue(pid_cid_fid_inlier, pid, cid, fid))
        continue;

      // Intersect the ray with the mesh
      Eigen::Vector2d dist_ip(keypoint_vec[cid][fid].first, keypoint_vec[cid][fid].second);
      Eigen::Vector3d mesh_xyz(0.0, 0.0, 0.0);
      bool have_mesh_intersection
        = dense_map::ray_mesh_intersect(dist_ip, cam_params[cams[cid].camera_type],
                                        world_to_cam[cid], mesh, bvh_tree,
                                        min_ray_dist, max_ray_dist,
                                        // Output
                                        mesh_xyz);

      if (have_mesh_intersection) {
        pid_cid_fid_mesh_xyz[pid][cid][fid] = mesh_xyz;
        avg_mesh_xyz += mesh_xyz;
        num_intersections += 1;
      }
    }

    // Average the intersections of all rays with the mesh
    if (num_intersections >= 1)
      avg_mesh_xyz /= num_intersections;
    else
      avg_mesh_xyz = bad_xyz;

    pid_mesh_xyz[pid] = avg_mesh_xyz;
  }

  return;
}

void flagOutlierByExclusionDist(  // Inputs
  int ref_cam_type, int nav_cam_num_exclude_boundary_pixels,
  std::vector<camera::CameraParameters> const& cam_params,
  std::vector<dense_map::cameraImage> const& cams,
  std::vector<std::map<int, int>> const& pid_to_cid_fid,
  std::vector<std::vector<std::pair<float, float>>> const& keypoint_vec,
  // Outputs
  std::vector<std::map<int, std::map<int, int>>>& pid_cid_fid_inlier) {
  // Initialize the output
  pid_cid_fid_inlier.resize(pid_to_cid_fid.size());

  // Iterate though interest point matches
  for (size_t pid = 0; pid < pid_to_cid_fid.size(); pid++) {
    for (auto cid_fid = pid_to_cid_fid[pid].begin(); cid_fid != pid_to_cid_fid[pid].end();
         cid_fid++) {
      int cid = cid_fid->first;
      int fid = cid_fid->second;

      // Initially there are inliers only
      pid_cid_fid_inlier[pid][cid][fid] = 1;

      if (cams[cid].camera_type == ref_cam_type) {
        // Flag as outliers pixels at the nav_cam boundary, if desired. This
        // is especially important when the nav_cam uses the radtan
        // model instead of fisheye.
        Eigen::Vector2d dist_ip(keypoint_vec[cid][fid].first, keypoint_vec[cid][fid].second);
        Eigen::Vector2i dist_size = cam_params[cams[cid].camera_type].GetDistortedSize();
        int excl = nav_cam_num_exclude_boundary_pixels;
        if (dist_ip.x() < excl || dist_ip.x() > dist_size[0] - 1 - excl ||
            dist_ip.y() < excl || dist_ip.y() > dist_size[1] - 1 - excl) {
          dense_map::setMapValue(pid_cid_fid_inlier, pid, cid, fid, 0);
        }
      }
    }
  }
  return;
}

// Flag outliers by triangulation angle and reprojection error.  It is
// assumed that the cameras in world_to_cam are up-to-date given the
// current state of optimization, and that the residuals (including
// the reprojection errors) have also been updated beforehand.
void flagOutliersByTriAngleAndReprojErr(  // Inputs
  double refiner_min_angle, double max_reprojection_error,
  std::vector<std::map<int, int>> const& pid_to_cid_fid,
  std::vector<std::vector<std::pair<float, float>>> const& keypoint_vec,
  std::vector<Eigen::Affine3d> const& world_to_cam, std::vector<Eigen::Vector3d> const& xyz_vec,
  std::vector<std::map<int, std::map<int, int>>> const& pid_cid_fid_to_residual_index,
  std::vector<double> const& residuals,
  // Outputs
  std::vector<std::map<int, std::map<int, int>>>& pid_cid_fid_inlier) {
  // Must deal with outliers by triangulation angle before
  // removing outliers by reprojection error, as the latter will
  // exclude some rays which form the given triangulated points.
  int num_outliers_by_angle = 0, num_total_features = 0;
  for (size_t pid = 0; pid < pid_to_cid_fid.size(); pid++) {
    // Find the largest angle among any two intersecting rays
    double max_rays_angle = 0.0;

    for (auto cid_fid1 = pid_to_cid_fid[pid].begin();
         cid_fid1 != pid_to_cid_fid[pid].end(); cid_fid1++) {
      int cid1 = cid_fid1->first;
      int fid1 = cid_fid1->second;

      // Deal with inliers only
      if (!dense_map::getMapValue(pid_cid_fid_inlier, pid, cid1, fid1)) continue;

      num_total_features++;

      Eigen::Vector3d cam_ctr1 = (world_to_cam[cid1].inverse()) * Eigen::Vector3d(0, 0, 0);
      Eigen::Vector3d ray1 = xyz_vec[pid] - cam_ctr1;
      ray1.normalize();

      for (auto cid_fid2 = pid_to_cid_fid[pid].begin();
           cid_fid2 != pid_to_cid_fid[pid].end(); cid_fid2++) {
        int cid2 = cid_fid2->first;
        int fid2 = cid_fid2->second;

        // Look at each cid and next cids
        if (cid2 <= cid1)
          continue;

        // Deal with inliers only
        if (!dense_map::getMapValue(pid_cid_fid_inlier, pid, cid2, fid2)) continue;

        Eigen::Vector3d cam_ctr2 = (world_to_cam[cid2].inverse()) * Eigen::Vector3d(0, 0, 0);
        Eigen::Vector3d ray2 = xyz_vec[pid] - cam_ctr2;
        ray2.normalize();

        double curr_angle = (180.0 / M_PI) * acos(ray1.dot(ray2));

        if (std::isnan(curr_angle) || std::isinf(curr_angle)) continue;

        max_rays_angle = std::max(max_rays_angle, curr_angle);
      }
    }

    if (max_rays_angle >= refiner_min_angle)
      continue;  // This is a good triangulated point, with large angle of convergence

    // Flag as outliers all the features for this cid
    for (auto cid_fid = pid_to_cid_fid[pid].begin();
         cid_fid != pid_to_cid_fid[pid].end(); cid_fid++) {
      int cid = cid_fid->first;
      int fid = cid_fid->second;

      // Deal with inliers only
      if (!dense_map::getMapValue(pid_cid_fid_inlier, pid, cid, fid)) continue;

      num_outliers_by_angle++;
      dense_map::setMapValue(pid_cid_fid_inlier, pid, cid, fid, 0);
    }
  }
  std::cout << std::setprecision(4) << "Removed " << num_outliers_by_angle
            << " outlier features with small angle of convergence, out of "
            << num_total_features << " ("
            << (100.0 * num_outliers_by_angle) / num_total_features << " %)\n";

  int num_outliers_reproj = 0;
  num_total_features = 0;  // reusing this variable
  for (size_t pid = 0; pid < pid_to_cid_fid.size(); pid++) {
    for (auto cid_fid = pid_to_cid_fid[pid].begin();
         cid_fid != pid_to_cid_fid[pid].end(); cid_fid++) {
      int cid = cid_fid->first;
      int fid = cid_fid->second;

      // Deal with inliers only
      if (!dense_map::getMapValue(pid_cid_fid_inlier, pid, cid, fid)) continue;

      num_total_features++;

      // Find the pixel residuals
      size_t residual_index = dense_map::getMapValue(pid_cid_fid_to_residual_index, pid, cid, fid);
      if (residuals.size() <= residual_index + 1) LOG(FATAL) << "Too few residuals.\n";

      double res_x = residuals[residual_index + 0];
      double res_y = residuals[residual_index + 1];
      // NaN values will never be inliers if the comparison is set as below
      bool is_good = (Eigen::Vector2d(res_x, res_y).norm() <= max_reprojection_error);
      if (!is_good) {
        num_outliers_reproj++;
        dense_map::setMapValue(pid_cid_fid_inlier, pid, cid, fid, 0);
      }
    }
  }

  std::cout << std::setprecision(4) << "Removed " << num_outliers_reproj
            << " outlier features using reprojection error, out of " << num_total_features
            << " (" << (100.0 * num_outliers_reproj) / num_total_features << " %)\n";

  return;
}

// Evaluate the residuals before and after optimization
void evalResiduals(  // Inputs
  std::string const& tag, std::vector<std::string> const& residual_names,
  std::vector<double> const& residual_scales,
  // Outputs
  ceres::Problem& problem, std::vector<double>& residuals) {
  double total_cost = 0.0;
  ceres::Problem::EvaluateOptions eval_options;
  eval_options.num_threads = 1;
  eval_options.apply_loss_function = false;  // want raw residuals
  problem.Evaluate(eval_options, &total_cost, &residuals, NULL, NULL);

  // Sanity checks, after the residuals are created
  if (residuals.size() != residual_names.size())
    LOG(FATAL) << "There must be as many residual names as residual values.";
  if (residuals.size() != residual_scales.size())
    LOG(FATAL) << "There must be as many residual values as residual scales.";

  // Compensate for the scale
  for (size_t it = 0; it < residuals.size(); it++)
    residuals[it] /= residual_scales[it];

  dense_map::calc_residuals_stats(residuals, residual_names, tag);
  return;
}

// Given all the merged and filtered tracks in pid_cid_fid, for each
// image pair cid1 and cid2 with cid1 < cid2 < cid1 + num_overlaps + 1,
// save the matches of this pair which occur in the set of tracks.
void saveInlinerMatchPairs(  // Inputs
  std::vector<std::string> const& image_files, int num_overlaps,
  std::vector<std::map<int, int>> const& pid_to_cid_fid,
  std::vector<std::vector<std::pair<float, float>>> const& keypoint_vec,
  std::vector<std::map<int, std::map<int, int>>> const& pid_cid_fid_inlier,
  std::string const& out_dir) {
  MATCH_MAP matches;

  for (size_t pid = 0; pid < pid_to_cid_fid.size(); pid++) {
    for (auto cid_fid1 = pid_to_cid_fid[pid].begin();
         cid_fid1 != pid_to_cid_fid[pid].end(); cid_fid1++) {
      int cid1 = cid_fid1->first;
      int fid1 = cid_fid1->second;

      for (auto cid_fid2 = pid_to_cid_fid[pid].begin();
           cid_fid2 != pid_to_cid_fid[pid].end(); cid_fid2++) {
        int cid2 = cid_fid2->first;
        int fid2 = cid_fid2->second;

        bool is_good = (cid1 < cid2 && cid2 < cid1 + num_overlaps + 1);
        if (!is_good)
          continue;

        // Consider inliers only
        if (!dense_map::getMapValue(pid_cid_fid_inlier, pid, cid1, fid1) ||
            !dense_map::getMapValue(pid_cid_fid_inlier, pid, cid2, fid2))
          continue;

        auto index_pair = std::make_pair(cid1, cid2);

        InterestPoint ip1(keypoint_vec[cid1][fid1].first, keypoint_vec[cid1][fid1].second);
        InterestPoint ip2(keypoint_vec[cid2][fid2].first, keypoint_vec[cid2][fid2].second);

        matches[index_pair].first.push_back(ip1);
        matches[index_pair].second.push_back(ip2);
      }
    }
  }  // End iterations over pid

  for (auto it = matches.begin(); it != matches.end(); it++) {
    auto & index_pair = it->first;
    dense_map::MATCH_PAIR const& match_pair = it->second;

    int left_index = index_pair.first;
    int right_index = index_pair.second;

    auto & left_image  = image_files[left_index];  // alias
    auto & right_image = image_files[right_index];  // alias

    std::string match_dir = out_dir + "/matches";
    dense_map::createDir(match_dir);

    std::string suffix = "-inliers";
    std::string match_file = dense_map::matchFileName(match_dir, left_image, right_image, suffix);

    std::cout << "Writing: " << match_file << std::endl;
    dense_map::writeMatchFile(match_file, match_pair.first, match_pair.second);
  }
}

// TODO(oalexan1): Move this to utils
// Save an affine transform represented as a matrix to a file.
std::string affineToStr(Eigen::Affine3d const& M) {
  Eigen::MatrixXd T = M.matrix();
  std::ostringstream os;
  os.precision(17);
  os << T(0, 0) << " " << T(0, 1) << " " << T(0, 2) << " "
     << T(1, 0) << " " << T(1, 1) << " " << T(1, 2) << " "
     << T(2, 0) << " " << T(2, 1) << " " << T(2, 2) << " "
     << T(0, 3) << " " << T(1, 3) << " " << T(2, 3);

  return os.str();
}

// TODO(oalexan1): Move to utils
// Form an affine transform from 12 values
Eigen::Affine3d vecToAffine(Eigen::VectorXd const& vals) {
  if (vals.size() != 12)
    LOG(FATAL) << "An affine transform must have 12 parameters.\n";

  Eigen::Affine3d M = Eigen::Affine3d::Identity();
  Eigen::MatrixXd T = M.matrix();

  int count = 0;

  // linear part
  T(0, 0) = vals[count++];
  T(0, 1) = vals[count++];
  T(0, 2) = vals[count++];
  T(1, 0) = vals[count++];
  T(1, 1) = vals[count++];
  T(1, 2) = vals[count++];
  T(2, 0) = vals[count++];
  T(2, 1) = vals[count++];
  T(2, 2) = vals[count++];

  // translation part
  T(0, 3) = vals[count++];
  T(1, 3) = vals[count++];
  T(2, 3) = vals[count++];

  M.matrix() = T;

  return M;
}

// TODO(oalexan1): Move this to utils

// Save the properties of images. Use space as separator.
void writeImageList(std::string const& out_dir, std::vector<dense_map::cameraImage> const& cams,
                    std::vector<std::string> const& image_files,
                    std::vector<std::string> const& depth_files,
                    std::vector<Eigen::Affine3d> const& world_to_cam) {
  dense_map::createDir(out_dir);
  std::string image_list = out_dir + "/images.txt";
  std::cout << "Writing: " << image_list << std::endl;

  std::ofstream f;
  f.open(image_list.c_str(), std::ios::binary | std::ios::out);
  if (!f.is_open()) LOG(FATAL) << "Cannot open file for writing: " << image_list << "\n";
  f.precision(17);

  f << "# image_file sensor_id timestamp depth_file world_to_image\n";

  for (size_t it = 0; it < cams.size(); it++) {
    std::string depth_file = depth_files[it];
    if (cams[it].depth_cloud.cols == 0 || cams[it].depth_cloud.rows == 0)
      depth_file = dense_map::NO_DEPTH_FILE;

    // Convert an affine transform to a 4x4 matrix
    Eigen::MatrixXd T = world_to_cam[it].matrix();

    // Save the rotation and translation of T
    f << image_files[it] << " " << cams[it].camera_type  << " " <<
      cams[it].timestamp << " " << depth_file << " "
      << dense_map::affineToStr(world_to_cam[it]) << "\n";
  }

  f.close();
}

// TODO(oalexan1): Move to utils
// Read the images, depth clouds, and their metadata
void readImageAndDepthData(  // Inputs
  std::string const& image_list_file, int ref_cam_type,
  // Outputs
  std::vector<double>& ref_timestamps, std::vector<Eigen::Affine3d>& world_to_ref,
  std::vector<std::vector<ImageMessage>>& image_data,
  std::vector<std::vector<ImageMessage>>& depth_data) {
  // Clear the outputs
  ref_timestamps.clear();
  world_to_ref.clear();
  image_data.clear();
  depth_data.clear();

  // Open the file
  std::cout << "Reading: " << image_list_file << std::endl;
  std::ifstream f;
  f.open(image_list_file.c_str(), std::ios::binary | std::ios::in);
  if (!f.is_open()) LOG(FATAL) << "Cannot open file for reading: " << image_list_file << "\n";

  // Read here temporarily the images and depth maps
  std::map<int, std::map<double, ImageMessage>> image_maps;
  std::map<int, std::map<double, ImageMessage>> depth_maps;

  std::string line;
  while (getline(f, line)) {
    if (line.empty() || line[0] == '#') continue;

    std::string image_file, depth_file;
    int cam_type;
    double timestamp;
    std::istringstream iss(line);
    if (!(iss >> image_file >> cam_type >> timestamp >> depth_file))
      LOG(FATAL) << "Cannot parse the image file, sensor id, timestamp, and depth file in: "
                 << image_list_file << "\n";

    if (cam_type < 0) LOG(FATAL) << "The sensor id must be non-negative.\n";

    // Read the camera to world transform
    Eigen::VectorXd vals(12);
    double val = -1.0;
    int count = 0;
    while (iss >> val) {
      if (count >= 12) break;
      vals[count] = val;
      count++;
    }

    if (count != 12)
      LOG(FATAL) << "Expecting 12 values for the transform on line:\n" << line << "\n";

    Eigen::Affine3d world_to_cam = vecToAffine(vals);

    // Create aliases
    std::map<double, ImageMessage> & image_map = image_maps[cam_type];
    std::map<double, ImageMessage> & depth_map = depth_maps[cam_type];

    if (image_map.find(timestamp) != image_map.end())
      LOG(FATAL) << "Duplicate timestamp " << std::setprecision(17) << timestamp
                 << " for sensor id " << cam_type << "\n";

    // Read the image exactly as written, which would be grayscale
    image_map[timestamp].image        = cv::imread(image_file, cv::IMREAD_UNCHANGED);
    image_map[timestamp].name         = image_file;
    image_map[timestamp].timestamp    = timestamp;
    image_map[timestamp].world_to_cam = world_to_cam;

    // Sanity check
    if (depth_map.find(timestamp) != depth_map.end())
      LOG(FATAL) << "Duplicate timestamp " << std::setprecision(17) << timestamp
                 << " for sensor id " << cam_type << "\n";

    // Read the depth data, if present
    if (depth_file != dense_map::NO_DEPTH_FILE) {
      dense_map::readXyzImage(depth_file, depth_map[timestamp].image);
      depth_map[timestamp].name      = depth_file;
      depth_map[timestamp].timestamp = timestamp;
    }
  }

  // Find the range of sensor ids.
  int max_cam_type = ref_cam_type;
  for (auto it = image_maps.begin(); it != image_maps.end(); it++)
    max_cam_type = std::max(max_cam_type, it->first);
  for (auto it = depth_maps.begin(); it != depth_maps.end(); it++)
    max_cam_type = std::max(max_cam_type, it->first);

  // Store all images and depth clouds in vectors chronologically for
  // fast access later.
  image_data.resize(max_cam_type + 1);
  depth_data.resize(max_cam_type + 1);
  for (size_t cam_type = 0; cam_type < image_data.size(); cam_type++) {
    std::map<double, ImageMessage> & image_map = image_maps[cam_type];
    std::map<double, ImageMessage> & depth_map = depth_maps[cam_type];

    for (auto it = image_map.begin(); it != image_map.end(); it++) {
      image_data[cam_type].push_back(it->second);

      // Collect the ref cam timestamps and world_to_ref as well
      if (cam_type == ref_cam_type) {
        world_to_ref.push_back(it->second.world_to_cam);
        ref_timestamps.push_back(it->second.timestamp);
      }
    }

    for (auto it = depth_map.begin(); it != depth_map.end(); it++)
      depth_data[cam_type].push_back(it->second);
  }

  return;
}

// Save the optimized rig configuration
void writeRigConfig(std::string const& out_dir, bool model_rig, int ref_cam_type,
                    std::vector<std::string> const& cam_names,
                    std::vector<camera::CameraParameters> const& cam_params,
                    std::vector<Eigen::Affine3d> const& ref_to_cam_trans,
                    std::vector<Eigen::Affine3d> const& depth_to_image,
                    std::vector<double> const& ref_to_cam_timestamp_offsets) {
  if (ref_cam_type != 0)
    LOG(FATAL) << "The reference camera id must be always 0.\n";

  dense_map::createDir(out_dir);
  std::string rig_config = out_dir + "/rig_config.txt";
  std::cout << "Writing: " << rig_config << std::endl;

  std::ofstream f;
  f.open(rig_config.c_str(), std::ios::binary | std::ios::out);
  if (!f.is_open()) LOG(FATAL) << "Cannot open file for writing: " << rig_config << "\n";
  f.precision(17);

  f << "ref_sensor_id: " << ref_cam_type << "\n";

  for (size_t cam_type = ref_cam_type; cam_type < cam_params.size(); cam_type++) {
    f << "\n";
    f << "sensor_id: "    << cam_type << "\n";
    f << "sensor_name: "  << cam_names[cam_type] << "\n";
    f << "focal_length: " << cam_params[cam_type].GetFocalLength() << "\n";

    Eigen::Vector2d c = cam_params[cam_type].GetOpticalOffset();
    f << "optical_center: " << c[0] << " " << c[1] << "\n";

    Eigen::VectorXd D = cam_params[cam_type].GetDistortion();

    f << "distortion_coeffs: ";
    for (int d = 0; d < D.size(); d++) {
      f << D[d];
      if (d + 1 < D.size()) f << " ";
    }
    f << "\n";

    if (D.size() == 0) f << "distortion_type: " << dense_map::NO_DISTORION << "\n";
    if (D.size() == 1)
      f << "distortion_type: " << dense_map::FISHEYE_DISTORTION << "\n";
    else if (D.size() >= 4 && D.size() <= 5)
      f << "distortion_type: " << dense_map::RADTAN_DISTORTION << "\n";
    else
      LOG(FATAL) << "Expecting 0, 1, 4, or 5 distortion coefficients.\n";

    Eigen::Vector2i image_size = cam_params[cam_type].GetDistortedSize();
    f << "image_size: " << image_size[0] << ' ' << image_size[1] << "\n";

    Eigen::Vector2i undist_size = cam_params[cam_type].GetUndistortedSize();
    f << "undistorted_image_size: " << undist_size[0] << ' ' << undist_size[1] << "\n";

    // This is very important. When we don't assume a rig,
    // the transform among the sensors should be invalid.
    Eigen::Affine3d Zero;
    Zero.matrix() = 0 * Zero.matrix();
    Eigen::Affine3d T;

    if (model_rig)
      T = ref_to_cam_trans[cam_type];
    else
      T = Zero;

    f << "ref_to_sensor_transform: "
      << dense_map::affineToStr(T)
      << "\n";

    f << "depth_to_image_transform: "
      << dense_map::affineToStr(depth_to_image[cam_type]) << "\n";

    f << "ref_to_sensor_timestamp_offset: " << ref_to_cam_timestamp_offsets[cam_type] << "\n";
  }

  f.close();
}

// TODO(oalexan1): Move to utils

// Read real values after given tag. Ignore comments, so any line starting
// with #, and empty lines. If desired_num_vals >=0, validate that we
// read the desired number.
void readConfigVals(std::ifstream & f, std::string const& tag, int desired_num_vals,
                    Eigen::VectorXd & vals) {
  // Clear the output
  vals.resize(0);

  std::vector<double> local_vals;  // std::vector has push_back()
  std::string line;
  while (getline(f, line)) {
    if (line.empty() || line[0] == '#') continue;

    std::istringstream iss(line);
    std::string token;
    iss >> token;
    double val = 0.0;
    while (iss >> val) {
      local_vals.push_back(val);
    }

    if (token != tag) throw std::runtime_error("Could not read value for: " + tag);

    // Copy to Eigen::VectorXd
    vals.resize(local_vals.size());
    for (int it = 0; it < vals.size(); it++) vals[it] = local_vals[it];

    if (desired_num_vals >= 0 && vals.size() != desired_num_vals)
      throw std::runtime_error("Read an incorrect number of values for: " + tag);

    return;
  }

  throw std::runtime_error("Could not read value for: " + tag);
}

// TODO(oalexan1): Move to utils
// Read strings separated by spaces after given tag. Ignore comments, so any line starting
// with #, and empty lines. If desired_num_vals >=0, validate that we
// read the desired number.
void readConfigVals(std::ifstream & f, std::string const& tag, int desired_num_vals,
                    std::vector<std::string> & vals) {
  // Clear the output
  vals.resize(0);

  std::string line;
  while (getline(f, line)) {
    if (line.empty() || line[0] == '#') continue;

    std::istringstream iss(line);
    std::string token;
    iss >> token;
    std::string val;
    while (iss >> val)
      vals.push_back(val);

    if (token != tag) throw std::runtime_error("Could not read value for: " + tag);

    if (desired_num_vals >= 0 && vals.size() != desired_num_vals)
      throw std::runtime_error("Read an incorrect number of values for: " + tag);

    return;
  }

  throw std::runtime_error("Could not read value for: " + tag);
}

// TODO(oalexan1): Move to utils

// Read a rig configuration. Check if the transforms among the sensors
// on the rig is not 0, in that case will use it.
void readRigConfig(std::string const& rig_config, bool & have_rig_transforms, int & ref_cam_type,
                   std::vector<std::string> & cam_names,
                   std::vector<camera::CameraParameters> & cam_params,
                   std::vector<Eigen::Affine3d> & ref_to_cam_trans,
                   std::vector<Eigen::Affine3d> & depth_to_image,
                   std::vector<double> & ref_to_cam_timestamp_offsets) {
  try {
    // Initialize the outputs
    have_rig_transforms = true;
    ref_cam_type = 0;
    cam_names.clear();
    cam_params.clear();
    ref_to_cam_trans.clear();
    depth_to_image.clear();
    ref_to_cam_timestamp_offsets.clear();

    // Open the file
    std::cout << "Reading: " << rig_config << std::endl;
    std::ifstream f;
    f.open(rig_config.c_str(), std::ios::binary | std::ios::in);
    if (!f.is_open()) LOG(FATAL) << "Cannot open file for reading: " << rig_config << "\n";

    // Read the ref sensor id
    Eigen::VectorXd vals;
    std::vector<std::string> str_vals;

    readConfigVals(f, "ref_sensor_id:", 1, vals);
    ref_cam_type = vals[0];
    if (ref_cam_type != 0) LOG(FATAL) << "The reference sensor id must be 0.\n";

    // Read each sensor
    int sensor_it = -1;
    while (1) {
      sensor_it++;

      try {
        readConfigVals(f, "sensor_id:", 1, vals);
      } catch(...) {
        // Likely no more sensors
        return;
      }
      int sensor_id = vals[0];
      if (sensor_id != sensor_it) LOG(FATAL) << "Expecting to read sensor id: "
                                             << sensor_it << "\n";

      readConfigVals(f, "sensor_name:", 1, str_vals);
      std::string sensor_name = str_vals[0];
      cam_names.push_back(sensor_name);

      readConfigVals(f, "focal_length:", 1, vals);
      Eigen::Vector2d focal_length(vals[0], vals[0]);

      readConfigVals(f, "optical_center:", 2, vals);
      Eigen::Vector2d optical_center(vals[0], vals[1]);

      readConfigVals(f, "distortion_coeffs:", -1, vals);
      if (vals.size() != 0 && vals.size() != 1 && vals.size() != 4 && vals.size() != 5)
        LOG(FATAL) << "Expecting 0, 1, 4, or 5 distortion coefficients.\n";
      Eigen::VectorXd distortion = vals;

      readConfigVals(f, "distortion_type:", 1, str_vals);
      if (distortion.size() == 0 && str_vals[0] != dense_map::NO_DISTORION)
        LOG(FATAL) << "When there are no distortion coefficients, distortion type must be: "
                   << dense_map::NO_DISTORION << "\n";
      if (distortion.size() == 1 && str_vals[0] != dense_map::FISHEYE_DISTORTION)
        LOG(FATAL) << "When there is 1 distortion coefficient, distortion type must be: "
                   << dense_map::FISHEYE_DISTORTION << "\n";
      if ((distortion.size() == 4 || distortion.size() == 5) &&
          str_vals[0] != dense_map::RADTAN_DISTORTION)
        LOG(FATAL) << "When there is 1 distortion coefficient, distortion type must be: "
                   << dense_map::RADTAN_DISTORTION << "\n";

      readConfigVals(f, "image_size:", 2, vals);
      Eigen::Vector2i image_size(vals[0], vals[1]);

      readConfigVals(f, "undistorted_image_size:", 2, vals);
      Eigen::Vector2i undistorted_image_size(vals[0], vals[1]);

      camera::CameraParameters params(image_size, focal_length, optical_center, distortion);
      params.SetUndistortedSize(undistorted_image_size);
      cam_params.push_back(params);

      readConfigVals(f, "ref_to_sensor_transform:", 12, vals);
      ref_to_cam_trans.push_back(vecToAffine(vals));

      // Sanity check
      if (have_rig_transforms &&
          ref_to_cam_trans.back().matrix() == 0 * ref_to_cam_trans.back().matrix()) {
        std::cout << "Found that ref_to_sensor_transform is zero. Will ignore it." << std::endl;
        have_rig_transforms = false;
      }

      readConfigVals(f, "depth_to_image_transform:", 12, vals);
      depth_to_image.push_back(vecToAffine(vals));

      readConfigVals(f, "ref_to_sensor_timestamp_offset:", 1, vals);
      double timestamp_offset = vals[0];
      ref_to_cam_timestamp_offsets.push_back(timestamp_offset);
    }

    // Sanity check
    if (have_rig_transforms) {
      if (ref_to_cam_trans[0].matrix() != Eigen::Affine3d::Identity().matrix())
        LOG(FATAL) << "The transform from the reference sensor to itself must be the identity.\n";
    }
  } catch(std::exception const& e) {
    LOG(FATAL) << e.what() << "\n";
  }

  return;
}

}  // namespace dense_map

int main(int argc, char** argv) {
#if HAVE_ASTROBEE
  ff_common::InitFreeFlyerApplication(&argc, &argv);
  GOOGLE_PROTOBUF_VERIFY_VERSION;
#else
  google::InitGoogleLogging(argv[0]);
  google::ParseCommandLineFlags(&argc, &argv, true);
#endif

  std::cout << "--rig config " << FLAGS_rig_config << std::endl;

  dense_map::parameterValidation();

  // We assume that the first cam is the ref cam (its id is 0)
  int ref_cam_type = 0;

  // Image names
  std::vector<std::string> cam_names;
  std::vector<Eigen::Affine3d> depth_to_image;
  std::vector<camera::CameraParameters> cam_params;
  std::vector<Eigen::Affine3d>          ref_to_cam_trans;
  std::vector<double>                   ref_to_cam_timestamp_offsets;

  bool have_rig_transforms = false;
  if (FLAGS_rig_config != "")  // Read a plain text config file for n sensors
    dense_map::readRigConfig(FLAGS_rig_config, have_rig_transforms, ref_cam_type, cam_names,
                             cam_params, ref_to_cam_trans, depth_to_image,
                             ref_to_cam_timestamp_offsets);
  else  // Read a lua-based config file for an Astrobee robot with 3 sensors
    dense_map::readLuaConfig(have_rig_transforms, ref_cam_type, cam_names, cam_params,
                             ref_to_cam_trans, depth_to_image, ref_to_cam_timestamp_offsets);

  // This is needed to make the tool reading a rig config file agree
  // with it reading a lua config file
  // double f = cam_params[ref_cam_type].GetFocalLength();
  // cam_params[ref_cam_type].SetFocalLength(Eigen::Vector2d(f, f));

  int num_cam_types = cam_params.size();

  // Optionally override the timestamp offset
  if (!std::isnan(FLAGS_nav_cam_to_sci_cam_offset_override_value)) {
    for (size_t it = 0; it < cam_names.size(); it++) {
      if (cam_names[it] == "sci_cam") {
        double new_val = FLAGS_nav_cam_to_sci_cam_offset_override_value;
        std::cout << "Overriding the value " << ref_to_cam_timestamp_offsets[it]
                  << " of nav_cam_to_sci_cam_timestamp_offset with: " << new_val << std::endl;
        ref_to_cam_timestamp_offsets[it] = new_val;
      }
    }
  }

  // If desired to process only specific timestamps
  std::set<double> sci_cam_timestamps_to_use;
  if (FLAGS_sci_cam_timestamps != "") {
    std::ifstream ifs(FLAGS_sci_cam_timestamps.c_str());
    double val;
    while (ifs >> val) sci_cam_timestamps_to_use.insert(val);
  }

  // Save this in case we need to restore the original parameters
  std::vector<camera::CameraParameters> orig_cam_params = cam_params;

  // Optionally load the mesh
  mve::TriangleMesh::Ptr mesh;
  std::shared_ptr<mve::MeshInfo> mesh_info;
  std::shared_ptr<tex::Graph> graph;
  std::shared_ptr<BVHTree> bvh_tree;
  if (FLAGS_mesh != "") dense_map::loadMeshBuildTree(FLAGS_mesh, mesh, mesh_info, graph, bvh_tree);

  // world_to_ref has the transforms from the ref cameras to the world,
  // while world_to_cam has the transforms from the world to all cameras,
  // including world_to_ref. Both of these are needed in certain circumstances,
  // and it is very important to always keep these in sync.
  std::vector<Eigen::Affine3d> world_to_ref, world_to_cam;

  // The timestamp for the ref camera.
  std::vector<double> ref_timestamps;

#if HAVE_ASTROBEE
  // Read the sparse map. It has the ref cam poses.
  boost::shared_ptr<sparse_mapping::SparseMap> sparse_map =
    boost::shared_ptr<sparse_mapping::SparseMap>(new sparse_mapping::SparseMap(FLAGS_sparse_map));

  // Parse the ref timestamps from the sparse map. We assume that the
  // map image names are the timestamps.
  const std::vector<std::string>& sparse_map_images = sparse_map->cid_to_filename_;
  ref_timestamps.resize(sparse_map_images.size());
  for (size_t cid = 0; cid < sparse_map_images.size(); cid++)
    ref_timestamps[cid] = dense_map::fileNameToTimestamp(sparse_map_images[cid]);
  if (ref_timestamps.empty()) LOG(FATAL) << "No sparse map timestamps found.";

  // Will optimize the ref cam poses as part of the process
  // Make a copy of them here.
  world_to_ref = sparse_map->cid_to_cam_t_global_;
#endif

  std::vector<std::vector<dense_map::ImageMessage>> image_data;
  std::vector<std::vector<dense_map::ImageMessage>> depth_data;
  if (FLAGS_image_list != "")
    dense_map::readImageAndDepthData(  // Inputs
      FLAGS_image_list, ref_cam_type,
      // Outputs
      ref_timestamps, world_to_ref, image_data, depth_data);

  // Put transforms of the reference cameras in a vector. We will optimize them.
  int num_ref_cams = world_to_ref.size();
  if (world_to_ref.size() != ref_timestamps.size())
    LOG(FATAL) << "Must have as many ref cam timestamps as ref cameras.\n";
  std::vector<double> world_to_ref_vec(num_ref_cams * dense_map::NUM_RIGID_PARAMS);
  for (int cid = 0; cid < num_ref_cams; cid++)
    dense_map::rigid_transform_to_array(world_to_ref[cid],
                                        &world_to_ref_vec[dense_map::NUM_RIGID_PARAMS * cid]);

  // Need the identity transform for when the cam is the ref cam, and
  // have to have a placeholder for the right bracketing cam which won't be used.
  Eigen::Affine3d identity = Eigen::Affine3d::Identity();
  std::vector<double> identity_vec(dense_map::NUM_RIGID_PARAMS);
  dense_map::rigid_transform_to_array(identity, &identity_vec[0]);

  // Put all timestamps to use in a vector, in the same order as the cameras
  std::vector<std::set<double>> cam_timestamps_to_use = {std::set<double>(),
                                                         std::set<double>(),
                                                         sci_cam_timestamps_to_use};

  // Which intrinsics from which cameras to float
  std::vector<std::set<std::string>> intrinsics_to_float(num_cam_types);
  dense_map::parse_intrinsics_to_float(FLAGS_nav_cam_intrinsics_to_float, intrinsics_to_float[0]);
  dense_map::parse_intrinsics_to_float(FLAGS_haz_cam_intrinsics_to_float, intrinsics_to_float[1]);
  dense_map::parse_intrinsics_to_float(FLAGS_sci_cam_intrinsics_to_float, intrinsics_to_float[2]);

  std::string depth_to_image_name = "depth_to_image";
  std::set<std::string> extrinsics_to_float;
  dense_map::parse_extrinsics_to_float(cam_names, cam_names[ref_cam_type], depth_to_image_name,
                                       FLAGS_extrinsics_to_float, extrinsics_to_float);

  if (!FLAGS_affine_depth_to_image && FLAGS_float_scale &&
      extrinsics_to_float.find(depth_to_image_name) == extrinsics_to_float.end())
    LOG(FATAL) << "Cannot float the scale of depth_to_image_transform unless this "
               << "this is allowed as part of --extrinsics_to_float.\n";

  // Put the extrinsics in arrays, so we can optimize them
  std::vector<double> ref_to_cam_vec(num_cam_types * dense_map::NUM_RIGID_PARAMS);
  for (int cam_type = 0; cam_type < num_cam_types; cam_type++)
    dense_map::rigid_transform_to_array
      (ref_to_cam_trans[cam_type],
       &ref_to_cam_vec[dense_map::NUM_RIGID_PARAMS * cam_type]);

  // Set up the variable blocks to optimize for BracketedDepthError
  int num_depth_params = dense_map::NUM_RIGID_PARAMS;
  if (FLAGS_affine_depth_to_image) num_depth_params = dense_map::NUM_AFFINE_PARAMS;

  // Separate the initial scale. This is convenient if
  // cam_depth_to_image is scale * rotation + translation and if
  // it is desired to keep the scale fixed. In either case, the scale
  // will be multiplied back when needed.
  std::vector<double> depth_to_image_scales;
  for (int cam_type = 0; cam_type < num_cam_types; cam_type++) {
    double depth_to_image_scale
      = pow(depth_to_image[cam_type].matrix().determinant(), 1.0 / 3.0);
    depth_to_image[cam_type].linear() /= depth_to_image_scale;
    depth_to_image_scales.push_back(depth_to_image_scale);
  }

  // Put depth_to_image arrays, so we can optimize them
  std::vector<double> depth_to_image_vec(num_cam_types * num_depth_params);
  for (int cam_type = 0; cam_type < num_cam_types; cam_type++) {
    if (FLAGS_affine_depth_to_image)
      dense_map::affine_transform_to_array
        (depth_to_image[cam_type],
         &depth_to_image_vec[num_depth_params * cam_type]);
    else
      dense_map::rigid_transform_to_array
        (depth_to_image[cam_type],
         &depth_to_image_vec[num_depth_params * cam_type]);
  }

  // Put the intrinsics in arrays
  std::vector<double> focal_lengths(num_cam_types);
  std::vector<Eigen::Vector2d> optical_centers(num_cam_types);
  std::vector<Eigen::VectorXd> distortions(num_cam_types);
  for (int it = 0; it < num_cam_types; it++) {
    focal_lengths[it] = cam_params[it].GetFocalLength();  // average the two focal lengths
    optical_centers[it] = cam_params[it].GetOpticalOffset();

    if (cam_params[it].GetDistortion().size() == 0)
      LOG(FATAL) << "The cameras are expected to have distortion.";
    distortions[it] = cam_params[it].GetDistortion();
  }

  // Build a map for quick access for all the messages we may need
  std::vector<std::string> topics;
  dense_map::StrToMsgMap bag_map;
  std::vector<std::string> image_topics, depth_topics;
#if HAVE_ASTROBEE
  image_topics = {FLAGS_nav_cam_topic,
                  FLAGS_haz_cam_intensity_topic,
                  FLAGS_sci_cam_topic};
  depth_topics = {"", FLAGS_haz_cam_points_topic, ""};

  std::cout << "Reading the bag: " << FLAGS_ros_bag << std::endl;
  for (auto it = 0; it < image_topics.size(); it++)
    if (image_topics[it] != "") topics.push_back(image_topics[it]);
  for (auto it = 0; it < depth_topics.size(); it++)
    if (depth_topics[it] != "") topics.push_back(depth_topics[it]);
  rosbag::Bag bag;
  bag.open(FLAGS_ros_bag, rosbag::bagmode::Read);
  rosbag::View view(bag, rosbag::TopicQuery(topics));
  dense_map::indexMessages(view, bag_map);
#endif

  // Keep here the images, timestamps, and bracketing information
  std::vector<dense_map::cameraImage> cams;
  //  The range of ref_to_cam_timestamp_offsets[cam_type] before
  //  getting out of the bracket.
  std::vector<double> min_timestamp_offset, max_timestamp_offset;

  // Select the images to use from the bag
  dense_map::lookupImagesAndBrackets(  // Inputs
    ref_cam_type, FLAGS_bracket_len, FLAGS_timestamp_offsets_max_change,
    FLAGS_max_haz_cam_image_to_depth_timestamp_diff, cam_names,
    ref_timestamps, image_topics, depth_topics,
    bag_map, image_data, depth_data,
    cam_timestamps_to_use,
    ref_to_cam_timestamp_offsets,
    // Outputs
    cams, min_timestamp_offset, max_timestamp_offset);

  // The images from the bag may need to be resized to be the same
  // size as in the calibration file. Sometimes the full-res images
  // can be so blurry that interest point matching fails, hence the
  // resizing.
  for (size_t it = 0; it < cams.size(); it++)
    dense_map::adjustImageSize(cam_params[cams[it].camera_type], cams[it].image);

  // Sort by the timestamp in reference camera time. This is essential
  // for matching each image to other images close in time. Note
  // that this does not affect the book-keeping of beg_ref_index
  // and end_ref_it in this vector because those indices point to
  // world_to_ref and ref_timestamp, which do not change.
  std::sort(cams.begin(), cams.end(), dense_map::timestampLess);

  // If we have initial rig transforms, compute the transform from the
  // world to every camera based on the rig transforms and ref_to_cam
  // transforms. It assumes that world_to_ref_vec and ref_to_cam_vec
  // are up-to-date. Use the version of calc_world_to_cam_transforms
  // without world_to_cam_vec, on input which was not computed yet.
  if (have_rig_transforms) {
    dense_map::calc_world_to_cam_transforms(  // Inputs
                               cams, world_to_ref_vec, ref_timestamps, ref_to_cam_vec,
                               ref_to_cam_timestamp_offsets,
                               // Output
                               world_to_cam);
  } else {
    // TODO(oalexan1): Make this into a function
    // These were read with the images
    std::vector<int> start_pos(num_cam_types, 0);  // to help advance in time
    world_to_cam.resize(cams.size());
    for (size_t cam_it = 0; cam_it < cams.size(); cam_it++) {
      int cam_type = cams[cam_it].camera_type;
      for (size_t pos = start_pos[cam_type]; pos < image_data[cam_type].size(); pos++) {
        // Count on the fact that image_data[cam_type] is sorted chronologically
        if (cams[cam_it].timestamp == image_data[cam_type][pos].timestamp) {
          world_to_cam[cam_it] = image_data[cam_type][pos].world_to_cam;
          start_pos[cam_type] = pos;  // save for next time
        }
      }
    }
  }

  // TODO(oalexan1): Eliminate world_to_cam, use only world_to_cam_vec
  // If using no extrinsics, each camera will float separately, using
  // world_to_cam as initial guesses. Use world_to_cam_vec as storage
  // for the camera poses to optimize.
  std::vector<double> world_to_cam_vec;
  if (FLAGS_no_extrinsics) {
    world_to_cam_vec.resize(cams.size() * dense_map::NUM_RIGID_PARAMS);
    for (size_t cid = 0; cid < cams.size(); cid++)
      dense_map::rigid_transform_to_array(world_to_cam[cid],
                                          &world_to_cam_vec[dense_map::NUM_RIGID_PARAMS * cid]);
  }

  // The image file names are needed to save the images and matches
  std::vector<std::string> image_files, depth_files;
  if (FLAGS_save_images_and_depth_clouds || FLAGS_save_matches)
    dense_map::genImageAndDepthFileNames(  // Inputs
      cams, cam_names, FLAGS_out_dir,
      // Outputs
      image_files, depth_files);

  if (FLAGS_save_images_and_depth_clouds)
    dense_map::saveImagesAndDepthClouds(cams, image_files, depth_files);

  // Detect and match features
  std::vector<std::vector<std::pair<float, float>>> keypoint_vec;
  std::vector<std::map<int, int>> pid_to_cid_fid;
  dense_map::detectMatchFeatures(  // Inputs
    cams, cam_params, image_files, FLAGS_out_dir, FLAGS_save_matches, world_to_cam,
    FLAGS_num_overlaps, FLAGS_initial_max_reprojection_error, FLAGS_num_match_threads,
    FLAGS_verbose,
    // Outputs
    keypoint_vec, pid_to_cid_fid);

  // Set up the block sizes
  std::vector<int> bracketed_cam_block_sizes;
  std::vector<int> bracketed_depth_block_sizes;
  std::vector<int> bracketed_depth_mesh_block_sizes;
  std::vector<int> mesh_block_sizes;
  dense_map::set_up_block_sizes(num_depth_params,
                                bracketed_cam_block_sizes, bracketed_depth_block_sizes,
                                bracketed_depth_mesh_block_sizes, mesh_block_sizes);

  // For a given fid = pid_to_cid_fid[pid][cid], the value
  // pid_cid_fid_inlier[pid][cid][fid] will be non-zero only if this
  // pixel is an inlier. Originally all pixels are inliers. Once an
  // inlier becomes an outlier, it never becomes an inlier again.
  std::vector<std::map<int, std::map<int, int>>> pid_cid_fid_inlier;
  dense_map::flagOutlierByExclusionDist(  // Inputs
    ref_cam_type, FLAGS_nav_cam_num_exclude_boundary_pixels, cam_params, cams, pid_to_cid_fid,
    keypoint_vec,
    // Outputs
    pid_cid_fid_inlier);

  // Structures needed to intersect rays with the mesh
  std::vector<std::map<int, std::map<int, Eigen::Vector3d>>> pid_cid_fid_mesh_xyz;
  std::vector<Eigen::Vector3d> pid_mesh_xyz;
  Eigen::Vector3d bad_xyz(1.0e+100, 1.0e+100, 1.0e+100);  // use this to flag invalid xyz

  for (int pass = 0; pass < FLAGS_refiner_num_passes; pass++) {
    std::cout << "\nOptimization pass " << pass + 1 << " / " << FLAGS_refiner_num_passes << "\n";

    // The transforms from the world to all cameras must be updated
    // given the current state of optimization
    // TODO(oalexan1): The call below is likely not necessary since this function
    // is already called earlier, and also whenever a pass finishes, see below.
    dense_map::calc_world_to_cam_transforms(  // Inputs
      FLAGS_no_extrinsics, cams, world_to_ref_vec, ref_timestamps, ref_to_cam_vec, world_to_cam_vec,
      ref_to_cam_timestamp_offsets,
      // Output
      world_to_cam);

    std::vector<Eigen::Vector3d> xyz_vec;
    dense_map::multiViewTriangulation(  // Inputs
                                      cam_params, cams, world_to_cam, pid_to_cid_fid, keypoint_vec,
                                      // Outputs
                                      pid_cid_fid_inlier, xyz_vec);

    // Compute where each ray intersects the mesh
    if (FLAGS_mesh != "")
      dense_map::meshTriangulations(  // Inputs
                                    cam_params, cams, world_to_cam, pid_to_cid_fid,
        pid_cid_fid_inlier, keypoint_vec, bad_xyz, FLAGS_min_ray_dist, FLAGS_max_ray_dist, mesh,
        bvh_tree,
        // Outputs
        pid_cid_fid_mesh_xyz, pid_mesh_xyz);

    // For a given fid = pid_to_cid_fid[pid][cid],
    // the value pid_cid_fid_to_residual_index[pid][cid][fid] will be the index in the array
    // of residuals (look only at pixel residuals). This structure is populated only for
    // inliers, so its total number of elements changes at each pass.
    std::vector<std::map<int, std::map<int, int>>> pid_cid_fid_to_residual_index;
    pid_cid_fid_to_residual_index.resize(pid_to_cid_fid.size());

    // Form the problem
    ceres::Problem problem;
    std::vector<std::string> residual_names;
    std::vector<double> residual_scales;
    for (size_t pid = 0; pid < pid_to_cid_fid.size(); pid++) {
      for (auto cid_fid = pid_to_cid_fid[pid].begin();
           cid_fid != pid_to_cid_fid[pid].end(); cid_fid++) {
        int cid = cid_fid->first;
        int fid = cid_fid->second;

        // Deal with inliers only
        if (!dense_map::getMapValue(pid_cid_fid_inlier, pid, cid, fid))
          continue;

        int cam_type = cams[cid].camera_type;
        double beg_ref_timestamp = -1.0, end_ref_timestamp = -1.0, cam_timestamp = -1.0;

        // Pointers to bracketing cameras and ref to cam transform. Their precise
        // definition is spelled out below.
        double *beg_cam_ptr = NULL, *end_cam_ptr = NULL, *ref_to_cam_ptr = NULL;

        if (!FLAGS_no_extrinsics) {
          // Default behavior, model extrinsics, use timestamps
          int beg_ref_index = cams[cid].beg_ref_index;
          int end_ref_index = cams[cid].end_ref_index;

          // Left bracketing ref cam for a given cam. For a ref cam, this is itself.
          beg_cam_ptr = &world_to_ref_vec[dense_map::NUM_RIGID_PARAMS * beg_ref_index];

          // Right bracketing camera. When the cam is the ref type,
          // this is nominal and not used.
          if (cam_type == ref_cam_type)
            end_cam_ptr = &identity_vec[0];
          else
            end_cam_ptr = &world_to_ref_vec[dense_map::NUM_RIGID_PARAMS * end_ref_index];

          // The beg and end timestamps will be the same only for the
          // ref cam
          beg_ref_timestamp = ref_timestamps[beg_ref_index];
          end_ref_timestamp = ref_timestamps[end_ref_index];
          cam_timestamp = cams[cid].timestamp;  // uses current camera's clock

        } else {
          // No extrinsics. Then, beg_cam_ptr is just current camera,
          // not the ref bracket, end_cam_ptr is the identity and
          // fixed. The beg and end timestamps are declared to be
          // same, which will be used in calc_world_to_cam_trans() to
          // ignore extrinsics end end_cam_ptr.
          cam_timestamp     = cams[cid].timestamp;
          beg_ref_timestamp = cam_timestamp;
          end_ref_timestamp = cam_timestamp;

          // Note how we use world_to_cam_vec and not world_to_ref_vec
          beg_cam_ptr  = &world_to_cam_vec[dense_map::NUM_RIGID_PARAMS * cid];
          end_cam_ptr = &identity_vec[0];
        }

        // Transform from reference camera to given camera. Won't be used when
        // FLAGS_no_extrinsics is true or when the cam is of ref type.
        ref_to_cam_ptr = &ref_to_cam_vec[dense_map::NUM_RIGID_PARAMS * cam_type];

        Eigen::Vector2d dist_ip(keypoint_vec[cid][fid].first, keypoint_vec[cid][fid].second);

        ceres::CostFunction* bracketed_cost_function =
          dense_map::BracketedCamError::Create(dist_ip, beg_ref_timestamp, end_ref_timestamp,
                                               cam_timestamp, bracketed_cam_block_sizes,
                                               cam_params[cam_type]);
        ceres::LossFunction* bracketed_loss_function
          = dense_map::GetLossFunction("cauchy", FLAGS_robust_threshold);

        // Remember the index of the residuals about to create
        pid_cid_fid_to_residual_index[pid][cid][fid] = residual_names.size();

        residual_names.push_back(cam_names[cam_type] + "_pix_x");
        residual_names.push_back(cam_names[cam_type] + "_pix_y");
        residual_scales.push_back(1.0);
        residual_scales.push_back(1.0);
        problem.AddResidualBlock
          (bracketed_cost_function, bracketed_loss_function,
           beg_cam_ptr, end_cam_ptr, ref_to_cam_ptr, &xyz_vec[pid][0],
           &ref_to_cam_timestamp_offsets[cam_type],
           &focal_lengths[cam_type], &optical_centers[cam_type][0], &distortions[cam_type][0]);

        // See which intrinsics to float
        if (intrinsics_to_float[cam_type].find("focal_length") ==
            intrinsics_to_float[cam_type].end())
          problem.SetParameterBlockConstant(&focal_lengths[cam_type]);
        if (intrinsics_to_float[cam_type].find("optical_center") ==
            intrinsics_to_float[cam_type].end())
          problem.SetParameterBlockConstant(&optical_centers[cam_type][0]);
        if (intrinsics_to_float[cam_type].find("distortion") == intrinsics_to_float[cam_type].end())
          problem.SetParameterBlockConstant(&distortions[cam_type][0]);

        // When the camera is the ref type, the right bracketing
        // camera is just a placeholder which is not used, hence
        // should not be optimized. Same for the ref_to_cam_vec and
        // ref_to_cam_timestamp_offsets, etc., as can be seen further
        // down.
        if (!FLAGS_no_extrinsics) {
          // See if to float the ref cameras
          if (!FLAGS_float_sparse_map)
            problem.SetParameterBlockConstant(beg_cam_ptr);
        } else {
          // There are no extrinsics. Then beg_cam_ptr refers to camera
          // for cams[cid], and not to its ref bracketing cam.
          // Use --float_sparse_map to float it if it is a ref cam,
          // and --float_nonref_cameras if it is not a ref cam.
          if ((cam_type == ref_cam_type && !FLAGS_float_sparse_map) ||
              (cam_type != ref_cam_type && !FLAGS_float_nonref_cameras))
            problem.SetParameterBlockConstant(beg_cam_ptr);
        }

        // The end cam floats only if told to, and if it brackets
        // a given non-ref cam.
        if (!FLAGS_float_sparse_map || cam_type == ref_cam_type || FLAGS_no_extrinsics)
          problem.SetParameterBlockConstant(end_cam_ptr);

        if (!FLAGS_float_timestamp_offsets || cam_type == ref_cam_type || FLAGS_no_extrinsics) {
          // Either we don't float timestamp offsets at all, or the cam is the ref type,
          // or with no extrinsics, when it can't float anyway.
          problem.SetParameterBlockConstant(&ref_to_cam_timestamp_offsets[cam_type]);
        } else {
          problem.SetParameterLowerBound(&ref_to_cam_timestamp_offsets[cam_type], 0,
                                         min_timestamp_offset[cam_type]);
          problem.SetParameterUpperBound(&ref_to_cam_timestamp_offsets[cam_type], 0,
                                         max_timestamp_offset[cam_type]);
        }
        // ref_to_cam is kept fixed at the identity if the cam is the ref type or
        // no extrinsics
        if (extrinsics_to_float.find(cam_names[cam_type]) == extrinsics_to_float.end() ||
            cam_type == ref_cam_type || FLAGS_no_extrinsics) {
          problem.SetParameterBlockConstant(ref_to_cam_ptr);
        }

        Eigen::Vector3d depth_xyz(0, 0, 0);
        bool have_depth_tri_constraint
          = (FLAGS_depth_tri_weight > 0 &&
             dense_map::depthValue(cams[cid].depth_cloud, dist_ip, depth_xyz));

        if (have_depth_tri_constraint) {
          // Ensure that the depth points agree with triangulated points
          ceres::CostFunction* bracketed_depth_cost_function
            = dense_map::BracketedDepthError::Create(FLAGS_depth_tri_weight, depth_xyz,
                                                     beg_ref_timestamp, end_ref_timestamp,
                                                     cam_timestamp, bracketed_depth_block_sizes);

          ceres::LossFunction* bracketed_depth_loss_function
            = dense_map::GetLossFunction("cauchy", FLAGS_robust_threshold);

          residual_names.push_back("depth_tri_x_m");
          residual_names.push_back("depth_tri_y_m");
          residual_names.push_back("depth_tri_z_m");
          residual_scales.push_back(FLAGS_depth_tri_weight);
          residual_scales.push_back(FLAGS_depth_tri_weight);
          residual_scales.push_back(FLAGS_depth_tri_weight);
          problem.AddResidualBlock
            (bracketed_depth_cost_function, bracketed_depth_loss_function,
             beg_cam_ptr, end_cam_ptr, ref_to_cam_ptr,
             &depth_to_image_vec[num_depth_params * cam_type],
             &depth_to_image_scales[cam_type],
             &xyz_vec[pid][0],
             &ref_to_cam_timestamp_offsets[cam_type]);

          // Note that above we already considered fixing some params.
          // We won't repeat that code here.
          // If we model an affine depth to image, fix its scale here,
          // it will change anyway as part of depth_to_image_vec.
          if (!FLAGS_float_scale || FLAGS_affine_depth_to_image) {
            problem.SetParameterBlockConstant(&depth_to_image_scales[cam_type]);
          }

          if (extrinsics_to_float.find(depth_to_image_name) == extrinsics_to_float.end()) {
            problem.SetParameterBlockConstant(
              &depth_to_image_vec[num_depth_params * cam_type]);
          }
        }

        // Add the depth to mesh constraint
        bool have_depth_mesh_constraint = false;
        depth_xyz = Eigen::Vector3d(0, 0, 0);
        Eigen::Vector3d mesh_xyz(0, 0, 0);
        if (FLAGS_mesh != "") {
          mesh_xyz = dense_map::getMapValue(pid_cid_fid_mesh_xyz, pid, cid, fid);
          have_depth_mesh_constraint
            = (FLAGS_depth_mesh_weight > 0 && mesh_xyz != bad_xyz &&
               dense_map::depthValue(cams[cid].depth_cloud, dist_ip, depth_xyz));
        }

        if (have_depth_mesh_constraint) {
          // Try to make each mesh intersection agree with corresponding depth measurement,
          // if it exists
          ceres::CostFunction* bracketed_depth_mesh_cost_function
            = dense_map::BracketedDepthMeshError::Create
            (FLAGS_depth_mesh_weight, depth_xyz, mesh_xyz, beg_ref_timestamp,
             end_ref_timestamp, cam_timestamp, bracketed_depth_mesh_block_sizes);

          ceres::LossFunction* bracketed_depth_mesh_loss_function
            = dense_map::GetLossFunction("cauchy", FLAGS_robust_threshold);

          residual_names.push_back("depth_mesh_x_m");
          residual_names.push_back("depth_mesh_y_m");
          residual_names.push_back("depth_mesh_z_m");
          residual_scales.push_back(FLAGS_depth_mesh_weight);
          residual_scales.push_back(FLAGS_depth_mesh_weight);
          residual_scales.push_back(FLAGS_depth_mesh_weight);
          problem.AddResidualBlock
            (bracketed_depth_mesh_cost_function, bracketed_depth_mesh_loss_function,
             beg_cam_ptr, end_cam_ptr, ref_to_cam_ptr,
             &depth_to_image_vec[num_depth_params * cam_type],
             &depth_to_image_scales[cam_type],
             &ref_to_cam_timestamp_offsets[cam_type]);

          // Note that above we already fixed some of these variables.
          // Repeat the fixing of depth variables, however, as the previous block
          // may not take place.
          if (!FLAGS_float_scale || FLAGS_affine_depth_to_image)
            problem.SetParameterBlockConstant(&depth_to_image_scales[cam_type]);

          if (extrinsics_to_float.find(depth_to_image_name) == extrinsics_to_float.end())
            problem.SetParameterBlockConstant
              (&depth_to_image_vec[num_depth_params * cam_type]);
        }
      }  // end iterating over all cid for given pid

      // This constraint will be for the pid
      bool have_mesh_tri_constraint = false;
      Eigen::Vector3d avg_mesh_xyz(0, 0, 0);
      if (FLAGS_mesh != "") {
        avg_mesh_xyz = pid_mesh_xyz.at(pid);
        if (FLAGS_mesh_tri_weight > 0 && avg_mesh_xyz != bad_xyz) have_mesh_tri_constraint = true;
      }
      if (have_mesh_tri_constraint) {
        // Try to make the triangulated point agree with the mesh intersection

        ceres::CostFunction* mesh_cost_function =
          dense_map::XYZError::Create(avg_mesh_xyz, mesh_block_sizes, FLAGS_mesh_tri_weight);

        ceres::LossFunction* mesh_loss_function =
          dense_map::GetLossFunction("cauchy", FLAGS_robust_threshold);

        problem.AddResidualBlock(mesh_cost_function, mesh_loss_function,
                                 &xyz_vec[pid][0]);

        residual_names.push_back("mesh_tri_x_m");
        residual_names.push_back("mesh_tri_y_m");
        residual_names.push_back("mesh_tri_z_m");
        residual_scales.push_back(FLAGS_mesh_tri_weight);
        residual_scales.push_back(FLAGS_mesh_tri_weight);
        residual_scales.push_back(FLAGS_mesh_tri_weight);
      }
    }  // end iterating over pid

    // Evaluate the residuals before optimization
    std::vector<double> residuals;
    dense_map::evalResiduals("before opt", residual_names, residual_scales, problem, residuals);

    // Solve the problem
    ceres::Solver::Options options;
    ceres::Solver::Summary summary;
    options.linear_solver_type = ceres::ITERATIVE_SCHUR;
    options.num_threads = FLAGS_num_opt_threads;  // The result is more predictable with one thread
    options.max_num_iterations = FLAGS_num_iterations;
    options.minimizer_progress_to_stdout = true;
    options.gradient_tolerance = 1e-16;
    options.function_tolerance = 1e-16;
    options.parameter_tolerance = FLAGS_parameter_tolerance;
    ceres::Solve(options, &problem, &summary);

    // The optimization is done. Right away copy the optimized states
    // to where they belong to keep all data in sync.

    if (!FLAGS_no_extrinsics) {
      // Copy back the reference transforms
      for (int cid = 0; cid < num_ref_cams; cid++)
        dense_map::array_to_rigid_transform
          (world_to_ref[cid], &world_to_ref_vec[dense_map::NUM_RIGID_PARAMS * cid]);
    } else {
      // Each camera floats individually. Update world_to_cam from optimized world_to_cam_vec.
      for (size_t cid = 0; cid < cams.size(); cid++) {
        dense_map::array_to_rigid_transform
          (world_to_cam[cid], &world_to_cam_vec[dense_map::NUM_RIGID_PARAMS * cid]);
        // Update world_to_ref as well, as it is part of the sparse map
        if (cams[cid].camera_type == ref_cam_type) {
          int ref_cid = cams[cid].beg_ref_index;
          world_to_ref[ref_cid] = world_to_cam[cid];
          // For consistency, also keep world_to_ref_vec updated, even
          // if it is not used in this case
          dense_map::rigid_transform_to_array
            (world_to_ref[ref_cid], &world_to_ref_vec[dense_map::NUM_RIGID_PARAMS * ref_cid]);
        }
      }
    }

    // Copy back the optimized intrinsics
    for (int it = 0; it < num_cam_types; it++) {
      cam_params[it].SetFocalLength(Eigen::Vector2d(focal_lengths[it], focal_lengths[it]));
      cam_params[it].SetOpticalOffset(optical_centers[it]);
      cam_params[it].SetDistortion(distortions[it]);
    }

    // If the nav cam did not get optimized, go back to the solution
    // with two focal lengths, rather than the one with one focal length
    // solved by this solver (as the average of the two). The two focal
    // lengths are very similar, but it is not worth modifying the
    // camera model we don't plan to optimize.
    if (FLAGS_nav_cam_intrinsics_to_float == "" || FLAGS_num_iterations == 0)
      cam_params[ref_cam_type] = orig_cam_params[ref_cam_type];

    // Copy back the optimized extrinsics, whether it was optimized or fixed
    for (int cam_type = 0; cam_type < num_cam_types; cam_type++)
      dense_map::array_to_rigid_transform
        (ref_to_cam_trans[cam_type],
         &ref_to_cam_vec[dense_map::NUM_RIGID_PARAMS * cam_type]);

    // Copy back the depth to image transforms without scales
    for (int cam_type = 0; cam_type < num_cam_types; cam_type++) {
      if (FLAGS_affine_depth_to_image)
        dense_map::array_to_affine_transform
          (depth_to_image[cam_type],
           &depth_to_image_vec[num_depth_params * cam_type]);
      else
        dense_map::array_to_rigid_transform(
          depth_to_image[cam_type],
          &depth_to_image_vec[num_depth_params * cam_type]);
    }

    // Evaluate the residuals after optimization
    dense_map::evalResiduals("after opt", residual_names, residual_scales, problem, residuals);

    // Must have up-to-date world_to_cam and residuals to flag the outliers
    dense_map::calc_world_to_cam_transforms(  // Inputs
      FLAGS_no_extrinsics, cams, world_to_ref_vec, ref_timestamps, ref_to_cam_vec, world_to_cam_vec,
      ref_to_cam_timestamp_offsets,
      // Output
      world_to_cam);

    // Flag outliers after this pass
    dense_map::flagOutliersByTriAngleAndReprojErr(  // Inputs
        FLAGS_refiner_min_angle, FLAGS_max_reprojection_error, pid_to_cid_fid, keypoint_vec,
        world_to_cam, xyz_vec, pid_cid_fid_to_residual_index, residuals,
        // Outputs
        pid_cid_fid_inlier);
  }  // End optimization passes

  // Put back the scale in depth_to_image
  for (int cam_type = 0; cam_type < num_cam_types; cam_type++)
    depth_to_image[cam_type].linear() *= depth_to_image_scales[cam_type];

  if (FLAGS_save_matches)
    dense_map::saveInlinerMatchPairs(image_files, FLAGS_num_overlaps, pid_to_cid_fid,
                                     keypoint_vec, pid_cid_fid_inlier, FLAGS_out_dir);

#if HAVE_ASTROBEE
  // Copy back the optimized ref map poses
  sparse_map->cid_to_cam_t_global_ = world_to_ref;

  bool map_changed = (FLAGS_num_iterations > 0 &&
                      (FLAGS_float_sparse_map || FLAGS_nav_cam_intrinsics_to_float != ""));
  if (map_changed) {
    // Rebuild the map. This does not change the poses.
    std::cout << "Either the sparse map intrinsics or cameras got modified. "
              << "The map must be rebuilt." << std::endl;
    dense_map::RebuildMap(FLAGS_output_map,  // Will be used for temporary saving of aux data
                          cam_params[ref_cam_type],
                          FLAGS_refiner_min_angle, FLAGS_verbose,
                          sparse_map);
  }

  if (FLAGS_registration) {
    std::cout << "Redoing registration of the obtained map and adjusting all extrinsics.\n";
    std::vector<std::string> data_files;
    data_files.push_back(FLAGS_hugin_file);
    data_files.push_back(FLAGS_xyz_file);
    bool verification = false;
    double map_scale
      = sparse_mapping::RegistrationOrVerification(data_files, verification, sparse_map.get());

    std::cout << "Registration resulted in a scale adjustment of: " << map_scale << ".\n";
    // We do not change depth_to_image_scales since multiplying the
    // affine component of depth_to_image is enough.
    for (int cam_type = 0; cam_type < num_cam_types; cam_type++) {
      // This transform is affine, so both the linear and translation parts need a scale
      depth_to_image[cam_type].linear() *= map_scale;
      depth_to_image[cam_type].translation() *= map_scale;
      // This is a rotation + translation, so only the translation needs the scale
      ref_to_cam_trans[cam_type].translation() *= map_scale;

      // TODO(oalexan1): This messes up the sparse map, which will likely require updating
      // world_to_ref_vec, and maybe other things.  First pull the camera transforms
      // from the registered sparse maps, and see what that affects.
      // Also consider the cases when we model or not the transform among sensors.

      // This is a bug if further changes are not done here, when registration is happening.
      LOG(FATAL) << "This is a bug. Registration is failing unless world_to_ref_vec "
                 << "and other quantities are updated.\n";

      // Update ref_to_cam_vec
      dense_map::rigid_transform_to_array(
        ref_to_cam_trans[cam_type],                                // input
        &ref_to_cam_vec[dense_map::NUM_RIGID_PARAMS * cam_type]);  // output
    }
  }

  std::cout << "Writing: " << FLAGS_output_map << std::endl;
  sparse_map->Save(FLAGS_output_map);

  // Update the config file
  dense_map::writeLuaConfig(cam_names, cam_params, ref_to_cam_trans,
                             ref_to_cam_timestamp_offsets,
                             depth_to_image);
#endif

  // Update the transforms from the world to every camera
  dense_map::calc_world_to_cam_transforms(  // Inputs
    FLAGS_no_extrinsics, cams, world_to_ref_vec, ref_timestamps, ref_to_cam_vec, world_to_cam_vec,
    ref_to_cam_timestamp_offsets,
    // Output
    world_to_cam);

  if (FLAGS_out_texture_dir != "") {
    // Project each image onto the mesh

    if (FLAGS_mesh == "")
      LOG(FATAL) << "Cannot project camera images onto a mesh if a mesh was not provided.\n";

    // TODO(oalexan1): Why the call below works without dense_map:: prepended to it?
    // TODO(oalexan1): This call to calc_world_to_cam_transforms is likely not
    // necessary since world_to_cam has been updated by now.
    dense_map::meshProjectCameras(cam_names, cam_params, cams, world_to_cam, mesh, bvh_tree,
                                  ref_cam_type,
                                  FLAGS_nav_cam_num_exclude_boundary_pixels,
                                  FLAGS_out_texture_dir);
  }

  if (FLAGS_save_images_and_depth_clouds) {
    dense_map::writeImageList(FLAGS_out_dir, cams, image_files, depth_files, world_to_cam);

    bool model_rig = (!FLAGS_no_extrinsics);
    dense_map::writeRigConfig(FLAGS_out_dir, model_rig, ref_cam_type, cam_names,
                              cam_params, ref_to_cam_trans, depth_to_image,
                              ref_to_cam_timestamp_offsets);
  }

  return 0;
} // NOLINT // TODO(oalexan1): Remove this, after making the code more modular
