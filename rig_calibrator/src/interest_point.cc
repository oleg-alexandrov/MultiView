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

#include <opencv2/xfeatures2d.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/calib3d/calib3d.hpp>

#include <rig_calibrator/basic_algs.h>
#include <rig_calibrator/interest_point.h>
#include <rig_calibrator/camera_image.h>
#include <rig_calibrator/system_utils.h>
#include <rig_calibrator/thread.h>
#include <rig_calibrator/matching.h>
#include <rig_calibrator/transform_utils.h>
#include <camera_model/camera_params.h>

// Get rid of warnings beyond our control
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic push
#include <openMVG/multiview/projection.hpp>
#include <openMVG/multiview/triangulation_nview.hpp>
#include <openMVG/tracks/tracks.hpp>
#pragma GCC diagnostic pop

#include <boost/filesystem.hpp>

#include <iostream>
#include <fstream>
#include <iomanip>

namespace fs = boost::filesystem;

// SIFT is doing so much better than SURF for haz cam images.
DEFINE_string(feature_detector, "SIFT", "The feature detector to use. SIFT or SURF.");
DEFINE_int32(sift_nFeatures, 10000, "Number of SIFT features");
DEFINE_int32(sift_nOctaveLayers, 3, "Number of SIFT octave layers");
DEFINE_double(sift_contrastThreshold, 0.02,
              "SIFT contrast threshold");  // decrease for more ip
DEFINE_double(sift_edgeThreshold, 10, "SIFT edge threshold");
DEFINE_double(sift_sigma, 1.6, "SIFT sigma");

