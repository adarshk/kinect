/*
 * Software License Agreement (BSD License)
 *
 *  Copyright (c) 2010
 *    Ivan Dryanovski <ivan.dryanovski@gmail.com>
 *    William Morris <morris@ee.ccny.cuny.edu>
 *    Stéphane Magnenat <stephane.magnenat@mavt.ethz.ch>
 *    Radu Bogdan Rusu <rusu@willowgarage.com>
 *
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
 *   * Neither the name of Willow Garage, Inc. nor the names of its
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
 *
 */

#include "kinect_camera/kinect.h"
#include <sensor_msgs/image_encodings.h>
#include <boost/make_shared.hpp>

namespace kinect_camera 
{

const double KinectDriver::SHIFT_SCALE = 0.125;

/** \brief Constructor */
KinectDriver::KinectDriver (ros::NodeHandle comm_nh, ros::NodeHandle param_nh)
  : reconfigure_server_(param_nh),
    width_ (640), height_ (480),
    f_ctx_(NULL), f_dev_(NULL),
    started_(false),
    depth_sent_ (true), rgb_sent_ (true), 
    have_depth_matrix_(false),
    can_switch_stream_(false)
{
  // Set up reconfigure server
  ReconfigureServer::CallbackType f = boost::bind(&KinectDriver::configCb, this, _1, _2);
  reconfigure_server_.setCallback(f);
  
  // Assemble the point cloud data
  std::string kinect_depth_frame;
  param_nh.param ("kinect_depth_frame", kinect_depth_frame, std::string ("/kinect_depth"));
  cloud_.header.frame_id = cloud2_.header.frame_id = kinect_depth_frame;
  cloud_.channels.resize (1);
  cloud_.channels[0].name = "rgb";
  cloud_.channels[0].values.resize (width_ * height_);

  cloud2_.height = height_;
  cloud2_.width = width_;
  cloud2_.fields.resize (4);
  cloud2_.fields[0].name = "x";
  cloud2_.fields[1].name = "y";
  cloud2_.fields[2].name = "z";
  cloud2_.fields[3].name = "rgb";

  // Set all the fields types accordingly
  int offset = 0;
  for (size_t s = 0; s < cloud2_.fields.size (); ++s, offset += 4)
  {
    cloud2_.fields[s].offset   = offset;
    cloud2_.fields[s].count    = 1;
    cloud2_.fields[s].datatype = sensor_msgs::PointField::FLOAT32;
  }

  cloud2_.point_step = offset;
  cloud2_.row_step   = cloud2_.point_step * cloud2_.width;
  cloud2_.data.resize (cloud2_.row_step   * cloud2_.height);
  cloud2_.is_dense = true;

  // Assemble the depth image data
  depth_image_.header.frame_id = kinect_depth_frame;
  depth_image_.height = height_;
  depth_image_.width = width_;
  depth_image_.encoding = "mono8";
  depth_image_.step = width_;
  depth_image_.data.resize(width_ * height_);

  // Assemble the image data
  std::string kinect_RGB_frame;
  param_nh.param ("kinect_rgb_frame", kinect_RGB_frame, std::string ("/kinect_rgb"));
  rgb_image_.header.frame_id = kinect_RGB_frame;
  rgb_image_.height = height_;
  rgb_image_.width = width_;
  rgb_info_.header.frame_id = rgb_image_.header.frame_id; 

  // Read calibration parameters from disk
  std::string cam_name, rgb_info_url, depth_info_url;
  param_nh.param ("camera_name", cam_name, std::string("camera"));
  param_nh.param ("rgb/camera_info_url", rgb_info_url, std::string("auto"));
  param_nh.param ("depth/camera_info_url", depth_info_url, std::string("auto"));
  if (rgb_info_url.compare("auto") == 0) 
    rgb_info_url = std::string("file://")+ros::package::getPath(ROS_PACKAGE_NAME)+std::string("/info/calibration_rgb.yaml");
  if (depth_info_url.compare("auto") == 0)
    depth_info_url = std::string("file://")+ros::package::getPath(ROS_PACKAGE_NAME)+std::string("/info/calibration_depth.yaml");
  ROS_INFO ("[KinectDriver] Calibration URLs:\n\tRGB: %s\n\tDepth: %s",
            rgb_info_url.c_str (), depth_info_url.c_str ());

  rgb_info_manager_   = boost::make_shared<CameraInfoManager> (ros::NodeHandle(comm_nh, "rgb"),
                                                               cam_name, rgb_info_url);
  depth_info_manager_ = boost::make_shared<CameraInfoManager> (ros::NodeHandle(comm_nh, "depth"),
                                                               cam_name, depth_info_url);
  rgb_info_   = rgb_info_manager_->getCameraInfo ();
  depth_info_ = depth_info_manager_->getCameraInfo ();
  rgb_info_.header.frame_id = kinect_RGB_frame; 
  depth_info_.header.frame_id = kinect_depth_frame;
  rgb_model_.fromCameraInfo(rgb_info_);
  depth_model_.fromCameraInfo(depth_info_);

  /// @todo Actually read all these parameters
  shift_offset_ = 1090.845963;
  baseline_ = 0.073012;

  // Compute transform matrix from (u,v,d) of depth camera to (u,v) of RGB camera
  Eigen::Matrix4d Q, S;
  Eigen::Matrix<double, 3, 4> P;
  // From (u,v,d,1) in depth image to (X,Y,Z,W) in depth camera frame
  Q << 1, 0, 0, -depth_model_.cx(),
       0, 1, 0, -depth_model_.cy(),
       0, 0, 0,  depth_model_.fx(),
       0, 0, 1.0 / baseline_, 0;

  // From (X,Y,Z,W) in depth camera frame to RGB camera frame
  S << 0.999968,  0.006511, -0.004615, -0.025201,
      -0.006504,  0.999978,  0.001417,  0.000112,
       0.004625, -0.001387,  0.999988,  0.001367,
       0,         0,         0,         1;

  // From (X,Y,Z,W) in RGB camera frame to (u,v,w) in RGB image
  P << rgb_model_.fx(), 0,               rgb_model_.cx(), 0,
       0,               rgb_model_.fy(), rgb_model_.cy(), 0,
       0,               0,               1,               0;

  // Putting it all together, from (u,v,d,1) in depth image to (u,v,w) in RGB image
  depth_to_rgb_ = P*S*Q;

  std::cout << "Transform matrix:" << std::endl << depth_to_rgb_ << std::endl << std::endl;

  // Publishers and subscribers
  image_transport::ImageTransport it(comm_nh);
  pub_rgb_     = it.advertiseCamera ("rgb/image_raw", 1);
  pub_rgb_rect_ = it.advertise("rgb/image_rect_color", 1);
  pub_depth_   = it.advertiseCamera ("depth/image_raw", 1);
  pub_ir_      = it.advertiseCamera ("ir/image_raw", 1);
  pub_points_  = comm_nh.advertise<sensor_msgs::PointCloud>("points", 15);
  pub_points2_ = comm_nh.advertise<sensor_msgs::PointCloud2>("points2", 15);
  pub_imu_ = comm_nh.advertise<sensor_msgs::Imu>("imu", 15);

  // Timer for switching between IR and color image streams when in calibration mode.
  // libfreenect freezes if we try to do this in the image callbacks.
  // Too short a period and the switching doesn't work (why?), 0.3s seems to be OK.
  format_switch_timer_ = comm_nh.createTimer(ros::Duration(0.3), &KinectDriver::formatSwitchCb, this);
  format_switch_timer_.stop();
}

/** \brief Initialize a Kinect device, given an index.
  * \param index the index of the device to initialize
  */
bool
  KinectDriver::init (int index)
{
  // Initialize the USB 
  if (freenect_init (&f_ctx_, NULL) < 0)
  {
    ROS_ERROR ("[KinectDriver::init] Initialization failed!");
    return (false);
  }

  // Get the number of devices available
  int nr_devices = freenect_num_devices (f_ctx_);
  if (nr_devices < 1)
  {
    ROS_WARN ("[KinectDriver::init] No devices found!");
    return (false);
  }
  ROS_DEBUG ("[KinectDriver::init] Number of devices found: %d", nr_devices);
  if (nr_devices <= index)
  {
    ROS_WARN ("[KinectDriver::init] Desired device index (%d) out of bounds (%d)!", index, nr_devices);
    return (false);
  }

  // Open the device
  if (freenect_open_device (f_ctx_, &f_dev_, index) < 0)
  {
    ROS_ERROR ("[KinectDriver::init] Could not open device with index (%d)!", index);
    return (false);
  }

  // Set the appropriate data callbacks
  freenect_set_user(f_dev_, this);
  freenect_set_depth_callback (f_dev_, &KinectDriver::depthCbInternal);
  freenect_set_rgb_callback   (f_dev_, &KinectDriver::rgbCbInternal);
  freenect_set_ir_callback    (f_dev_, &KinectDriver::irCbInternal);
  
  updateDeviceSettings();

  return (true);
}

void KinectDriver::depthCbInternal (freenect_device *dev, void *buf, uint32_t timestamp)
{
  freenect_depth* tbuf = reinterpret_cast<freenect_depth*>(buf);
  KinectDriver* driver = reinterpret_cast<KinectDriver*>(freenect_get_user(dev));
  driver->depthCb(dev, tbuf, timestamp);
}

void KinectDriver::rgbCbInternal (freenect_device *dev, freenect_pixel *buf, uint32_t timestamp)
{
  KinectDriver* driver = reinterpret_cast<KinectDriver*>(freenect_get_user(dev));
  driver->rgbCb(dev, buf, timestamp);
}

void KinectDriver::irCbInternal (freenect_device *dev, freenect_pixel_ir *buf, uint32_t timestamp)
{
  KinectDriver* driver = reinterpret_cast<KinectDriver*>(freenect_get_user(dev));
  driver->irCb(dev, buf, timestamp);
}

/** \brief Destructor */
KinectDriver::~KinectDriver ()
{
  freenect_close_device (f_dev_);
  freenect_shutdown (f_ctx_);
  
  delete [] depth_proj_matrix_;
}

/** \brief Start (resume) the data acquisition process. */
void 
  KinectDriver::start ()
{
  freenect_start_depth (f_dev_);
  if (config_.color_format == FREENECT_FORMAT_IR)
    freenect_start_ir (f_dev_);
  else
    freenect_start_rgb (f_dev_);
  
  if (config_.calibration_mode)
    format_switch_timer_.start();
  
  started_ = true;
}

/** \brief Stop (pause) the data acquisition process. */
void 
  KinectDriver::stop ()
{
  freenect_stop_depth (f_dev_);
  if (config_.color_format == FREENECT_FORMAT_IR)
    freenect_stop_ir (f_dev_);
  else
    freenect_stop_rgb (f_dev_);

  format_switch_timer_.stop();
  
  started_ = false;
}

/** \brief Depth callback. Virtual. 
  * \param dev the Freenect device
  * \param buf the resultant output buffer
  * \param timestamp the time when the data was acquired
  */
void 
  KinectDriver::depthCb (freenect_device *dev, freenect_depth *buf, uint32_t timestamp)
{
  boost::mutex::scoped_lock lock (buffer_mutex_);

  // if first time in this function, build the depth projection matrix
  /// @todo Don't need depth projection matrix anymore
  if(!have_depth_matrix_)
  {
    createDepthProjectionMatrix();
    have_depth_matrix_ = true;
  }

  depth_buf_ = buf;
  depth_sent_ = false;

  // Publish only if we have an rgb image too
  if (!rgb_sent_) 
    processRgbAndDepth ();
}

/** \brief RGB callback. Virtual.
  * \param dev the Freenect device
  * \param buf the resultant output buffer
  * \param timestamp the time when the data was acquired
  */
void 
  KinectDriver::rgbCb (freenect_device *dev, freenect_pixel *rgb, uint32_t timestamp)
{
  boost::mutex::scoped_lock lock (buffer_mutex_);

  rgb_buf_ = rgb;
  rgb_sent_ = false;

  // Publish only if we have a depth image too
  if (!depth_sent_) {
    processRgbAndDepth ();
  }
}

void
  KinectDriver::irCb (freenect_device *dev, freenect_pixel_ir *rgb, uint32_t timestamp)
{
  boost::mutex::scoped_lock lock (buffer_mutex_);

  // Reusing rgb_image_ for now
  rgb_sent_ = false;

  if (pub_ir_.getNumSubscribers () > 0)
  {
    /// @todo Publish original 16-bit output? Shifting down to 8-bit for convenience for now
    for (int i = 0; i < FREENECT_FRAME_PIX; ++i) {
      int val = rgb[i];
      rgb_image_.data[i] = (val >> 2) & 0xff;
    }
  }

  if (!depth_sent_) {
    publish ();
  }
}

void KinectDriver::processRgbAndDepth()
{
  // Fill raw RGB image message
  if (pub_rgb_.getNumSubscribers () > 0)
  {
    // Copy the image data
    memcpy (&rgb_image_.data[0], &rgb_buf_[0], rgb_image_.data.size());

    // Check the camera info
    if (rgb_info_.height != (size_t)rgb_image_.height || rgb_info_.width != (size_t)rgb_image_.width)
      ROS_DEBUG_THROTTLE (60, "[KinectDriver::rgbCb] Uncalibrated Camera");
  }
  cv::Mat rgb_raw(height_, width_, CV_8UC3, rgb_buf_);

  // Convert the data to ROS format
  /// @todo Make this use new projection stuff
  if (pub_points_.getNumSubscribers () > 0)
  {
    cloud_.points.resize (width_ * height_);
    int nrp = 0;
    // Assemble an ancient sensor_msgs/PointCloud message
    for (int u = 0; u < width_; ++u) 
    {
      for (int v = 0; v < height_; ++v)
      {
        if (!getPoint3D (depth_buf_, u, v, cloud_.points[nrp].x, cloud_.points[nrp].y, cloud_.points[nrp].z))
          continue;

        nrp++;
      }
    }
    // Resize to the correct number of points
    cloud_.points.resize (nrp);
  }
  
  if (pub_points2_.getNumSubscribers () > 0)
  {
    cv::Mat rgb_rect;
    rgb_model_.rectifyImage(rgb_raw, rgb_rect);
    double fT = depth_model_.fx() * baseline_;
    
    float bad_point = std::numeric_limits<float>::quiet_NaN ();
    // Assemble an awesome sensor_msgs/PointCloud2 message
    int k = 0;
    for (int v = 0; v < height_; ++v)
    {
      for (int u = 0; u < width_; ++u, ++k) 
      {
        float* pt_data = reinterpret_cast<float*>(&cloud2_.data[0] + k * cloud2_.point_step);
        double d = SHIFT_SCALE * (shift_offset_ - depth_buf_[k]); // disparity
        if (d <= 0.0) {
          // not valid
          pt_data[0] = bad_point;
          pt_data[1] = bad_point;
          pt_data[2] = bad_point;
          // Fill in RGB
          pt_data[3] = 0;
          continue;
        }

        // Fill in XYZ
        double Z = fT / d;
        double X = ((u - depth_model_.cx()) / depth_model_.fx()) * Z;
        double Y = ((v - depth_model_.cy()) / depth_model_.fy()) * Z;
        pt_data[0] = X;
        pt_data[1] = Y;
        pt_data[2] = Z;

        // Fill in RGB from corresponding pixel in rectified RGB image
        Eigen::Vector4d uvd1;
        uvd1 << u, v, d, 1;
        Eigen::Vector3d uvw;
        uvw = depth_to_rgb_ * uvd1;
        int u_rgb = uvw[0]/uvw[2] + 0.5;
        int v_rgb = uvw[1]/uvw[2] + 0.5;
        cv::Vec3b rgb = rgb_rect.at<cv::Vec3b>(v_rgb, u_rgb);
        int32_t rgb_packed = (rgb[0] << 16) | (rgb[1] << 8) | rgb[2];
        memcpy(&pt_data[3], &rgb_packed, sizeof(int32_t));
      }
    }
  }
  if (pub_depth_.getNumSubscribers () > 0)
  { 
    // Fill in the depth image data
    depthBufferTo8BitImage(depth_buf_);
  }

  publish();
}

void KinectDriver::formatSwitchCb(const ros::TimerEvent& e)
{
  if (!can_switch_stream_)
    return;
  
  if (config_.color_format == FREENECT_FORMAT_IR) {
    config_.color_format = FREENECT_FORMAT_RGB;
    configCb(config_, 0);
  }
  else {
    config_.color_format = FREENECT_FORMAT_IR;
    configCb(config_, 0);
  }
  can_switch_stream_ = false;
}

void 
  KinectDriver::publish ()
{
  /// @todo Investigate how well synchronized the depth & color images are
  
  /// @todo Do something with the real timestamps from the device
  // Get the current time
  ros::Time time = ros::Time::now ();
  cloud_.header.stamp = cloud2_.header.stamp = time;
  rgb_image_.header.stamp   = rgb_info_.header.stamp   = time;
  depth_image_.header.stamp = depth_info_.header.stamp = time;

  // Publish RGB or IR Image
  if (config_.color_format == FREENECT_FORMAT_IR)
  {
    if (pub_ir_.getNumSubscribers() > 0)
      pub_ir_.publish (boost::make_shared<const sensor_msgs::Image> (rgb_image_), boost::make_shared<const sensor_msgs::CameraInfo> (depth_info_));
  }
  else if (pub_rgb_.getNumSubscribers () > 0)
    pub_rgb_.publish (boost::make_shared<const sensor_msgs::Image> (rgb_image_), boost::make_shared<const sensor_msgs::CameraInfo> (rgb_info_)); 

  // Publish depth Image
  if (pub_depth_.getNumSubscribers () > 0)
    pub_depth_.publish (boost::make_shared<const sensor_msgs::Image> (depth_image_), boost::make_shared<const sensor_msgs::CameraInfo> (depth_info_));

  // Publish the PointCloud messages
  if (pub_points_.getNumSubscribers () > 0)
    pub_points_.publish  (boost::make_shared<const sensor_msgs::PointCloud> (cloud_));
  if (pub_points2_.getNumSubscribers () > 0)
    pub_points2_.publish (boost::make_shared<const sensor_msgs::PointCloud2> (cloud2_));

  rgb_sent_   = true;
  depth_sent_ = true;
  can_switch_stream_ = true;
}

void KinectDriver::publishImu()
{
  imu_msg_.header.stamp = ros::Time::now();
  imu_msg_.linear_acceleration.x = accel_x_;
  imu_msg_.linear_acceleration.y = accel_y_;
  imu_msg_.linear_acceleration.z = accel_z_;
  imu_msg_.linear_acceleration_covariance[0] = imu_msg_.linear_acceleration_covariance[4]
      = imu_msg_.linear_acceleration_covariance[8] = 0.01; // @todo - what should these be?
  imu_msg_.angular_velocity_covariance[0] = -1; // indicates angular velocity not provided
  imu_msg_.orientation_covariance[0] = -1; // indicates orientation not provided
  if (pub_imu_.getNumSubscribers() > 0)
    pub_imu_.publish(imu_msg_);
}

void KinectDriver::configCb (Config &config, uint32_t level)
{
  /// @todo Integrate init() in here, so can change device and not worry about first config call
  
  // Configure color output to be RGB or Bayer
  /// @todo Mucking with image_ here might not be thread-safe
  if (config.color_format == FREENECT_FORMAT_RGB) {
    rgb_image_.encoding = sensor_msgs::image_encodings::RGB8;
    rgb_image_.data.resize(FREENECT_RGB_SIZE);
    rgb_image_.step = FREENECT_FRAME_W * 3;
  }
  else if (config.color_format == FREENECT_FORMAT_BAYER) {
    rgb_image_.encoding = sensor_msgs::image_encodings::BAYER_GRBG8;
    rgb_image_.data.resize(FREENECT_BAYER_SIZE);
    rgb_image_.step = FREENECT_FRAME_W;
  }
  else if (config.color_format == FREENECT_FORMAT_IR) {
    rgb_image_.encoding = sensor_msgs::image_encodings::MONO8;
    rgb_image_.data.resize(FREENECT_BAYER_SIZE);
    rgb_image_.step = FREENECT_FRAME_W;
  }
  else {
    ROS_ERROR("Unknown color format code %d", config.color_format);
  }

  if (config.calibration_mode && started_)
    format_switch_timer_.start();
  else
    format_switch_timer_.stop();
  
  config_ = config;
  updateDeviceSettings();
}

void KinectDriver::updateDeviceSettings()
{
  if (f_dev_) {
    freenect_set_rgb_format(f_dev_, (freenect_rgb_format)config_.color_format);
    freenect_set_led(f_dev_, (freenect_led_options)config_.led);
    freenect_set_tilt_degs(f_dev_, config_.tilt);

    if (started_) {
      if (config_.color_format == FREENECT_FORMAT_IR) {
        freenect_stop_rgb(f_dev_); // just to get rid of log spew
        freenect_start_ir(f_dev_);
      }
      else {
        freenect_stop_ir(f_dev_); // ditto
        freenect_start_rgb(f_dev_);
      }
    }
  }
}

void KinectDriver::createDepthProjectionMatrix()
{
  depth_proj_matrix_ = new cv::Point3d[height_ * width_];

  for (int y=0; y<height_; ++y)
	for (int x=0; x<width_ ; ++x) 
  {
    cv::Point2d rawPoint(x, y);
    cv::Point2d rectPoint;
    cv::Point3d rectRay;

    depth_model_.rectifyPoint(rawPoint, rectPoint);
    depth_model_.projectPixelTo3dRay(rectPoint, rectRay);

    // the depth reading is proportional to the z value, not the distance
    // so we need to renormalize the vector to z = 1.0;
    rectRay *= 1.0/rectRay.z;

    depth_proj_matrix_[y*width_ + x] = rectRay;
  }
}

inline double KinectDriver::getDistanceFromReading(freenect_depth reading) const
{
  // The formula is Z = T*R/(T - dX), where:
  //   - Z is the distance from the camera
  //   - T is the baseline between the depth camera and IR projector
  //   - R is the distance to the plane of the reference image captured during factory calibration
  //   - dX is the calculated pattern shift in the reference plane
  // If we assume the reading r is linearly related to dX (e.g. is the disparity) by some scaling
  // and offset, we need to fit parameters A and B such that Z = A / (B - r).
  // See data points at: http://www.ros.org/wiki/kinect_node
  /// @todo Make A and B calibration parameters of some sort.
  static const double A = 325.616;
  static const double B = 1084.61;
  return A / (B - reading);
}

inline bool KinectDriver::getPoint3D (freenect_depth *buf, int u, int v, float &x, float &y, float &z) const
{
  int reading = buf[v * width_ + u];

  if (reading  >= 2048 || reading <= 0) 
    return (false);

  double range = getDistanceFromReading(reading);

  if (range > config_.max_range || range <= 0)
    return (false);  

  cv::Point3d rectRay = depth_proj_matrix_[v*width_ + u];
  rectRay *= range;
  x = rectRay.x;
  y = rectRay.y;
  z = rectRay.z;

  return (true);
}

void KinectDriver::depthBufferTo8BitImage(const freenect_depth * buf)
{
  for (int y=0; y<height_; ++y)
  for (int x=0; x<width_;  ++x) 
  {
    int index(y*width_ + x);
    int reading = buf[index];
    double range = getDistanceFromReading(reading);

    uint8_t color;

    if (range > config_.max_range || range < 0) 
      color = 255;
    else
      color = 255 * range / config_.max_range;

    depth_image_.data[index] = color;
  }
}

} // namespace kinect_camera
