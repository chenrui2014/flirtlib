/*
 * Copyright (c) 2008, Willow Garage, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of the Willow Garage, Inc. nor the names of its
 *       contributors may be used to endorse or promote products derived from
 *       this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 */

/**
 * \file 
 * 
 * Reads saved localized scans, then matches new incoming scans to them.
 *
 * \author Bhaskara Marthi
 */

#define BOOST_NO_HASH
#include <flirtlib_ros/flirtlib.h>

#include <flirtlib_ros/conversions.h>
#include <ros/ros.h>
#include <tf/transform_listener.h>
#include <boost/thread.hpp>
#include <boost/foreach.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/optional.hpp>

namespace sm=sensor_msgs;
namespace vm=visualization_msgs;
namespace gm=geometry_msgs;

using std::string;
using std::vector;

typedef boost::mutex::scoped_lock Lock;
typedef vector<InterestPoint*> InterestPointVec;
typedef std::pair<InterestPoint*, InterestPoint*> Correspondence;
typedef vector<Correspondence> Correspondences;

namespace flirtlib_ros
{


/************************************************************
 * Node class
 ***********************************************************/

class Node
{
public:

  Node ();
  void scanCB (sm::LaserScan::ConstPtr scan);

private:

  void initializeRefScans();
  gm::Pose getPose();

  // Needed during initialization
  boost::mutex mutex_;
  ros::NodeHandle nh_;

  // Parameters 

  // State

  // Flirtlib objects
  boost::shared_ptr<SimpleMinMaxPeakFinder> peak_finder_;
  boost::shared_ptr<HistogramDistance<double> > histogram_dist_;
  boost::shared_ptr<Detector> detector_;
  boost::shared_ptr<DescriptorGenerator> descriptor_;
  boost::shared_ptr<RansacFeatureSetMatcher> ransac_;
  
  // Ros objects
  tf::TransformListener tf_;
  ros::Subscriber scan_sub_;
  ros::Publisher marker_pub_;

};


/************************************************************
 * Initialization
 ***********************************************************/

template <class T>
T getPrivateParam (const string& name)
{
  ros::NodeHandle nh("~");
  T val;
  const bool found = nh.getParam(name, val);
  ROS_ASSERT_MSG (found, "Could not find parameter %s", name.c_str());
  ROS_DEBUG_STREAM_NAMED ("init", "Initialized " << name << " to " << val);
  return val;
}

template <class T>
T getPrivateParam (const string& name, const T& default_val)
{
  ros::NodeHandle nh("~");
  T val;
  nh.param(name, val, default_val);
  ROS_DEBUG_STREAM_NAMED ("init", "Initialized " << name << " to " << val <<
                          "(default was " << default_val << ")");
  return val;
}

SimpleMinMaxPeakFinder* createPeakFinder ()
{
  return new SimpleMinMaxPeakFinder(0.34, 0.001);
}

Detector* createDetector (SimpleMinMaxPeakFinder* peak_finder)
{
  const double scale = 5.0;
  const double dmst = 2.0;
  const double base_sigma = 0.2;
  const double sigma_step = 1.4;
  CurvatureDetector* det = new CurvatureDetector(peak_finder, scale, base_sigma,
                                                 sigma_step, dmst);
  det->setUseMaxRange(false);
  return det;
}

DescriptorGenerator* createDescriptor (HistogramDistance<double>* dist)
{
  const double min_rho = 0.02;
  const double max_rho = 0.5;
  const double bin_rho = 4;
  const double bin_phi = 12;
  BetaGridGenerator* gen = new BetaGridGenerator(min_rho, max_rho, bin_rho,
                                                 bin_phi);
  gen->setDistanceFunction(dist);
  return gen;
}

Node::Node () :

  peak_finder_(createPeakFinder()),
  histogram_dist_(new EuclideanDistance<double>()),
  detector_(createDetector(peak_finder_.get())),
  descriptor_(createDescriptor(histogram_dist_.get())),
  ransac_(new RansacFeatureSetMatcher(0.0599, 0.95, 0.4, 0.4,
                                           0.0384, false)),
  scan_sub_(nh_.subscribe("scan", 1, &Node::scanCB, this)),
  marker_pub_(nh_.advertise<vm::Marker>("visualization_marker", 10))
{
}


/************************************************************
 * Main
 ***********************************************************/


gm::Pose Node::getPose ()
{
  tf::StampedTransform trans;
  tf_.lookupTransform("/map", "base_laser_link", ros::Time(), trans);
  gm::Pose pose;
  tf::poseTFToMsg(trans, pose);
  return pose;
}


void Node::scanCB (sm::LaserScan::ConstPtr scan)
{
  try
  {
    // Getting pose is the part that can throw exceptions
    const gm::Pose current_pose = getPose();
    const double theta = tf::getYaw(current_pose.orientation);
    const double x=current_pose.position.x;
    const double y=current_pose.position.y;
    ROS_INFO("Matching scan at %.2f, %.2f, %.2f", x, y, theta);

    Lock l(mutex_);

    // Extract features for this scan
    InterestPointVec pts;
    boost::shared_ptr<LaserReading> reading = fromRos(*scan);
    detector_->detect(*reading, pts);
    BOOST_FOREACH (InterestPoint* p, pts) 
      p->setDescriptor(descriptor_->describe(*p, *reading));
    marker_pub_.publish(interestPointMarkers(pts, current_pose, 0));
  }

  catch (tf::TransformException& e)
  {
    ROS_INFO ("Skipping because of tf exception");
  }
}




} // namespace

int main (int argc, char** argv)
{
  ros::init(argc, argv, "flirtlib_ros_test");
  flirtlib_ros::Node node;
  ros::spin();
  return 0;
}