namespace dense_map {

void detectFeatures(const cv::Mat& image, bool verbose,
                    // Outputs
                    cv::Mat* descriptors, Eigen::Matrix2Xd* keypoints) {
  bool histogram_equalization = false;

  // If using histogram equalization, need an extra image to store it
  cv::Mat* image_ptr = const_cast<cv::Mat*>(&image);
  cv::Mat hist_image;
  if (histogram_equalization) {
    cv::equalizeHist(image, hist_image);
    image_ptr = &hist_image;
  }

  std::vector<cv::KeyPoint> storage;

  if (FLAGS_feature_detector == "SIFT") {
    cv::Ptr<cv::xfeatures2d::SIFT> sift =
      cv::xfeatures2d::SIFT::create(FLAGS_sift_nFeatures, FLAGS_sift_nOctaveLayers,
                                    FLAGS_sift_contrastThreshold,
                                    FLAGS_sift_edgeThreshold, FLAGS_sift_sigma);
    sift->detect(image, storage);
    sift->compute(image, storage, *descriptors);

  } else if (FLAGS_feature_detector == "SURF") {
    interest_point::FeatureDetector detector("SURF");
    detector.Detect(*image_ptr, &storage, descriptors);

    // Undo the shift in the detector
    for (cv::KeyPoint& key : storage) {
      key.pt.x += image.cols / 2.0;
      key.pt.y += image.rows / 2.0;
    }

  } else {
    LOG(FATAL) << "Unknown feature detector: " << FLAGS_feature_detector;
  }

  if (verbose) std::cout << "Features detected " << storage.size() << std::endl;

  // Copy to data structures expected by subsequent code
  keypoints->resize(2, storage.size());
  Eigen::Vector2d output;
  for (size_t j = 0; j < storage.size(); j++) {
    keypoints->col(j) = Eigen::Vector2d(storage[j].pt.x, storage[j].pt.y);
  }
}

// This really likes haz cam first and nav cam second
void matchFeatures(std::mutex* match_mutex, int left_image_index, int right_image_index,
                   cv::Mat const& left_descriptors, cv::Mat const& right_descriptors,
                   Eigen::Matrix2Xd const& left_keypoints,
                   Eigen::Matrix2Xd const& right_keypoints, bool verbose,
                   // output
                   MATCH_PAIR* matches) {
  std::vector<cv::DMatch> cv_matches;
  interest_point::FindMatches(left_descriptors, right_descriptors, &cv_matches);

  std::vector<cv::Point2f> left_vec;
  std::vector<cv::Point2f> right_vec;
  for (size_t j = 0; j < cv_matches.size(); j++) {
    int left_ip_index = cv_matches.at(j).queryIdx;
    int right_ip_index = cv_matches.at(j).trainIdx;

    // Get the keypoints from the good matches
    left_vec.push_back(cv::Point2f(left_keypoints.col(left_ip_index)[0],
                                   left_keypoints.col(left_ip_index)[1]));
    right_vec.push_back(cv::Point2f(right_keypoints.col(right_ip_index)[0],
                                    right_keypoints.col(right_ip_index)[1]));
  }

  if (left_vec.empty()) return;

  // These may need some tweaking but works reasonably well.
  double ransacReprojThreshold = 20.0;
  cv::Mat inlier_mask;
  int maxIters = 10000;
  double confidence = 0.8;

  // affine2D works better than homography
  // cv::Mat H = cv::findHomography(left_vec, right_vec, cv::RANSAC,
  // ransacReprojThreshold, inlier_mask, maxIters, confidence);
  cv::Mat H = cv::estimateAffine2D(left_vec, right_vec, inlier_mask, cv::RANSAC,
                                   ransacReprojThreshold, maxIters, confidence);

  std::vector<InterestPoint> left_ip, right_ip;
  for (size_t j = 0; j < cv_matches.size(); j++) {
    int left_ip_index = cv_matches.at(j).queryIdx;
    int right_ip_index = cv_matches.at(j).trainIdx;

    if (inlier_mask.at<uchar>(j, 0) == 0) continue;

    cv::Mat left_desc = left_descriptors.row(left_ip_index);
    cv::Mat right_desc = right_descriptors.row(right_ip_index);

    InterestPoint left;
    left.setFromCvKeypoint(left_keypoints.col(left_ip_index), left_desc);

    InterestPoint right;
    right.setFromCvKeypoint(right_keypoints.col(right_ip_index), right_desc);

    left_ip.push_back(left);
    right_ip.push_back(right);
  }

  // Update the shared variable using a lock
  match_mutex->lock();

  // Print the verbose message inside the lock, otherwise the text
  // may get messed up.
  if (verbose)
    std::cout << "Number of matches for pair "
              << left_image_index << ' ' << right_image_index << ": "
              << left_ip.size() << std::endl;

  *matches = std::make_pair(left_ip, right_ip);
  match_mutex->unlock();
}

// Match features while assuming that the input cameras can be used to filter out
// outliers by reprojection error.
void matchFeaturesWithCams(std::mutex* match_mutex, int left_image_index, int right_image_index,
                           camera::CameraParameters const& left_params,
                           camera::CameraParameters const& right_params,
                           Eigen::Affine3d const& left_world_to_cam,
                           Eigen::Affine3d const& right_world_to_cam,
                           double reprojection_error,
                           cv::Mat const& left_descriptors, cv::Mat const& right_descriptors,
                           Eigen::Matrix2Xd const& left_keypoints,
                           Eigen::Matrix2Xd const& right_keypoints,
                           bool verbose,
                           // output
                           MATCH_PAIR* matches) {
  // Match by using descriptors first
  std::vector<cv::DMatch> cv_matches;
  interest_point::FindMatches(left_descriptors, right_descriptors, &cv_matches);

  // Do filtering
  std::vector<cv::Point2f> left_vec;
  std::vector<cv::Point2f> right_vec;
  std::vector<cv::DMatch> filtered_cv_matches;
  for (size_t j = 0; j < cv_matches.size(); j++) {
    int left_ip_index = cv_matches.at(j).queryIdx;
    int right_ip_index = cv_matches.at(j).trainIdx;

    Eigen::Vector2d dist_left_ip(left_keypoints.col(left_ip_index)[0],
                                 left_keypoints.col(left_ip_index)[1]);

    Eigen::Vector2d dist_right_ip(right_keypoints.col(right_ip_index)[0],
                                  right_keypoints.col(right_ip_index)[1]);

    Eigen::Vector2d undist_left_ip;
    Eigen::Vector2d undist_right_ip;
    left_params.Convert<camera::DISTORTED,  camera::UNDISTORTED_C>
      (dist_left_ip, &undist_left_ip);
    right_params.Convert<camera::DISTORTED, camera::UNDISTORTED_C>
      (dist_right_ip, &undist_right_ip);

    Eigen::Vector3d X =
      dense_map::TriangulatePair(left_params.GetFocalLength(), right_params.GetFocalLength(),
                                 left_world_to_cam, right_world_to_cam,
                                 undist_left_ip, undist_right_ip);

    // Project back into the cameras
    Eigen::Vector3d left_cam_X = left_world_to_cam * X;
    Eigen::Vector2d undist_left_pix
      = left_params.GetFocalVector().cwiseProduct(left_cam_X.hnormalized());
    Eigen::Vector2d dist_left_pix;
    left_params.Convert<camera::UNDISTORTED_C, camera::DISTORTED>(undist_left_pix,
                                                                  &dist_left_pix);

    Eigen::Vector3d right_cam_X = right_world_to_cam * X;
    Eigen::Vector2d undist_right_pix
      = right_params.GetFocalVector().cwiseProduct(right_cam_X.hnormalized());
    Eigen::Vector2d dist_right_pix;
    right_params.Convert<camera::UNDISTORTED_C, camera::DISTORTED>(undist_right_pix,
                                                                   &dist_right_pix);

    // Filter out points whose reprojection error is too big
    bool is_good = ((dist_left_ip - dist_left_pix).norm() <= reprojection_error &&
                    (dist_right_ip - dist_right_pix).norm() <= reprojection_error);

    // If any values above are Inf or NaN, is_good will be false as well
    if (!is_good) continue;

    // Get the keypoints from the good matches
    left_vec.push_back(cv::Point2f(left_keypoints.col(left_ip_index)[0],
                                   left_keypoints.col(left_ip_index)[1]));
    right_vec.push_back(cv::Point2f(right_keypoints.col(right_ip_index)[0],
                                    right_keypoints.col(right_ip_index)[1]));

    filtered_cv_matches.push_back(cv_matches[j]);
  }

  if (left_vec.empty()) return;

  // Filter using geometry constraints
  // These may need some tweaking but works reasonably well.
  double ransacReprojThreshold = 20.0;
  cv::Mat inlier_mask;
  int maxIters = 10000;
  double confidence = 0.8;

  // affine2D works better than homography
  // cv::Mat H = cv::findHomography(left_vec, right_vec, cv::RANSAC,
  // ransacReprojThreshold, inlier_mask, maxIters, confidence);
  cv::Mat H = cv::estimateAffine2D(left_vec, right_vec, inlier_mask, cv::RANSAC,
                                   ransacReprojThreshold, maxIters, confidence);

  std::vector<InterestPoint> left_ip, right_ip;
  for (size_t j = 0; j < filtered_cv_matches.size(); j++) {
    int left_ip_index = filtered_cv_matches.at(j).queryIdx;
    int right_ip_index = filtered_cv_matches.at(j).trainIdx;

    if (inlier_mask.at<uchar>(j, 0) == 0) continue;

    cv::Mat left_desc = left_descriptors.row(left_ip_index);
    cv::Mat right_desc = right_descriptors.row(right_ip_index);

    InterestPoint left;
    left.setFromCvKeypoint(left_keypoints.col(left_ip_index), left_desc);

    InterestPoint right;
    right.setFromCvKeypoint(right_keypoints.col(right_ip_index), right_desc);

    left_ip.push_back(left);
    right_ip.push_back(right);
  }

  // Update the shared variable using a lock
  match_mutex->lock();

  // Print the verbose message inside the lock, otherwise the text
  // may get messed up.
  if (verbose)
    std::cout << "Number of matches for pair "
              << left_image_index << ' ' << right_image_index << ": "
              << left_ip.size() << std::endl;

  *matches = std::make_pair(left_ip, right_ip);
  match_mutex->unlock();
}
  
void writeIpRecord(std::ofstream& f, InterestPoint const& p) {
  f.write(reinterpret_cast<const char*>(&(p.x)), sizeof(p.x));
  f.write(reinterpret_cast<const char*>(&(p.y)), sizeof(p.y));
  f.write(reinterpret_cast<const char*>(&(p.ix)), sizeof(p.ix));
  f.write(reinterpret_cast<const char*>(&(p.iy)), sizeof(p.iy));
  f.write(reinterpret_cast<const char*>(&(p.orientation)), sizeof(p.orientation));
  f.write(reinterpret_cast<const char*>(&(p.scale)), sizeof(p.scale));
  f.write(reinterpret_cast<const char*>(&(p.interest)), sizeof(p.interest));
  f.write(reinterpret_cast<const char*>(&(p.polarity)), sizeof(p.polarity));
  f.write(reinterpret_cast<const char*>(&(p.octave)), sizeof(p.octave));
  f.write(reinterpret_cast<const char*>(&(p.scale_lvl)), sizeof(p.scale_lvl));
  uint64_t size = p.size();
  f.write(reinterpret_cast<const char*>((&size)), sizeof(uint64));
  for (size_t i = 0; i < p.descriptor.size(); ++i)
    f.write(reinterpret_cast<const char*>(&(p.descriptor[i])), sizeof(p.descriptor[i]));
}

// Write matches to disk
void writeMatchFile(std::string match_file, std::vector<InterestPoint> const& ip1,
                    std::vector<InterestPoint> const& ip2) {
  std::ofstream f;
  f.open(match_file.c_str(), std::ios::binary | std::ios::out);
  std::vector<InterestPoint>::const_iterator iter1 = ip1.begin();
  std::vector<InterestPoint>::const_iterator iter2 = ip2.begin();
  uint64 size1 = ip1.size();
  uint64 size2 = ip2.size();
  f.write(reinterpret_cast<const char*>(&size1), sizeof(uint64));
  f.write(reinterpret_cast<const char*>(&size2), sizeof(uint64));
  for (; iter1 != ip1.end(); ++iter1) writeIpRecord(f, *iter1);
  for (; iter2 != ip2.end(); ++iter2) writeIpRecord(f, *iter2);
  f.close();
}

// TODO(oalexan1): Duplicate code
void Triangulate(bool rm_invalid_xyz, double focal_length,
                 std::vector<Eigen::Affine3d> const& cid_to_cam_t_global,
                 std::vector<Eigen::Matrix2Xd> const& cid_to_keypoint_map,
                 std::vector<std::map<int, int> > * pid_to_cid_fid,
                 std::vector<Eigen::Vector3d> * pid_to_xyz) {
  Eigen::Matrix3d k;
  k << focal_length, 0, 0,
    0, focal_length, 0,
    0, 0, 1;

  // Build p matrices for all of the cameras. openMVG::Triangulation
  // will be holding pointers to all of the cameras.
  std::vector<openMVG::Mat34> cid_to_p(cid_to_cam_t_global.size());
  for (size_t cid = 0; cid < cid_to_p.size(); cid++) {
    openMVG::P_From_KRt(k, cid_to_cam_t_global[cid].linear(),
                        cid_to_cam_t_global[cid].translation(), &cid_to_p[cid]);
  }

  pid_to_xyz->resize(pid_to_cid_fid->size());
  for (int pid = pid_to_cid_fid->size() - 1; pid >= 0; pid--) {
    openMVG::Triangulation tri;
    for (std::pair<int, int> const& cid_fid : pid_to_cid_fid->at(pid)) {
      tri.add(cid_to_p[cid_fid.first],  // they're holding a pointer to this
              cid_to_keypoint_map[cid_fid.first].col(cid_fid.second));
    }
    Eigen::Vector3d solution = tri.compute();
    if ( rm_invalid_xyz && (std::isnan(solution[0]) || tri.minDepth() < 0) ) {
      pid_to_xyz->erase(pid_to_xyz->begin() + pid);
      pid_to_cid_fid->erase(pid_to_cid_fid->begin() + pid);
    } else {
      pid_to_xyz->at(pid) = solution;
    }
  }

}
  
// Triangulate rays emanating from given undistorted and centered pixels
Eigen::Vector3d TriangulatePair(double focal_length1, double focal_length2,
                                Eigen::Affine3d const& world_to_cam1,
                                Eigen::Affine3d const& world_to_cam2,
                                Eigen::Vector2d const& pix1,
                                Eigen::Vector2d const& pix2) {
  Eigen::Matrix3d k1;
  k1 << focal_length1, 0, 0, 0, focal_length1, 0, 0, 0, 1;

  Eigen::Matrix3d k2;
  k2 << focal_length2, 0, 0, 0, focal_length2, 0, 0, 0, 1;

  openMVG::Mat34 cid_to_p1, cid_to_p2;
  openMVG::P_From_KRt(k1, world_to_cam1.linear(), world_to_cam1.translation(), &cid_to_p1);
  openMVG::P_From_KRt(k2, world_to_cam2.linear(), world_to_cam2.translation(), &cid_to_p2);

  openMVG::Triangulation tri;
  tri.add(cid_to_p1, pix1);
  tri.add(cid_to_p2, pix2);

  Eigen::Vector3d solution = tri.compute();
  return solution;
}

// Triangulate n rays emanating from given undistorted and centered pixels
Eigen::Vector3d Triangulate(std::vector<double>          const& focal_length_vec,
                            std::vector<Eigen::Affine3d> const& world_to_cam_vec,
                            std::vector<Eigen::Vector2d> const& pix_vec) {
  if (focal_length_vec.size() != world_to_cam_vec.size() ||
      focal_length_vec.size() != pix_vec.size())
    LOG(FATAL) << "All inputs to Triangulate() must have the same size.";

  if (focal_length_vec.size() <= 1)
    LOG(FATAL) << "At least two rays must be passed to Triangulate().";

  openMVG::Triangulation tri;

  for (size_t it = 0; it < focal_length_vec.size(); it++) {
    Eigen::Matrix3d k;
    k << focal_length_vec[it], 0, 0, 0, focal_length_vec[it], 0, 0, 0, 1;

    openMVG::Mat34 cid_to_p;
    openMVG::P_From_KRt(k, world_to_cam_vec[it].linear(), world_to_cam_vec[it].translation(),
                        &cid_to_p);

    tri.add(cid_to_p, pix_vec[it]);
  }

  Eigen::Vector3d solution = tri.compute();
  return solution;
}

// Form the match file name. Assume the input images are of the form
// cam_name/image.jpg Keep the name of the cameras as part of the
// match file name, to avoid the case when two different cameras have
// images with the same name.
std::string matchFileName(std::string const& match_dir,
                          std::string const& left_image, std::string const& right_image,
                          std::string const& suffix) {
  // Keep the name of the cameras as part of the match file name,
  // to avoid the case when two different cameras have
  // images with the same name.
  std::string left_cam_name
    = boost::filesystem::path(left_image).parent_path().stem().string();
  std::string right_cam_name
    = boost::filesystem::path(right_image).parent_path().stem().string();

  if (left_cam_name == "" || right_cam_name == "")
    LOG(FATAL) << "The image name must have the form cam_name/image. Got: "
               << left_image << " and " << right_image << ".\n";

  std::string left_stem = boost::filesystem::path(left_image).stem().string();
  std::string right_stem = boost::filesystem::path(right_image).stem().string();

  std::string match_file = match_dir + "/" + left_cam_name + "-" + left_stem + "__"
    + right_cam_name + "-" + right_stem + suffix + ".match";

  return match_file;
}

void detectMatchFeatures(// Inputs
                         std::vector<dense_map::cameraImage> const& cams,
                         std::vector<camera::CameraParameters> const& cam_params,
                         std::string const& out_dir, bool save_matches,
                         std::vector<Eigen::Affine3d> const& world_to_cam, int num_overlaps,
                         int initial_max_reprojection_error, int num_match_threads,
                         bool verbose,
                         // Outputs
                         std::vector<std::vector<std::pair<float, float>>>& keypoint_vec,
                         std::vector<std::map<int, int>>& pid_to_cid_fid) {
  // Wipe the outputs
  keypoint_vec.clear();
  pid_to_cid_fid.clear();

  // Detect features using multiple threads. Too many threads may result
  // in high memory usage.
  std::ostringstream oss;
  oss << num_match_threads;
  std::string num_threads = oss.str();
  google::SetCommandLineOption("num_threads", num_threads.c_str());
  if (!gflags::GetCommandLineOption("num_threads", &num_threads))
    LOG(FATAL) << "Failed to get the value of --num_threads in Astrobee software.\n";
  std::cout << "Using " << num_threads << " threads for feature detection/matching." << std::endl;

  std::cout << "Detecting features." << std::endl;

  std::vector<cv::Mat> cid_to_descriptor_map;
  std::vector<Eigen::Matrix2Xd> cid_to_keypoint_map;
  cid_to_descriptor_map.resize(cams.size());
  cid_to_keypoint_map.resize(cams.size());
  {
    // Make the thread pool go out of scope when not needed to not use up memory
    dense_map::ThreadPool thread_pool;
    for (size_t it = 0; it < cams.size(); it++) {
      thread_pool.AddTask
        (&dense_map::detectFeatures,    // multi-threaded  // NOLINT
         // dense_map::detectFeatures(  // single-threaded // NOLINT
         cams[it].image, verbose, &cid_to_descriptor_map[it], &cid_to_keypoint_map[it]);
    }
    thread_pool.Join();
  }

  MATCH_MAP matches;

  std::vector<std::pair<int, int> > image_pairs;
  for (size_t it1 = 0; it1 < cams.size(); it1++) {
    for (size_t it2 = it1 + 1; it2 < std::min(cams.size(), it1 + num_overlaps + 1); it2++) {
      image_pairs.push_back(std::make_pair(it1, it2));
    }
  }

  {
    std::cout << "Matching features." << std::endl;
    dense_map::ThreadPool thread_pool;
    std::mutex match_mutex;
    for (size_t pair_it = 0; pair_it < image_pairs.size(); pair_it++) {
      auto pair = image_pairs[pair_it];
      int left_image_it = pair.first, right_image_it = pair.second;
      thread_pool.AddTask
        (&dense_map::matchFeaturesWithCams,   // multi-threaded  // NOLINT
         // dense_map::matchFeaturesWithCams( // single-threaded // NOLINT
         &match_mutex, left_image_it, right_image_it, cam_params[cams[left_image_it].camera_type],
         cam_params[cams[right_image_it].camera_type], world_to_cam[left_image_it],
         world_to_cam[right_image_it], initial_max_reprojection_error,
         cid_to_descriptor_map[left_image_it], cid_to_descriptor_map[right_image_it],
         cid_to_keypoint_map[left_image_it], cid_to_keypoint_map[right_image_it], verbose,
         &matches[pair]);
    }
    thread_pool.Join();
  }
  cid_to_descriptor_map = std::vector<cv::Mat>();  // Wipe, takes memory

  // Give all interest points in a given image a unique id, and put
  // them in a vector with the id corresponding to the interest point
  std::vector<std::map<std::pair<float, float>, int>> keypoint_map(cams.size());
  for (auto it = matches.begin(); it != matches.end(); it++) {
    std::pair<int, int> const& index_pair = it->first;     // alias

    int left_index = index_pair.first;
    int right_index = index_pair.second;

    dense_map::MATCH_PAIR const& match_pair = it->second;  // alias
    std::vector<dense_map::InterestPoint> const& left_ip_vec = match_pair.first;
    std::vector<dense_map::InterestPoint> const& right_ip_vec = match_pair.second;
    for (size_t ip_it = 0; ip_it < left_ip_vec.size(); ip_it++) {
      auto dist_left_ip  = std::make_pair(left_ip_vec[ip_it].x,  left_ip_vec[ip_it].y);
      auto dist_right_ip = std::make_pair(right_ip_vec[ip_it].x, right_ip_vec[ip_it].y);
      // Initialize to zero for the moment
      keypoint_map[left_index][dist_left_ip] = 0;
      keypoint_map[right_index][dist_right_ip] = 0;
    }
  }
  keypoint_vec.resize(cams.size());
  for (size_t cid = 0; cid < cams.size(); cid++) {
    keypoint_vec[cid].resize(keypoint_map[cid].size());
    int fid = 0;
    for (auto ip_it = keypoint_map[cid].begin(); ip_it != keypoint_map[cid].end();
         ip_it++) {
      auto& dist_ip = ip_it->first;  // alias
      keypoint_map[cid][dist_ip] = fid;
      keypoint_vec[cid][fid] = dist_ip;
      fid++;
    }
  }

  // If feature A in image I matches feather B in image J, which
  // matches feature C in image K, then (A, B, C) belong together in
  // a track, and will have a single triangulated xyz. Build such a track.

  openMVG::matching::PairWiseMatches match_map;
  for (auto it = matches.begin(); it != matches.end(); it++) {
    std::pair<int, int> const& index_pair = it->first;     // alias

    int left_index = index_pair.first;
    int right_index = index_pair.second;

    dense_map::MATCH_PAIR const& match_pair = it->second;  // alias
    std::vector<dense_map::InterestPoint> const& left_ip_vec = match_pair.first;
    std::vector<dense_map::InterestPoint> const& right_ip_vec = match_pair.second;

    std::vector<openMVG::matching::IndMatch> mvg_matches;

    for (size_t ip_it = 0; ip_it < left_ip_vec.size(); ip_it++) {
      auto dist_left_ip  = std::make_pair(left_ip_vec[ip_it].x,  left_ip_vec[ip_it].y);
      auto dist_right_ip = std::make_pair(right_ip_vec[ip_it].x, right_ip_vec[ip_it].y);

      int left_id = keypoint_map[left_index][dist_left_ip];
      int right_id = keypoint_map[right_index][dist_right_ip];
      mvg_matches.push_back(openMVG::matching::IndMatch(left_id, right_id));
    }
    match_map[index_pair] = mvg_matches;
  }

  if (save_matches) {
    if (out_dir.empty())
      LOG(FATAL) << "Cannot save matches if no output directory was provided.\n";

    std::string match_dir = out_dir + "/matches";
    dense_map::createDir(match_dir);

    for (auto it = matches.begin(); it != matches.end(); it++) {
      std::pair<int, int> index_pair = it->first;
      dense_map::MATCH_PAIR const& match_pair = it->second;

      int left_index = index_pair.first;
      int right_index = index_pair.second;

      std::string const& left_image = cams[left_index].image_name; // alias
      std::string const& right_image = cams[right_index].image_name; // alias

      std::string suffix = "";
      std::string match_file = matchFileName(match_dir, left_image, right_image, suffix);

      std::cout << "Writing: " << left_image << " " << right_image << " "
                << match_file << std::endl;
      dense_map::writeMatchFile(match_file, match_pair.first, match_pair.second);
    }
  }

  // De-allocate data not needed anymore and take up a lot of RAM
  matches.clear(); matches = MATCH_MAP();
  keypoint_map.clear(); keypoint_map.shrink_to_fit();
  cid_to_keypoint_map.clear(); cid_to_keypoint_map.shrink_to_fit();

  {
    // Build tracks
    // De-allocate these as soon as not needed to save memory
    openMVG::tracks::TracksBuilder trackBuilder;
    trackBuilder.Build(match_map);  // Build:  Efficient fusion of correspondences
    trackBuilder.Filter();          // Filter: Remove tracks that have conflict
    // trackBuilder.ExportToStream(std::cout);
    // Export tracks as a map (each entry is a sequence of imageId and featureIndex):
    //  {TrackIndex => {(imageIndex, featureIndex), ... ,(imageIndex, featureIndex)}
    openMVG::tracks::STLMAPTracks map_tracks;
    trackBuilder.ExportToSTL(map_tracks);
    match_map = openMVG::matching::PairWiseMatches();  // wipe this, no longer needed
    trackBuilder = openMVG::tracks::TracksBuilder();   // wipe it

    if (map_tracks.empty())
      LOG(FATAL) << "No tracks left after filtering. Perhaps images are too dis-similar?\n";

    // Populate the filtered tracks
    size_t num_elems = map_tracks.size();
    pid_to_cid_fid.resize(num_elems);
    size_t curr_id = 0;
    for (auto itr = map_tracks.begin(); itr != map_tracks.end(); itr++) {
      for (auto itr2 = (itr->second).begin(); itr2 != (itr->second).end(); itr2++) {
        pid_to_cid_fid[curr_id][itr2->first] = itr2->second;
      }
      curr_id++;
    }
  }

  return;
}

void multiViewTriangulation(// Inputs
                            std::vector<camera::CameraParameters>   const& cam_params,
                            std::vector<dense_map::cameraImage>     const& cams,
                            std::vector<Eigen::Affine3d>            const& world_to_cam,
                            std::vector<std::map<int, int>>         const& pid_to_cid_fid,
                            std::vector<std::vector<std::pair<float, float>>>
                            const& keypoint_vec,
                            // Outputs
                            std::vector<std::map<int, std::map<int, int>>>& pid_cid_fid_inlier,
                            std::vector<Eigen::Vector3d>& xyz_vec) {
  
  xyz_vec.clear();
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

    bool bad_xyz = false;
    for (int c = 0; c < xyz_vec[pid].size(); c++) {
      if (std::isinf(xyz_vec[pid][c]) || std::isnan(xyz_vec[pid][c])) 
        bad_xyz = true;
    }
    if (bad_xyz) {
      // if triangulation failed, must set all features for this pid to outliers.
      for (auto cid_fid = pid_to_cid_fid[pid].begin(); cid_fid != pid_to_cid_fid[pid].end();
           cid_fid++) {
        int cid = cid_fid->first;
        int fid = cid_fid->second;
        dense_map::setMapValue(pid_cid_fid_inlier, pid, cid, fid, 0);
      }
    }
    
  } // end iterating over triangulated points
  
