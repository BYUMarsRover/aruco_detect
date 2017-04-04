/*
 * Copyright (c) 2017, Austin Hendrix
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met: 
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer. 
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution. 
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
 * The views and conclusions contained in the software and documentation are
 * those of the authors and should not be interpreted as representing official
 * policies, either expressed or implied, of the FreeBSD Project.
 *
 */

#include <assert.h>
#include <sys/time.h>
#include <unistd.h>

#include <ros/ros.h>
#include <tf/transform_datatypes.h>
#include <tf2/LinearMath/Transform.h>
#include <tf2_ros/buffer.h>
#include <tf2_ros/transform_broadcaster.h>
#include <tf2_ros/transform_listener.h>
#include <tf2_geometry_msgs/tf2_geometry_msgs.h>
#include <visualization_msgs/Marker.h>
#include <image_transport/image_transport.h>
#include <cv_bridge/cv_bridge.h>
#include <sensor_msgs/image_encodings.h>
#include <dynamic_reconfigure/server.h>

#include "fiducial_msgs/Fiducial.h"
#include "fiducial_msgs/FiducialArray.h"
#include "fiducial_msgs/FiducialTransform.h"
#include "fiducial_msgs/FiducialTransformArray.h"
#include "aruco_detect/DetectorParamsConfig.h"

#include <opencv2/highgui.hpp>
#include <opencv2/aruco.hpp>


#include <list>
#include <string>

using namespace std;
using namespace cv;

class FiducialsNode {
  private:
    ros::Publisher * vertices_pub;
    ros::Publisher * pose_pub;

    ros::Subscriber caminfo_sub;
    image_transport::ImageTransport it;
    image_transport::Subscriber img_sub;
  

    // if set, we publish the images that contain fiducials
    bool publish_images;

    double fiducial_len;
    
    bool haveCamInfo;
    cv::Mat K;
    cv::Mat dist;
    int frameNum;
    std::string frameId;
  
    image_transport::Publisher image_pub;

    cv::Ptr<aruco::DetectorParameters> detectorParams;
    cv::Ptr<aruco::Dictionary> dictionary;

    void imageCallback(const sensor_msgs::ImageConstPtr &msg);
    void camInfoCallback(const sensor_msgs::CameraInfo::ConstPtr &msg);
    void configCallback(aruco_detect::DetectorParamsConfig &config, uint32_t level);

    dynamic_reconfigure::Server<aruco_detect::DetectorParamsConfig> configServer;
    dynamic_reconfigure::Server<aruco_detect::DetectorParamsConfig>::CallbackType callbackType;

  public:
    FiducialsNode(ros::NodeHandle &nh);
};


void FiducialsNode::configCallback(aruco_detect::DetectorParamsConfig & config, uint32_t level)
{
    /* Don't load initial config, since it will overwrite the rosparam settings */
    if (level == 0xFFFFFFFF) {
        return;
    }

    detectorParams->adaptiveThreshConstant = config.adaptiveThreshConstant;
    detectorParams->adaptiveThreshWinSizeMin = config.adaptiveThreshWinSizeMin;
    detectorParams->adaptiveThreshWinSizeMax = config.adaptiveThreshWinSizeMax;
    detectorParams->adaptiveThreshWinSizeStep = config.adaptiveThreshWinSizeStep;
    detectorParams->cornerRefinementMaxIterations = config.cornerRefinementMaxIterations;
    detectorParams->cornerRefinementMinAccuracy = config.cornerRefinementMinAccuracy;
    detectorParams->cornerRefinementWinSize = config.cornerRefinementWinSize;
    detectorParams->doCornerRefinement = config.doCornerRefinement;
    detectorParams->errorCorrectionRate = config.errorCorrectionRate;
    detectorParams->minCornerDistanceRate = config.minCornerDistanceRate;
    detectorParams->markerBorderBits = config.markerBorderBits;
    detectorParams->maxErroneousBitsInBorderRate = config.maxErroneousBitsInBorderRate;
    detectorParams->minDistanceToBorder = config.minDistanceToBorder;
    detectorParams->minMarkerDistanceRate = config.minMarkerDistanceRate;
    detectorParams->minMarkerPerimeterRate = config.minMarkerPerimeterRate;
    detectorParams->maxMarkerPerimeterRate = config.maxMarkerPerimeterRate;
    detectorParams->minOtsuStdDev = config.minOtsuStdDev;
    detectorParams->perspectiveRemoveIgnoredMarginPerCell = config.perspectiveRemoveIgnoredMarginPerCell;
    detectorParams->perspectiveRemovePixelPerCell = config.perspectiveRemovePixelPerCell;
    detectorParams->polygonalApproxAccuracyRate = config.polygonalApproxAccuracyRate;
}

