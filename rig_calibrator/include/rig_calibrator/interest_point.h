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

#ifndef INTEREST_POINT_H_
#define INTEREST_POINT_H_

#include <opencv2/imgproc.hpp>
#include <glog/logging.h>

#include <Eigen/Core>
#include <Eigen/Geometry>

#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <utility>

namespace camera {
  // Forward declaration
  class CameraParameters;
}

namespace dense_map {

// Forward declarations
class cameraImage;
class ImageMessage;
  
/// A class for storing information about an interest point
/// in the format the NASA ASP can read it (very useful
// for visualization)
struct InterestPoint {
  typedef std::vector<float> descriptor_type;
  typedef descriptor_type::iterator iterator;
  typedef descriptor_type::const_iterator const_iterator;

  // The best way of creating interest points is:
  // ip = InterestPoint(x, y). But in the worst case, when the user
  // chooses to simply create a blank interest point, like
  // InterestPoint ip;
  // at least make sure that its members are always initialized,
  // as seen in the constructor below.
  // TODO(Oleg): There is no way to enforce that ix be in sync with x or
  // iy with y.
  InterestPoint(float x = 0, float y = 0, float scale = 1.0, float interest = 0.0,
                float ori = 0.0, bool pol = false,
                uint32_t octave = 0, uint32_t scale_lvl = 0)
      : x(x), y(y), scale(scale), ix(int32_t(x)), iy(int32_t(y)),
        orientation(ori), interest(interest), polarity(pol),
        octave(octave), scale_lvl(scale_lvl) {}

  /// Subpixel (col, row) location of point
  float x, y;

  /// Scale of point.  This may come from the pyramid level, from
  /// interpolating the interest function between levels, or from some
  /// other scale detector like the Laplace scale used by Mikolajczyk & Schmid
  float scale;

  /// Integer location (unnormalized), mainly for internal use.
  int32_t ix;
  int32_t iy;

  /// Since the orientation is not necessarily unique, we may have more
  /// than one hypothesis for the orientation of an interest point.  I
  /// considered making a vector of orientations for a single point.
  /// However, it is probably better to make more than one interest
  /// point with the same (x,y,s) since the descriptor will be unique
  /// for a given orientation anyway.
  float orientation;

  /// The interest measure (could be Harris, LoG, etc.).
  float interest;

  /// These are some extras for SURF-like implementations
  bool polarity;
  /// This is the integer location in scale space (used for indexing
  /// a vector of interest images)
  uint32_t octave, scale_lvl;

  /// And finally the descriptor for the interest point.  For example,
  /// PCA descriptors would have a vector of floats or doubles...
  descriptor_type descriptor;

  const_iterator begin() const { return descriptor.begin(); }
  iterator begin() { return descriptor.begin(); }
  const_iterator end() const { return descriptor.end(); }
  iterator end() { return descriptor.end(); }

  size_t size() const { return descriptor.size(); }
  float operator[](int index) { return descriptor[index]; }

  /// std::sort can be used to sort InterestPoints in descending
  /// order of interest.
  bool operator<(const InterestPoint& other) const { return (other.interest < interest); }

  /// Generates a human readable string
  std::string to_string() const {
    std::stringstream s;
    s << "IP: (" << x << "," << y << ")  scale: " << scale << "  orientation: " << orientation
      << "  interest: " << interest << "  polarity: " << polarity << "  octave: " << octave
      << "  scale_lvl: " << scale_lvl << "\n";
    s << "  descriptor: ";
    for (size_t i = 0; i < descriptor.size(); ++i) s << descriptor[i] << "  ";
    s << std::endl;
    return s.str();
  }