  return;
}
  
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

        // When num_overlaps == 0, we save only matches read from nvm rather
        // ones made wen this tool was run.
        bool is_good = (cid1 < cid2 && (num_overlaps == 0 || cid2 < cid1 + num_overlaps + 1));
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

    std::string match_dir = out_dir + "/matches";
    dense_map::createDir(match_dir);

    std::string suffix = "-inliers";
    std::string match_file = dense_map::matchFileName(match_dir,
                                                      cams[left_index].image_name,
                                                      cams[right_index].image_name,
                                                      suffix);

    std::cout << "Writing: " << cams[left_index].image_name << ' ' << cams[right_index].image_name
              << " " << match_file << std::endl;
    dense_map::writeMatchFile(match_file, match_pair.first, match_pair.second);
  }
}

// Given a set of points in 3D, heuristically estimate what it means
// for two points to be "not far" from each other. The logic is to
// find a bounding box of an inner cluster and multiply that by 0.2.
double estimateCloseDistance(std::vector<Eigen::Vector3d> const& vec) {
  Eigen::Vector3d range;
  int num_pts = vec.size();
  if (num_pts <= 0)
    LOG(FATAL) << "Empty set of points.\n";  // to avoid a segfault

  std::vector<double> vals(num_pts);
  for (int it = 0; it < range.size(); it++) {  // iterate in each coordinate
    // Sort all values in given coordinate
    for (int p = 0; p < num_pts; p++)
      vals[p] = vec[p][it];
    std::sort(vals.begin(), vals.end());

    // Find some percentiles
    int min_p = round(num_pts*0.25);
    int max_p = round(num_pts*0.75);
    if (min_p >= num_pts) min_p = num_pts - 1;
    if (max_p >= num_pts) max_p = num_pts - 1;
    double min_val = vals[min_p], max_val = vals[max_p];
    range[it] = 0.2*(max_val - min_val);
  }

  // Find the average of all ranges
  double range_val = 0.0;
  for (int it = 0; it < range.size(); it++)
    range_val += range[it];
  range_val /= range.size();

  return range_val;
}