void FiducialsNode::camInfoCallback(const sensor_msgs::CameraInfo::ConstPtr& msg)
{
    if (haveCamInfo) {
        return;
    }

    for (int i=0; i<3; i++) {
        for (int j=0; j<3; j++) {
            K.at<double>(i, j) = msg->K[i*3+j];
        }
    }

    for (int i=0; i<5; i++) {
        dist.at<double>(0,i) = msg->D[i];
    }

    haveCamInfo = true;
    frameId = msg->header.frame_id;
}

void FiducialsNode::imageCallback(const sensor_msgs::ImageConstPtr & msg) {
    ROS_INFO("Got image");
    frameNum++;

    cv_bridge::CvImagePtr cv_ptr;

    fiducial_msgs::FiducialTransformArray fta;
    fta.header.stamp = msg->header.stamp;
    fta.header.frame_id = frameId;
    fta.image_seq = msg->header.seq;

    fiducial_msgs::FiducialArray fva;
    fva.header.stamp = msg->header.stamp;
    fva.header.frame_id =frameId;
    fva.image_seq = msg->header.seq;

    try {
        cv_ptr = cv_bridge::toCvCopy(msg, sensor_msgs::image_encodings::BGR8);
        
        vector <int>  ids;
        vector <vector <Point2f> > corners, rejected;
        vector <Vec3d>  rvecs, tvecs;

        aruco::detectMarkers(cv_ptr->image, dictionary, corners, ids, detectorParams);
        ROS_INFO("Detectd %d markers", (int)ids.size());
 
        for (int i=0; i<ids.size(); i++) {
            fiducial_msgs::Fiducial fid;
            fid.fiducial_id = ids[i];
            
            fid.x0 = corners[i][0].x;
            fid.y0 = corners[i][0].y;
            fid.x1 = corners[i][1].x;
            fid.y1 = corners[i][1].y;
            fid.x2 = corners[i][2].x;
            fid.y2 = corners[i][2].y;
            fid.x3 = corners[i][3].x;
            fid.y3 = corners[i][3].y;
            fva.fiducials.push_back(fid);
        }

        vertices_pub->publish(fva);

        if (!haveCamInfo) {
            if (frameNum > 5) {
                ROS_ERROR("No camera intrinsics");
            }
            return;
        }

        aruco::estimatePoseSingleMarkers(corners, fiducial_len, K, dist, rvecs, tvecs);
        if(ids.size() > 0) {
            aruco::drawDetectedMarkers(cv_ptr->image, corners, ids);
        }

        for (int i=0; i<ids.size(); i++) {
            aruco::drawAxis(cv_ptr->image, K, dist, rvecs[i], tvecs[i], fiducial_len);

            ROS_INFO("Detected id %d T %.2f %.2f %.2f R %.2f %.2f %.2f", ids[i],
                     tvecs[i][0], tvecs[i][1], tvecs[i][2],
                     rvecs[i][0], rvecs[i][1], rvecs[i][2]);

            double angle = norm(rvecs[i]);
            Vec3d axis = rvecs[i] / angle;
            ROS_INFO("angle %f axis %f %f %f", angle, axis[0], axis[1], axis[2]);

            fiducial_msgs::FiducialTransform ft;
            ft.fiducial_id = ids[i];

            ft.transform.translation.x = tvecs[i][0];
            ft.transform.translation.y = tvecs[i][1];
            ft.transform.translation.z = tvecs[i][2];
            
            tf2::Quaternion q;
            q.setRotation(tf2::Vector3(axis[0], axis[1], axis[2]), angle);

            ft.transform.rotation.w = q.w();
            ft.transform.rotation.x = q.x();
            ft.transform.rotation.y = q.y();
            ft.transform.rotation.z = q.z();

            fta.transforms.push_back(ft);

        }

	    image_pub.publish(cv_ptr->toImageMsg());
        pose_pub->publish(fta);
    }
     catch(cv_bridge::Exception & e) {
        ROS_ERROR("cv_bridge exception: %s", e.what());
    }
     catch(cv::Exception & e) {
        ROS_ERROR("cv exception: %s", e.what());
    }
}