  /// Copy IP information from an OpenCV KeyPoint object.
  void setFromCvKeypoint(Eigen::Vector2d const& key, cv::Mat const& cv_descriptor) {
    x = key[0];
    y = key[1];
    ix = round(x);
    iy = round(y);
    interest = 0;
    octave = 0;
    scale_lvl = 1;
    scale = 1;
    orientation = 0;
    polarity = false;

    if (cv_descriptor.rows != 1 || cv_descriptor.cols < 2)
      LOG(FATAL) << "The descriptors must be in one row, and have at least two columns.";

    descriptor.resize(cv_descriptor.cols);
    for (size_t it = 0; it < descriptor.size(); it++) {
      descriptor[it] = cv_descriptor.at<float>(0, it);
    }
  }
};  // End class InterestPoint

  
typedef std::pair<std::vector<InterestPoint>, std::vector<InterestPoint> > MATCH_PAIR;
typedef std::map<std::pair<int, int>, dense_map::MATCH_PAIR> MATCH_MAP;

void detectFeatures(const cv::Mat& image, bool verbose,
                    // Outputs
                    cv::Mat* descriptors, Eigen::Matrix2Xd* keypoints);

// This really likes haz cam first and nav cam second
void matchFeatures(std::mutex* match_mutex, int left_image_index, int right_image_index,
                   cv::Mat const& left_descriptors, cv::Mat const& right_descriptors,
                   Eigen::Matrix2Xd const& left_keypoints,
                   Eigen::Matrix2Xd const& right_keypoints, bool verbose,
                   // Output
                   MATCH_PAIR* matches);

// Form the match file name. Assume the input images are of the form
// cam_name/image.jpg Keep the name of the cameras as part of the
// match file name, to avoid the case when two different cameras have
// images with the same name.
std::string matchFileName(std::string const& match_dir,
                          std::string const& left_image, std::string const& right_image,
                          std::string const& suffix);

// Routines for reading & writing interest point match files
void writeMatchFile(std::string match_file, std::vector<InterestPoint> const& ip1,
                    std::vector<InterestPoint> const& ip2);

// TODO(oalexan1): Duplicate code
void Triangulate(bool rm_invalid_xyz, double focal_length,
                 std::vector<Eigen::Affine3d> const& cid_to_cam_t_global,
                 std::vector<Eigen::Matrix2Xd> const& cid_to_keypoint_map,
                 std::vector<std::map<int, int> > * pid_to_cid_fid,
                 std::vector<Eigen::Vector3d> * pid_to_xyz);
  
// Triangulate two rays emanating from given undistorted and centered pixels
Eigen::Vector3d TriangulatePair(double focal_length1, double focal_length2,
                                Eigen::Affine3d const& world_to_cam1,
                                Eigen::Affine3d const& world_to_cam2, Eigen::Vector2d const& pix1,
                                Eigen::Vector2d const& pix2);

// Triangulate n rays emanating from given undistorted and centered pixels
Eigen::Vector3d Triangulate(std::vector<double> const& focal_length_vec,
                            std::vector<Eigen::Affine3d> const& world_to_cam_vec,
                            std::vector<Eigen::Vector2d> const& pix_vec);

// Apply a given transform to the given set of cameras.
// We assume that the transform is of the form
// T(x) = scale * rotation * x + translation
void TransformCameras(Eigen::Affine3d const& T, std::vector<Eigen::Affine3d> &world_to_cam);
  
// Apply same transform as above to points
void TransformPoints(Eigen::Affine3d const& T, std::vector<Eigen::Vector3d> *xyz);

// Apply a registration transform to a rig. The only thing that
// changes is scale, as the rig transforms are between coordinate
// systems of various cameras.
void TransformRig(Eigen::Affine3d const& T, std::vector<Eigen::Affine3d> & ref_to_cam_trans);
  
// Find the 3D transform from an abstract coordinate system to the
// world, given control points (pixel matches) and corresponding 3D
// measurements. It is assumed all images are from the reference
// camera.
Eigen::Affine3d registrationTransform(std::string const& hugin_file, std::string const& xyz_file,
                                      camera::CameraParameters const& ref_cam_params,
                                      std::vector<std::string> const& cid_to_filename,
                                      std::vector<Eigen::Affine3d>  & world_to_cam_trans); 
  
struct cameraImage;

void detectMatchFeatures(// Inputs
                         std::vector<dense_map::cameraImage> const& cams,
                         std::vector<camera::CameraParameters> const& cam_params,
                         std::string const& out_dir, bool save_matches,
                         std::vector<Eigen::Affine3d> const& world_to_cam, int num_overlaps,
                         int initial_max_reprojection_error, int num_match_threads,
                         bool verbose,
                         // Outputs
                         std::vector<std::vector<std::pair<float, float>>>& keypoint_vec,
                         std::vector<std::map<int, int>>& pid_to_cid_fid);

void multiViewTriangulation(// Inputs
                            std::vector<camera::CameraParameters>  const& cam_params,
                            std::vector<dense_map::cameraImage>    const& cams,
                            std::vector<Eigen::Affine3d>           const& world_to_cam,
                            std::vector<std::map<int, int>>        const& pid_to_cid_fid,
                            std::vector<std::vector<std::pair<float, float>>> const& keypoint_vec,
                            // Outputs
                            std::vector<std::map<int, std::map<int, int>>>& pid_cid_fid_inlier,
                            std::vector<Eigen::Vector3d>& xyz_vec);

// Given all the merged and filtered tracks in pid_cid_fid, for each
// image pair cid1 and cid2 with cid1 < cid2 < cid1 + num_overlaps + 1,
// save the matches of this pair which occur in the set of tracks.
void saveInlinerMatchPairs(// Inputs
                           std::vector<dense_map::cameraImage> const& cams,
                           int num_overlaps,
                           std::vector<std::map<int, int>> const& pid_to_cid_fid,
                           std::vector<std::vector<std::pair<float, float>>>
                           const& keypoint_vec,
                           std::vector<std::map<int, std::map<int, int>>>
                           const& pid_cid_fid_inlier,
                           std::string const& out_dir);

// Read cameras and interest points from an nvm file  
void ReadNVM(std::string const& input_filename,
             std::vector<Eigen::Matrix2Xd> * cid_to_keypoint_map,
             std::vector<std::string> * cid_to_filename,
             std::vector<std::map<int, int>> * pid_to_cid_fid,
             std::vector<Eigen::Vector3d> * pid_to_xyz,
             std::vector<Eigen::Affine3d> *
             cid_to_cam_t_global);

// Write the inliers in nvm format. The keypoints are shifted relative to the optical
// center, as written by Theia.
void writeNvm(std::string                                       const& nvm_file,
              std::vector<camera::CameraParameters>             const& cam_params,
              std::vector<dense_map::cameraImage>               const& cams,
              std::vector<Eigen::Affine3d>                      const& world_to_cam,
              std::vector<std::vector<std::pair<float, float>>> const& keypoint_vec,
              std::vector<std::map<int, int>>                   const& pid_to_cid_fid,
              std::vector<std::map<int, std::map<int, int>>>    const& pid_cid_fid_inlier,
              std::vector<Eigen::Vector3d>                      const& xyz_vec);
  
// Write an nvm file. Note that a single focal length is assumed and no distortion.
// Those are ignored, and only camera poses, matches, and keypoints are used.
void WriteNVM(std::vector<Eigen::Matrix2Xd> const& cid_to_keypoint_map,
              std::vector<std::string> const& cid_to_filename,
              std::vector<double> const& focal_lengths,
              std::vector<std::map<int, int>> const& pid_to_cid_fid,
              std::vector<Eigen::Vector3d> const& pid_to_xyz,
              std::vector<Eigen::Affine3d> const& cid_to_cam_t_global,
              std::string const& output_filename);

struct nvmData {
  std::vector<Eigen::Matrix2Xd>    cid_to_keypoint_map;
  std::vector<std::string>         cid_to_filename;
  std::vector<std::map<int, int>>  pid_to_cid_fid;
  std::vector<Eigen::Vector3d>     pid_to_xyz;
  std::vector<Eigen::Affine3d>     cid_to_cam_t_global;
};

// Write an image with 3 floats per pixel. OpenCV's imwrite() cannot do that.
void saveXyzImage(std::string const& filename, cv::Mat const& img);

// Save images and depth clouds to disk
void saveImagesAndDepthClouds(std::vector<cameraImage> const& cams);

// Read an image with 3 floats per pixel. OpenCV's imread() cannot do that.
void readXyzImage(std::string const& filename, cv::Mat & img);

void readCameraPoses(// Inputs
                     std::string const& camera_poses_file, int ref_cam_type,
                     std::vector<std::string> const& cam_names,
                     // Outputs
                     nvmData & nvm,
                     std::vector<double>& ref_timestamps,
                     std::vector<Eigen::Affine3d>& world_to_ref,
                     std::vector<std::string> & ref_image_files,
                     std::vector<std::vector<ImageMessage>>& image_data,
                     std::vector<std::vector<ImageMessage>>& depth_data);
  
// Read camera information and images from an NVM file, exported
// from Theia
void readNvm(// Inputs
             std::string const& nvm_file, int ref_cam_type,
             std::vector<std::string> const& cam_names,
             // Outputs
             nvmData & nvm,
             std::vector<double>& ref_timestamps,
             std::vector<Eigen::Affine3d>& world_to_ref,
             std::vector<std::string>    & ref_image_files,
             std::vector<std::vector<ImageMessage>>& image_data,
             std::vector<std::vector<ImageMessage>>& depth_data);
  
// Append to existing keypoints and pid_to_cid_fid the entries from the nvm file.  
// Need to account for the fact that the nvm file will likely have the images
// in different order than in the 'cams' vector, and may have more such images,
// as later we may have used bracketing to thin them out. So, some book-keeping is
// necessary.
void appendMatchesFromNvm(// Inputs
                          std::vector<camera::CameraParameters> const& cam_params,
                          std::vector<dense_map::cameraImage>   const& cams,
                          nvmData const& nvm,
                          // Outputs (these get appended to)
                          std::vector<std::map<int, int>> & pid_to_cid_fid,
                          std::vector<std::vector<std::pair<float, float>>> & keypoint_vec);
  
}  // namespace dense_map

#endif  // INTEREST_POINT_H_