// Given two sets of 3D points, find the rotation + translation + scale
// which best maps the first set to the second.
// Source: http://en.wikipedia.org/wiki/Kabsch_algorithm
// TODO(oalexan1): Use robust version!  
void Find3DAffineTransform(Eigen::Matrix3Xd const & in,
                           Eigen::Matrix3Xd const & out,
                           Eigen::Affine3d* result) {
  // Default output
  result->linear() = Eigen::Matrix3d::Identity(3, 3);
  result->translation() = Eigen::Vector3d::Zero();

  if (in.cols() != out.cols())
    throw "Find3DAffineTransform(): input data mis-match";

  // Local copies we can modify
  Eigen::Matrix3Xd local_in = in, local_out = out;

  // First find the scale, by finding the ratio of sums of some distances,
  // then bring the datasets to the same scale.
  double dist_in = 0, dist_out = 0;
  for (int col = 0; col < local_in.cols()-1; col++) {
    dist_in  += (local_in.col(col+1) - local_in.col(col)).norm();
    dist_out += (local_out.col(col+1) - local_out.col(col)).norm();
  }
  if (dist_in <= 0 || dist_out <= 0)
    return;
  double scale = dist_out/dist_in;
  local_out /= scale;

  // Find the centroids then shift to the origin
  Eigen::Vector3d in_ctr = Eigen::Vector3d::Zero();
  Eigen::Vector3d out_ctr = Eigen::Vector3d::Zero();
  for (int col = 0; col < local_in.cols(); col++) {
    in_ctr  += local_in.col(col);
    out_ctr += local_out.col(col);
  }
  in_ctr /= local_in.cols();
  out_ctr /= local_out.cols();
  for (int col = 0; col < local_in.cols(); col++) {
    local_in.col(col)  -= in_ctr;
    local_out.col(col) -= out_ctr;
  }

  // SVD
  Eigen::Matrix3d Cov = local_in * local_out.transpose();
  Eigen::JacobiSVD<Eigen::Matrix3d> svd(Cov, Eigen::ComputeFullU | Eigen::ComputeFullV);

  // Find the rotation
  double d = (svd.matrixV() * svd.matrixU().transpose()).determinant();
  if (d > 0)
    d = 1.0;
  else
    d = -1.0;
  Eigen::Matrix3d I = Eigen::Matrix3d::Identity(3, 3);
  I(2, 2) = d;
  Eigen::Matrix3d R = svd.matrixV() * I * svd.matrixU().transpose();

  // The final transform
  result->linear() = scale * R;
  result->translation() = scale*(out_ctr - R*in_ctr);
}
  
// Extract control points and the images they correspond 2 from
// a hugin project file
void ParseHuginControlPoints(std::string const& hugin_file,
                             std::vector<std::string> * images,
                             Eigen::MatrixXd * points) {
  
  // Initialize the outputs
  (*images).clear();
  *points = Eigen::MatrixXd(6, 1);

  std::ifstream hf(hugin_file.c_str());
  if (!hf.good())
    LOG(FATAL) << "ParseHuginControlPoints(): Could not open hugin file: " << hugin_file;

  int num_points = 0;
  std::string line;
  while (getline(hf, line)) {
    // Parse for images
    if (line.find("i ") == 0) {
      size_t it = line.find("n\"");
      if (it == std::string::npos)
        LOG(FATAL) << "ParseHuginControlPoints(): Invalid line: " << line;
      it += 2;
      std::string image;
      while (it < line.size() && line[it] != '"') {
        image += line[it];
        it++;
      }
      (*images).push_back(image);
    }

    // Parse control points
    if (line.find("c ") == 0) {
      // First wipe all letters
      std::string orig_line = line;
      char * ptr = const_cast<char*>(line.c_str());
      for (size_t i = 0; i < line.size(); i++) {
        // Wipe some extra chars
        if ( (ptr[i] >= 'a' && ptr[i] <= 'z') ||
             (ptr[i] >= 'A' && ptr[i] <= 'Z') )
          ptr[i] = ' ';
      }

      // Out of a line like:
      // c n0 N1 x367 y240 X144.183010710425 Y243.04008545843 t0
      // we store the numbers, 0, 1, 367, 240, 144.183010710425 243.04008545843
      // as a column.
      // The stand for left image index, right image index,
      // left image x, left image y, right image x, right image y.
      double a, b, c, d, e, f;
      if (sscanf(ptr, "%lf %lf %lf %lf %lf %lf", &a, &b, &c, &d, &e, &f) != 6)
        LOG(FATAL) << "ParseHuginControlPoints(): Could not scan line: " << line;

      // The left and right images must be different
      if (a == b)
        LOG(FATAL) << "The left and right images must be distinct. "
                   << "Offending line in " << hugin_file << " is:\n"
                   << orig_line << "\n";

      num_points++;
      (*points).conservativeResize(Eigen::NoChange_t(), num_points);
      (*points).col(num_points-1) << a, b, c, d, e, f;
    }
  }
}