FiducialsNode::FiducialsNode(ros::NodeHandle & nh) : it(nh)
{
    frameNum = 0;

    // Camera intrinsics
    K = cv::Mat::zeros(3, 3, CV_64F);

    // distortion coefficients
    dist = cv::Mat::zeros(1, 5, CV_64F);
  
    haveCamInfo = false;

    int dicno;

    detectorParams = new aruco::DetectorParameters();

    nh.param<bool>("publish_images", publish_images, false);
    nh.param<double>("fiducial_len", fiducial_len, 0.14);
    nh.param<int>("dictionary", dicno, 7);

    image_pub = it.advertise("/fiducial_images", 1);

    vertices_pub = new ros::Publisher(nh.advertise<fiducial_msgs::FiducialArray>("/fiducial_vertices", 1));

    pose_pub = new ros::Publisher(nh.advertise<fiducial_msgs::FiducialTransformArray>("/fiducial_transforms", 1)); 
    
    dictionary = aruco::getPredefinedDictionary(dicno);

    img_sub = it.subscribe("/camera", 1,
                           &FiducialsNode::imageCallback, this);

    caminfo_sub = nh.subscribe("/camera_info", 1,
			       &FiducialsNode::camInfoCallback, this);

    callbackType = boost::bind(&FiducialsNode::configCallback, this, _1, _2);
    configServer.setCallback(callbackType);

    nh.param<double>("adaptiveThreshConstant", detectorParams->adaptiveThreshConstant, 7);
    nh.param<int>("adaptiveThreshWinSizeMax", detectorParams->adaptiveThreshWinSizeMax, 53); /* defailt 23 */
    nh.param<int>("adaptiveThreshWinSizeMin", detectorParams->adaptiveThreshWinSizeMin, 3);
    nh.param<int>("adaptiveThreshWinSizeStep", detectorParams->adaptiveThreshWinSizeStep, 4); /* default 10 */
    nh.param<int>("cornerRefinementMaxIterations", detectorParams->cornerRefinementMaxIterations, 30);
    nh.param<double>("cornerRefinementMinAccuracy", detectorParams->cornerRefinementMinAccuracy, 0.01); /* default 0.1 */
    nh.param<int>("cornerRefinementWinSize", detectorParams->cornerRefinementWinSize, 5);
    nh.param<bool>("doCornerRefinement",detectorParams->doCornerRefinement, true); /* default false */
    nh.param<double>("errorCorrectionRate", detectorParams->errorCorrectionRate , 0.6);
    nh.param<double>("minCornerDistanceRate", detectorParams->minCornerDistanceRate , 0.05);
    nh.param<int>("markerBorderBits", detectorParams->markerBorderBits, 1);
    nh.param<double>("maxErroneousBitsInBorderRate", detectorParams->maxErroneousBitsInBorderRate, 0.04);
    nh.param<int>("minDistanceToBorder", detectorParams->minDistanceToBorder, 3);
    nh.param<double>("minMarkerDistanceRate", detectorParams->minMarkerDistanceRate, 0.05);
    nh.param<double>("minMarkerPerimeterRate", detectorParams->minMarkerPerimeterRate, 0.1); /* default 0.3 */
    nh.param<double>("maxMarkerPerimeterRate", detectorParams->maxMarkerPerimeterRate, 4.0);
    nh.param<double>("minOtsuStdDev", detectorParams->minOtsuStdDev, 5.0);
    nh.param<double>("perspectiveRemoveIgnoredMarginPerCell", detectorParams->perspectiveRemoveIgnoredMarginPerCell, 0.13);
    nh.param<int>("perspectiveRemovePixelPerCell", detectorParams->perspectiveRemovePixelPerCell, 8);
    nh.param<double>("polygonalApproxAccuracyRate", detectorParams->polygonalApproxAccuracyRate, 0.01); /* default 0.05 */

    ROS_INFO("Aruco detection ready");
}

int main(int argc, char ** argv) {
    ros::init(argc, argv, "aruco_detect");
    ros::NodeHandle nh("~");

    FiducialsNode * node = new FiducialsNode(nh);

    ros::spin();

    return 0;
}