// A little helper function
bool is_blank(std::string const& line) {
  return (line.find_first_not_of(" \t\n\v\f\r") == std::string::npos);
}

// Parse a file having on each line xyz coordinates
void ParseXYZ(std::string const& xyz_file,
                              Eigen::MatrixXd * xyz) {
  // Initialize the outputs
  *xyz = Eigen::MatrixXd(3, 1);

  std::ifstream hf(xyz_file.c_str());
  if (!hf.good())
    LOG(FATAL) << "ParseXYZ(): Could not open hugin file: " << xyz_file;

  int num_points = 0;
  std::string line;
  while (getline(hf, line)) {
    // Ignore lines starting with comments and empty lines
    if (line.find("#") == 0 || is_blank(line)) continue;

    // Apparently sometimes empty lines show up as if of length 1
    if (line.size() == 1)
      continue;

    // Replace commas with spaces
    char * ptr = const_cast<char*>(line.c_str());
    for (size_t c = 0; c < line.size(); c++)
      if (ptr[c] == ',') ptr[c] = ' ';
    double x, y, z;
    if (sscanf(line.c_str(), "%lf %lf %lf", &x, &y, &z) != 3)
      LOG(FATAL) << "ParseXYZ(): Could not scan line: '" << line << "'\n";

    num_points++;
    (*xyz).conservativeResize(Eigen::NoChange_t(), num_points);
    (*xyz).col(num_points-1) << x, y, z;
  }
}

// Apply a given transform to the given set of cameras.
// We assume that the transform is of the form
// T(x) = scale * rotation * x + translation
void TransformCameras(Eigen::Affine3d const& T, std::vector<Eigen::Affine3d> &world_to_cam) {
  
  // Inverse of rotation component
  double scale = pow(T.linear().determinant(), 1.0 / 3.0);
  Eigen::MatrixXd Tinv = (T.linear()/scale).inverse();

  for (size_t cid = 0; cid < world_to_cam.size(); cid++) {
    world_to_cam[cid].linear() = world_to_cam[cid].linear()*Tinv;
    world_to_cam[cid].translation() = scale*world_to_cam[cid].translation() -
      world_to_cam[cid].linear()*T.translation();
  }
}

// Apply same transform as above to points
void TransformPoints(Eigen::Affine3d const& T, std::vector<Eigen::Vector3d> *xyz) {
  for (size_t pid = 0; pid < (*xyz).size(); pid++)
    (*xyz)[pid] = T * (*xyz)[pid];
}

// Apply a registration transform to a rig. The only thing that
// changes is scale, as the rig transforms are between coordinate
// systems of various cameras.
void TransformRig(Eigen::Affine3d const& T, std::vector<Eigen::Affine3d> & ref_to_cam_trans) {
  double scale = pow(T.linear().determinant(), 1.0 / 3.0);
  for (size_t cam_type = 0; cam_type < ref_to_cam_trans.size(); cam_type++) 
    ref_to_cam_trans[cam_type].translation() *= scale;
}

// Two minor and local utility functions
std::string print_vec(double a) {
  char st[256];
  snprintf(st, sizeof(st), "%7.4f", a);
  return std::string(st);
}
std::string print_vec(Eigen::Vector3d a) {
  char st[256];
  snprintf(st, sizeof(st), "%7.4f %7.4f %7.4f", a[0], a[1], a[2]);
  return std::string(st);
}
  
// Find the 3D transform from an abstract coordinate system to the
// world, given control points (pixel matches) and corresponding 3D
// measurements. It is assumed all images are from the reference
// camera.
Eigen::Affine3d registrationTransform(std::string const& hugin_file, std::string const& xyz_file,
                                  camera::CameraParameters const& ref_cam_params,
                                  std::vector<std::string> const& cid_to_filename,
                                  std::vector<Eigen::Affine3d>  & world_to_cam_trans) { 
  
  // Get the interest points in the images, and their positions in
  // the world coordinate system, as supplied by a user.
  // Parse and concatenate that information from multiple files.
  std::vector<std::string> images;
  Eigen::MatrixXd user_ip;
  Eigen::MatrixXd user_xyz;
  
  ParseHuginControlPoints(hugin_file, &images, &user_ip);
  ParseXYZ(xyz_file, &user_xyz);

  int num_points = user_ip.cols();
  if (num_points != user_xyz.cols())
    LOG(FATAL) << "Could not parse an equal number of control "
               << "points and xyz coordinates. Their numbers are "
               << num_points << " vs " << user_xyz.cols() << ".\n";


  std::map<std::string, int> filename_to_cid;
  for (size_t cid = 0; cid < cid_to_filename.size(); cid++)
    filename_to_cid[cid_to_filename[cid]] = cid;

  // Wipe images that are missing from the map
  std::map<int, int> cid2cid;
  int good_cid = 0;
  for (size_t cid = 0; cid < images.size(); cid++) {
    std::string image = images[cid];
    if (filename_to_cid.find(image) == filename_to_cid.end()) {
      LOG(WARNING) << "Will ignore image missing from map: " << image;
      continue;
    }
    cid2cid[cid] = good_cid;
    images[good_cid] = images[cid];
    good_cid++;
  }
  images.resize(good_cid);

  // Remove points corresponding to images missing from map
  int good_pid = 0;
  for (int pid = 0; pid < num_points; pid++) {
    int id1 = user_ip(0, pid);
    int id2 = user_ip(1, pid);
    if (cid2cid.find(id1) == cid2cid.end() || cid2cid.find(id2) == cid2cid.end()) {
      continue;
    }
    user_ip.col(good_pid) = user_ip.col(pid);
    user_xyz.col(good_pid) = user_xyz.col(pid);
    good_pid++;
  }
  user_ip.conservativeResize(Eigen::NoChange_t(), good_pid);
  user_xyz.conservativeResize(Eigen::NoChange_t(), good_pid);
  num_points = good_pid;
  for (int pid = 0; pid < num_points; pid++) {
    int id1 = user_ip(0, pid);
    int id2 = user_ip(1, pid);
    if (cid2cid.find(id1) == cid2cid.end() || cid2cid.find(id2) == cid2cid.end())
      LOG(FATAL) << "Book-keeping failure in registration.";
    user_ip(0, pid) = cid2cid[id1];
    user_ip(1, pid) = cid2cid[id2];
  }


  if (num_points < 3) 
    LOG(FATAL) << "Must have at least 3 points to apply registration. Got: "
               << num_points << "\n";
  
  // Iterate over the control points in the hugin file. Copy the
  // control points to the list of user keypoints, and create the
  // corresponding user_pid_to_cid_fid.
  std::vector<Eigen::Matrix2Xd> user_cid_to_keypoint_map;
  std::vector<std::map<int, int> > user_pid_to_cid_fid;
  user_cid_to_keypoint_map.resize(cid_to_filename.size());
  user_pid_to_cid_fid.resize(num_points);
  for (int pid = 0; pid < num_points; pid++) {
    // Left and right image indices
    int id1 = user_ip(0, pid);
    int id2 = user_ip(1, pid);

    // Sanity check
    if (id1 < 0 || id2 < 0 ||
        id1 >= static_cast<int>(images.size()) ||
        id2 >= static_cast<int>(images.size()) )
      LOG(FATAL) << "Invalid image indices in the hugin file: " << id1 << ' ' << id2;

    // Find the corresponding indices in the map where these keypoints will go to
    if (filename_to_cid.find(images[id1]) == filename_to_cid.end())
      LOG(FATAL) << "File missing from map: " << images[id1];
    if (filename_to_cid.find(images[id2]) == filename_to_cid.end())
      LOG(FATAL) << "File missing from map: " << images[id2];
    int cid1 = filename_to_cid[images[id1]];
    int cid2 = filename_to_cid[images[id2]];

    // Append to the keypoints for cid1
    Eigen::Matrix<double, 2, -1> &M1 = user_cid_to_keypoint_map[cid1];  // alias
    Eigen::Matrix<double, 2, -1> N1(M1.rows(), M1.cols()+1);
    N1 << M1, user_ip.block(2, pid, 2, 1);  // left image pixel x and pixel y
    M1.swap(N1);

    // Append to the keypoints for cid2
    Eigen::Matrix<double, 2, -1> &M2 = user_cid_to_keypoint_map[cid2];  // alias
    Eigen::Matrix<double, 2, -1> N2(M2.rows(), M2.cols()+1);
    N2 << M2, user_ip.block(4, pid, 2, 1);  // right image pixel x and pixel y
    M2.swap(N2);

    // The corresponding user_pid_to_cid_fid
    user_pid_to_cid_fid[pid][cid1] = user_cid_to_keypoint_map[cid1].cols()-1;
    user_pid_to_cid_fid[pid][cid2] = user_cid_to_keypoint_map[cid2].cols()-1;
  }

  // Apply undistortion
  Eigen::Vector2d output;
  for (size_t cid = 0; cid < user_cid_to_keypoint_map.size(); cid++) {
    for (int i = 0; i < user_cid_to_keypoint_map[cid].cols(); i++) {
      ref_cam_params.Convert<camera::DISTORTED, camera::UNDISTORTED_C>
        (user_cid_to_keypoint_map[cid].col(i), &output);
      user_cid_to_keypoint_map[cid].col(i) = output;
    }
  }

  // Triangulate to find the coordinates of the current points
  // in the virtual coordinate system
  std::vector<Eigen::Vector3d> unreg_pid_to_xyz;
  bool rm_invalid_xyz = false;  // there should be nothing to remove hopefully
  Triangulate(rm_invalid_xyz,
              ref_cam_params.GetFocalLength(),
              world_to_cam_trans,
              user_cid_to_keypoint_map,
              &user_pid_to_cid_fid,
              &unreg_pid_to_xyz);

  double mean_err = 0;
  for (int i = 0; i < user_xyz.cols(); i++) {
    Eigen::Vector3d a = unreg_pid_to_xyz[i];
    Eigen::Vector3d b = user_xyz.col(i);
    mean_err += (a-b).norm();
  }
  mean_err /= user_xyz.cols();
  std::cout << "Mean absolute error before registration: " << mean_err << " meters" << std::endl;
  std::cout << "Un-transformed computed xyz -- measured xyz -- error diff -- error norm (meters)"
            << std::endl;

  for (int i = 0; i < user_xyz.cols(); i++) {
    Eigen::Vector3d a = unreg_pid_to_xyz[i];
    Eigen::Vector3d b = user_xyz.col(i);
    std::cout << print_vec(a) << " -- "
              << print_vec(b) << " -- "
              << print_vec(a-b) << " -- "
              << print_vec((a - b).norm())
              << std::endl;
  }


  // Find the transform from the computed map coordinate system
  // to the world coordinate system.
  int np = unreg_pid_to_xyz.size();
  Eigen::Matrix3Xd in(3, np);
  for (int i = 0; i < np; i++)
    in.col(i) = unreg_pid_to_xyz[i];

  Eigen::Affine3d registration_trans;  
  Find3DAffineTransform(in, user_xyz, &registration_trans);

  // Transform the map to the world coordinate system
  TransformCameras(registration_trans, world_to_cam_trans);
  
  mean_err = 0.0;
  for (int i = 0; i < user_xyz.cols(); i++)
    mean_err += (registration_trans*in.col(i) - user_xyz.col(i)).norm();
  mean_err /= user_xyz.cols();

  // We don't use LOG(INFO) below, as it does not play well with
  // Eigen.
  double scale = pow(registration_trans.linear().determinant(), 1.0 / 3.0);
  std::cout << "Registration transform (to measured world coordinates)." << std::endl;
  std::cout << "Rotation:\n" << registration_trans.linear() / scale << std::endl;
  std::cout << "Scale:\n" << scale << std::endl;
  std::cout << "Translation:\n" << registration_trans.translation().transpose()
            << std::endl;

  std::cout << "Mean absolute error after registration: "
            << mean_err << " meters" << std::endl;

  std::cout << "Transformed computed xyz -- measured xyz -- "
            << "error diff - error norm (meters)" << std::endl;
  for (int i = 0; i < user_xyz.cols(); i++) {
    Eigen::Vector3d a = registration_trans*in.col(i);
    Eigen::Vector3d b = user_xyz.col(i);
    int id1 = user_ip(0, i);
    int id2 = user_ip(1, i);

    std::cout << print_vec(a) << " -- "
              << print_vec(b) << " -- "
              << print_vec(a - b) << " -- "
              << print_vec((a - b).norm()) << " -- "
              << images[id1] << ' '
              << images[id2] << std::endl;
  }


  return registration_trans;
}
  
// Reads the NVM control network format.
void ReadNVM(std::string const& input_filename,
             std::vector<Eigen::Matrix2Xd> * cid_to_keypoint_map,
             std::vector<std::string> * cid_to_filename,
             std::vector<std::map<int, int> > * pid_to_cid_fid,
             std::vector<Eigen::Vector3d> * pid_to_xyz,
             std::vector<Eigen::Affine3d> *
             cid_to_cam_t_global) {
  std::ifstream f(input_filename, std::ios::in);
  std::string token;
  std::getline(f, token);
  
  // Assert that we start with our NVM token
  if (token.compare(0, 6, "NVM_V3") != 0) {
    LOG(FATAL) << "File doesn't start with NVM token";
  }

  // Read the number of cameras
  ptrdiff_t number_of_cid;
  f >> number_of_cid;
  if (number_of_cid < 1) {
    LOG(FATAL) << "NVM file is missing cameras";
  }

  // Resize all our structures to support the number of cameras we now expect
  cid_to_keypoint_map->resize(number_of_cid);
  cid_to_filename->resize(number_of_cid);
  cid_to_cam_t_global->resize(number_of_cid);
  for (ptrdiff_t cid = 0; cid < number_of_cid; cid++) {
    // Clear keypoints from map. We'll read these in shortly
    cid_to_keypoint_map->at(cid).resize(Eigen::NoChange_t(), 2);

    // Read the line that contains camera information
    double focal, dist1, dist2;
    Eigen::Quaterniond q;
    Eigen::Vector3d c;
    f >> token >> focal;
    f >> q.w() >> q.x() >> q.y() >> q.z();
    f >> c[0] >> c[1] >> c[2] >> dist1 >> dist2;
    cid_to_filename->at(cid) = token;

    // Solve for t, which is part of the affine transform
    Eigen::Matrix3d r = q.matrix();
    cid_to_cam_t_global->at(cid).linear() = r;
    cid_to_cam_t_global->at(cid).translation() = -r * c;
  }

  // Read the number of points
  ptrdiff_t number_of_pid;
  f >> number_of_pid;
  if (number_of_pid < 1) {
    LOG(FATAL) << "The NVM file has no triangulated points.";
  }

  // Read the point
  pid_to_cid_fid->resize(number_of_pid);
  pid_to_xyz->resize(number_of_pid);
  Eigen::Vector3d xyz;
  Eigen::Vector3i color;
  Eigen::Vector2d pt;
  ptrdiff_t cid, fid;
  for (ptrdiff_t pid = 0; pid < number_of_pid; pid++) {
    pid_to_cid_fid->at(pid).clear();

    ptrdiff_t number_of_measures;
    f >> xyz[0] >> xyz[1] >> xyz[2] >>
      color[0] >> color[1] >> color[2] >> number_of_measures;
    pid_to_xyz->at(pid) = xyz;
    for (ptrdiff_t m = 0; m < number_of_measures; m++) {
      f >> cid >> fid >> pt[0] >> pt[1];

      pid_to_cid_fid->at(pid)[cid] = fid;

      if (cid_to_keypoint_map->at(cid).cols() <= fid) {
        cid_to_keypoint_map->at(cid).conservativeResize(Eigen::NoChange_t(), fid + 1);
      }
      cid_to_keypoint_map->at(cid).col(fid) = pt;
    }

    if (!f.good())
      LOG(FATAL) << "Unable to correctly read PID: " << pid;
  }
}

// Write the inliers in nvm format. The keypoints are shifted relative to the optical
// center, as written by Theia.
void writeNvm(std::string                                       const& nvm_file,
              std::vector<camera::CameraParameters>             const& cam_params,
              std::vector<dense_map::cameraImage>               const& cams,
              std::vector<Eigen::Affine3d>                      const& world_to_cam,
              std::vector<std::vector<std::pair<float, float>>> const& keypoint_vec,
              std::vector<std::map<int, int>>                   const& pid_to_cid_fid,
              std::vector<std::map<int, std::map<int, int>>>    const& pid_cid_fid_inlier,
              std::vector<Eigen::Vector3d>                      const& xyz_vec) {

  // Sanity checks
  if (world_to_cam.size() != cams.size())
    LOG(FATAL) << "Expecting as many world-to-camera transforms as cameras.\n";
  if (world_to_cam.size() != keypoint_vec.size()) 
    LOG(FATAL) << "Expecting as many sets of keypoints as cameras.\n";  
  if (pid_to_cid_fid.size() != pid_cid_fid_inlier.size())
    LOG(FATAL) << "Expecting as many inlier flags as there are tracks.\n";
  if (pid_to_cid_fid.size() != xyz_vec.size()) 
    LOG(FATAL) << "Expecting as many tracks as there are triangulated points.\n";

  // Initialize the keypoints in expected format. Copy the filenames
  // and focal lengths.
  std::vector<Eigen::Matrix2Xd> cid_to_keypoint_map(keypoint_vec.size());
  std::vector<std::string> cid_to_filename(keypoint_vec.size());
  std::vector<double> focal_lengths(keypoint_vec.size());
  for (size_t cid = 0; cid < cams.size(); cid++) {
    cid_to_keypoint_map[cid] = Eigen::MatrixXd(2, keypoint_vec[cid].size());
    cid_to_filename[cid] = cams[cid].image_name;
    focal_lengths[cid] = cam_params[cams[cid].camera_type].GetFocalLength();
  }
  
  // Copy over only inliers
  std::vector<std::map<int, int>> nvm_pid_to_cid_fid;
  std::vector<Eigen::Vector3d> nvm_pid_to_xyz;

  // Keep track how many fid we end up having for each cid
  std::vector<int> fid_count(keypoint_vec.size(), 0);
  
  for (size_t pid = 0; pid < pid_to_cid_fid.size(); pid++) {

    std::map<int, int> nvm_cid_fid;
    for (auto cid_fid = pid_to_cid_fid[pid].begin();
         cid_fid != pid_to_cid_fid[pid].end(); cid_fid++) {
      int cid = cid_fid->first;
      int fid = cid_fid->second;

      // Keep inliers only
      if (!dense_map::getMapValue(pid_cid_fid_inlier, pid, cid, fid))
        continue;

      Eigen::Vector2d dist_ip(keypoint_vec[cid][fid].first, keypoint_vec[cid][fid].second);
      // Offset relative to the optical center
      dist_ip -= cam_params[cams[cid].camera_type].GetOpticalOffset();

      // Add this to the keypoint map for cid in the location at fid_count[cid]
      cid_to_keypoint_map.at(cid).col(fid_count[cid]) = dist_ip;
      nvm_cid_fid[cid] = fid_count[cid];
      fid_count[cid]++;
    }

    // Keep only tracks with at least two points
    if (nvm_cid_fid.size() > 2) {
      nvm_pid_to_cid_fid.push_back(nvm_cid_fid);
      nvm_pid_to_xyz.push_back(xyz_vec[pid]);
    }
  }    

  // Shrink to keep only the inlier keypoints we added
  for (size_t cid = 0; cid < cams.size(); cid++)
    cid_to_keypoint_map.at(cid).conservativeResize(Eigen::NoChange_t(), fid_count[cid]);

  WriteNVM(cid_to_keypoint_map, cid_to_filename, focal_lengths, nvm_pid_to_cid_fid,  
           nvm_pid_to_xyz, world_to_cam, nvm_file);
}
  
// Write an nvm file. Note that a single focal length is assumed and no distortion.
// Those are ignored, and only camera poses, matches, and keypoints are used.
void WriteNVM(std::vector<Eigen::Matrix2Xd> const& cid_to_keypoint_map,
              std::vector<std::string> const& cid_to_filename,
              std::vector<double> const& focal_lengths,
              std::vector<std::map<int, int>> const& pid_to_cid_fid,
              std::vector<Eigen::Vector3d> const& pid_to_xyz,
              std::vector<Eigen::Affine3d> const& cid_to_cam_t_global,
              std::string const& output_filename) {

  // Ensure that the output directory exists
  std::string out_dir = fs::path(output_filename).parent_path().string();
  dense_map::createDir(out_dir);

  std::cout << "Writing: " << output_filename << std::endl;
  
  std::fstream f(output_filename, std::ios::out);
  f.precision(17); // double precision
  f << "NVM_V3\n";

  CHECK(cid_to_filename.size() == cid_to_keypoint_map.size())
    << "Unequal number of filenames and keypoints";
  CHECK(pid_to_cid_fid.size() == pid_to_xyz.size())
    << "Unequal number of pid_to_cid_fid and xyz measurements";
  CHECK(cid_to_filename.size() == cid_to_cam_t_global.size())
    << "Unequal number of filename and camera transforms";

  // Write camera information
  f << cid_to_filename.size() << std::endl;
  for (size_t cid = 0; cid < cid_to_filename.size(); cid++) {

    // World-to-camera rotation quaternion
    Eigen::Quaterniond q(cid_to_cam_t_global[cid].rotation());

    // Camera center in world coordinates
    Eigen::Vector3d t(cid_to_cam_t_global[cid].translation());
    Eigen::Vector3d camera_center =
      - cid_to_cam_t_global[cid].rotation().inverse() * t;

    f << cid_to_filename[cid] << " " << focal_lengths[cid]
      << " " << q.w() << " " << q.x() << " " << q.y() << " " << q.z() << " "
      << camera_center[0] << " " << camera_center[1] << " "
      << camera_center[2] << " " << "0 0\n"; // zero distortion, not used
  }

  // Write the number of points
  f << pid_to_cid_fid.size() << std::endl;

  for (size_t pid = 0; pid < pid_to_cid_fid.size(); pid++) {
    f << pid_to_xyz[pid][0] << " " << pid_to_xyz[pid][1] << " "
      << pid_to_xyz[pid][2] << " 0 0 0 "
      << pid_to_cid_fid[pid].size();

    CHECK(pid_to_cid_fid[pid].size() > 1)
      << "PID " << pid << " has " << pid_to_cid_fid[pid].size() << " measurements";

    for (std::map<int, int>::const_iterator it = pid_to_cid_fid[pid].begin();
         it != pid_to_cid_fid[pid].end(); it++) {
      f << " " << it->first << " " << it->second << " "
        << cid_to_keypoint_map[it->first].col(it->second)[0] << " "
        << cid_to_keypoint_map[it->first].col(it->second)[1];
    }
    f << std::endl;
  }

  // Close the file
  f.flush();
  f.close();
}

// A function to copy image data from maps to vectors with the data stored
// chronologically in them, to speed up traversal.
void ImageDataToVectors
(// Inputs
 int ref_cam_type,
 std::map<int, std::map<double, dense_map::ImageMessage>> const& image_maps,
 std::map<int, std::map<double, dense_map::ImageMessage>> const& depth_maps,
 // Outputs
 std::vector<double>& ref_timestamps,
 std::vector<Eigen::Affine3d> & world_to_ref,
 std::vector<std::string> & ref_image_files,
 std::vector<std::vector<dense_map::ImageMessage>> & image_data,
 std::vector<std::vector<dense_map::ImageMessage>> & depth_data) {

  // Wipe the outputs
  ref_timestamps.clear();
  world_to_ref.clear();
  ref_image_files.clear();
  image_data.clear();
  depth_data.clear();
  
  // Find the range of sensor ids.
  int max_cam_type = 0;
  for (auto it = image_maps.begin(); it != image_maps.end(); it++)
    max_cam_type = std::max(max_cam_type, it->first);
  for (auto it = depth_maps.begin(); it != depth_maps.end(); it++)
    max_cam_type = std::max(max_cam_type, it->first);

  image_data.resize(max_cam_type + 1);
  depth_data.resize(max_cam_type + 1);
  for (size_t cam_type = 0; cam_type < image_data.size(); cam_type++) {

    auto image_map_it = image_maps.find(cam_type);
    if (image_map_it != image_maps.end()) {
      auto image_map = image_map_it->second; 

      for (auto it = image_map.begin(); it != image_map.end(); it++) {
        image_data[cam_type].push_back(it->second);
        
        // Collect the ref cam timestamps, world_to_ref, and image names,
        // in chronological order
        if (cam_type == ref_cam_type) {
          world_to_ref.push_back(it->second.world_to_cam);
          ref_timestamps.push_back(it->second.timestamp);
          ref_image_files.push_back(it->second.name);
        }
      }
    }
    
    auto depth_map_it = depth_maps.find(cam_type);
    if (depth_map_it != depth_maps.end()) {
      auto depth_map = depth_map_it->second; 
      for (auto it = depth_map.begin(); it != depth_map.end(); it++)
        depth_data[cam_type].push_back(it->second);
    }
  }
}


// Write an image with 3 floats per pixel. OpenCV's imwrite() cannot do that.
void saveXyzImage(std::string const& filename, cv::Mat const& img) {
  if (img.depth() != CV_32F)
    LOG(FATAL) << "Expecting an image with float values\n";
  if (img.channels() != 3) LOG(FATAL) << "Expecting 3 channels.\n";

  std::ofstream f;
  f.open(filename.c_str(), std::ios::binary | std::ios::out);
  if (!f.is_open()) LOG(FATAL) << "Cannot open file for writing: " << filename << "\n";

  // Assign these to explicit variables so we know their type and size in bytes
  int rows = img.rows, cols = img.cols, channels = img.channels();

  // TODO(oalexan1): Avoid C-style cast. Test if
  // reinterpret_cast<char*> does the same thing.
  f.write((char*)(&rows), sizeof(rows));         // NOLINT
  f.write((char*)(&cols), sizeof(cols));         // NOLINT
  f.write((char*)(&channels), sizeof(channels)); // NOLINT

  for (int row = 0; row < rows; row++) {
    for (int col = 0; col < cols; col++) {
      cv::Vec3f const& P = img.at<cv::Vec3f>(row, col);  // alias
      // TODO(oalexan1): See if using reinterpret_cast<char*> does the same
      // thing.
      for (int c = 0; c < channels; c++)
        f.write((char*)(&P[c]), sizeof(P[c])); // NOLINT
    }
  }

  return;
}

// Save images and depth clouds to disk
void saveImagesAndDepthClouds(std::vector<dense_map::cameraImage> const& cams) {
  for (size_t it = 0; it < cams.size(); it++) {

    std::cout << "Writing: " << cams[it].image_name << std::endl;
    cv::imwrite(cams[it].image_name, cams[it].image);

    if (cams[it].depth_cloud.cols > 0 && cams[it].depth_cloud.rows > 0) {
      std::cout << "Writing: " << cams[it].depth_name << std::endl;
      dense_map::saveXyzImage(cams[it].depth_name, cams[it].depth_cloud);
    }
  }

  return;
}

// Read an image with 3 floats per pixel. OpenCV's imread() cannot do that.
void readXyzImage(std::string const& filename, cv::Mat & img) {
  std::ifstream f;
  f.open(filename.c_str(), std::ios::binary | std::ios::in);
  if (!f.is_open()) LOG(FATAL) << "Cannot open file for reading: " << filename << "\n";

  int rows, cols, channels;
  f.read((char*)(&rows), sizeof(rows));         // NOLINT
  f.read((char*)(&cols), sizeof(cols));         // NOLINT
  f.read((char*)(&channels), sizeof(channels)); // NOLINT

  img = cv::Mat::zeros(rows, cols, CV_32FC3);

  for (int row = 0; row < rows; row++) {
    for (int col = 0; col < cols; col++) {
      cv::Vec3f P;
      // TODO(oalexan1): See if using reinterpret_cast<char*> does the same
      // thing.
      for (int c = 0; c < channels; c++)
        f.read((char*)(&P[c]), sizeof(P[c])); // NOLINT
      img.at<cv::Vec3f>(row, col) = P;
    }
  }

  return;
}
  
void readImageEntry(// Inputs
                    std::string const& image_file,
                    Eigen::Affine3d const& world_to_cam,
                    std::vector<std::string> const& cam_names,
                    // Outputs
                    std::map<int, std::map<double, dense_map::ImageMessage>> & image_maps,
                    std::map<int, std::map<double, dense_map::ImageMessage>> & depth_maps) {
  
  // The cam name is the subdir having the images
  std::string cam_name =
    fs::path(image_file).parent_path().filename().string();
    
  std::string basename = fs::path(image_file).filename().string();
  if (basename.empty() || basename[0] < '0' || basename[0] > '9')
    LOG(FATAL) << "Image name (without directory) must start with digits. Got: "
               << basename << "\n";
  double timestamp = atof(basename.c_str());

  // Infer cam type from cam name
  int cam_type = 0;
  bool success = false;
  for (size_t cam_it = 0; cam_it < cam_names.size(); cam_it++) {
    if (cam_names[cam_it] == cam_name) {
      cam_type = cam_it;
      success = true;
      break;
    }
  }
  if (!success) 
    LOG(FATAL) << "Could not determine sensor name from image path: " << image_file << "\n";
    
  // Aliases
  std::map<double, ImageMessage> & image_map = image_maps[cam_type];
  std::map<double, ImageMessage> & depth_map = depth_maps[cam_type];

  if (image_map.find(timestamp) != image_map.end())
    LOG(FATAL) << "Duplicate timestamp " << std::setprecision(17) << timestamp
               << " for sensor id " << cam_type << "\n";
  
  // Read the image as grayscale, in order for feature matching to work
  // For texturing, texrecon should use the original color images.
  std::cout << "Reading: " << image_file << std::endl;
  image_map[timestamp].image        = cv::imread(image_file, cv::IMREAD_GRAYSCALE);
  image_map[timestamp].name         = image_file;
  image_map[timestamp].timestamp    = timestamp;
  image_map[timestamp].world_to_cam = world_to_cam;

  // Sanity check
  if (depth_map.find(timestamp) != depth_map.end())
    LOG(FATAL) << "Duplicate timestamp " << std::setprecision(17) << timestamp
               << " for sensor id " << cam_type << "\n";

  // Read the depth data, if present
  std::string depth_file = fs::path(image_file).replace_extension(".pc").string();
  if (fs::exists(depth_file)) {
    std::cout << "Reading: " << depth_file << std::endl;
    dense_map::readXyzImage(depth_file, depth_map[timestamp].image);
    depth_map[timestamp].name      = depth_file;
    depth_map[timestamp].timestamp = timestamp;
  }
}

void readCameraPoses(// Inputs
                      std::string const& camera_poses_file, int ref_cam_type,
                      std::vector<std::string> const& cam_names,
                      // Outputs
                      nvmData & nvm,
                      std::vector<double>& ref_timestamps,
                      std::vector<Eigen::Affine3d>& world_to_ref,
                      std::vector<std::string> & ref_image_files,
                      std::vector<std::vector<ImageMessage>>& image_data,
                      std::vector<std::vector<ImageMessage>>& depth_data) {
  
  // Clear the outputs
  ref_timestamps.clear();
  world_to_ref.clear();
  ref_image_files.clear();
  image_data.clear();
  depth_data.clear();
  nvm = nvmData(); // will not be filled in
  
  // Open the file
  std::cout << "Reading: " << camera_poses_file << std::endl;
  std::ifstream f;
  f.open(camera_poses_file.c_str(), std::ios::binary | std::ios::in);
  if (!f.is_open()) LOG(FATAL) << "Cannot open file for reading: " << camera_poses_file << "\n";

  // Read here temporarily the images and depth maps
  std::map<int, std::map<double, ImageMessage>> image_maps;
  std::map<int, std::map<double, ImageMessage>> depth_maps;

  std::string line;
  while (getline(f, line)) {
    if (line.empty() || line[0] == '#') continue;

    std::string image_file;
    std::istringstream iss(line);
    if (!(iss >> image_file))
      LOG(FATAL) << "Cannot parse the image file in: "
                 << camera_poses_file << "\n";

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
    readImageEntry(image_file, world_to_cam, cam_names,  
                   image_maps, depth_maps); // out 
  }

  // Put in vectors
  dense_map::ImageDataToVectors(// Inputs
                                ref_cam_type, image_maps,  depth_maps,
                                // Outputs
                                ref_timestamps, world_to_ref, ref_image_files,
                                image_data, depth_data);
  
  return;
}

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
             std::vector<std::vector<ImageMessage>>& depth_data) {
  
  // cid_to_cam_t_global has world_to_cam
  dense_map::ReadNVM(nvm_file,  
                     &nvm.cid_to_keypoint_map,  
                     &nvm.cid_to_filename,  
                     &nvm.pid_to_cid_fid,  
                     &nvm.pid_to_xyz,  
                     &nvm.cid_to_cam_t_global);
  
  // Read here temporarily the images and depth maps
  std::map<int, std::map<double, dense_map::ImageMessage>> image_maps;
  std::map<int, std::map<double, dense_map::ImageMessage>> depth_maps;
  
  for (size_t it = 0; it < nvm.cid_to_filename.size(); it++) {

    // Aliases
    auto const& image_file = nvm.cid_to_filename[it];
    auto const& world_to_cam = nvm.cid_to_cam_t_global[it];

    readImageEntry(image_file, world_to_cam, cam_names,  
                   image_maps, depth_maps); // out 
  }

  // This entails some book-keeping
  // TODO(oalexan1): Just keep image_maps and depth_maps and change the book-keeping
  // to use std::map rather than std::vector iterators
  dense_map::ImageDataToVectors(// Inputs
                                ref_cam_type, image_maps, depth_maps,
                                // Outputs
                                ref_timestamps, world_to_ref, ref_image_files,
                                image_data, depth_data);
}

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
                          std::vector<std::vector<std::pair<float, float>>> & keypoint_vec) {
    
  if (!keypoint_vec.empty() && keypoint_vec.size() != cams.size()) 
    LOG(FATAL) << "There must be as many sets of keypoints as images, or none at all.\n";

  if (keypoint_vec.empty()) 
    keypoint_vec.resize(cams.size());
    
  // First find how to map each cid from nvm to cid in 'cams'.
  std::map<std::string, int> nvm_image_name_to_cid;
  for (size_t nvm_cid = 0; nvm_cid < nvm.cid_to_filename.size(); nvm_cid++)
    nvm_image_name_to_cid[nvm.cid_to_filename[nvm_cid]] = nvm_cid;
  std::map<int, int> nvm_cid_to_cams_cid;
  for (size_t cid = 0; cid < cams.size(); cid++) {
    std::string const& image_name = cams[cid].image_name;
    auto nvm_it = nvm_image_name_to_cid.find(image_name);
    if (nvm_it == nvm_image_name_to_cid.end()) 
      LOG(FATAL) << "Could not look up image: " << image_name << " in the input nvm file.\n";
    int nvm_cid = nvm_it->second;
    nvm_cid_to_cams_cid[nvm_cid] = cid;
  }
  
  // Get new pid_to_cid_fid and keypoint_vec. Note that we ignore the triangulated
  // points in nvm.pid_to_xyz. Triangulation will be redone later.
  for (size_t pid = 0; pid < nvm.pid_to_cid_fid.size(); pid++) {

    std::map<int, int> out_cid_fid;
    for (auto cid_fid = nvm.pid_to_cid_fid[pid].begin();
         cid_fid != nvm.pid_to_cid_fid[pid].end(); cid_fid++) {
      int nvm_cid = cid_fid->first;
      int nvm_fid = cid_fid->second;
      Eigen::Vector2d keypoint = nvm.cid_to_keypoint_map.at(nvm_cid).col(nvm_fid);

      auto it = nvm_cid_to_cams_cid.find(nvm_cid);
      if (it == nvm_cid_to_cams_cid.end()) 
        continue; // this image went missing during bracketing

      int cid = it->second; // cid value in 'cams'
      // Add the offset Theia removes
      keypoint += cam_params[cams[cid].camera_type].GetOpticalOffset();

      int fid = keypoint_vec[cid].size(); // this is before we add the keypoint
      out_cid_fid[cid] = fid;
      keypoint_vec[cid].push_back(std::make_pair(keypoint[0], keypoint[1])); // size is fid + 1
    }

    // Keep only the tracks with at least two matches
    if (out_cid_fid.size() > 1) 
      pid_to_cid_fid.push_back(out_cid_fid);
      
  } // end iterating over nvm pid
}
  
}  // end namespace dense_map
