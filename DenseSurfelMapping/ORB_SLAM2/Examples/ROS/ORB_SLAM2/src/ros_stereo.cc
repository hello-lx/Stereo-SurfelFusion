/**
* This file is part of ORB-SLAM2.
*
* Copyright (C) 2014-2016 Raúl Mur-Artal <raulmur at unizar dot es> (University of Zaragoza)
* For more information see <https://github.com/raulmur/ORB_SLAM2>
*
* ORB-SLAM2 is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* ORB-SLAM2 is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with ORB-SLAM2. If not, see <http://www.gnu.org/licenses/>.
*/


#include<iostream>
#include<algorithm>
#include<fstream>
#include<chrono>
#include<Eigen/Dense>
#include "../../../include/Converter.h"
#include<ros/ros.h>
#include <cv_bridge/cv_bridge.h>
#include <message_filters/subscriber.h>
#include <message_filters/time_synchronizer.h>
#include <message_filters/sync_policies/approximate_time.h>
#include <nav_msgs/Path.h>
#include <nav_msgs/Odometry.h>
#include <geometry_msgs/PoseStamped.h>
#include <sensor_msgs/PointCloud.h>
#include <sensor_msgs/ChannelFloat32.h>
#include<opencv2/core/core.hpp>

#include"../../../include/System.h"

using namespace std;
ros::Publisher path_publish;
ros::Publisher pose_publish;
ros::Publisher loop_publish;
bool LastKeyframeDecision;

class ImageGrabber
{
public:
    ImageGrabber(ORB_SLAM2::System* pSLAM):mpSLAM(pSLAM){}

    void GrabStereo(const sensor_msgs::ImageConstPtr& msgLeft,const sensor_msgs::ImageConstPtr& msgRight);

    ORB_SLAM2::System* mpSLAM;
    bool do_rectify;
    cv::Mat M1l,M2l,M1r,M2r;

    bool first_image;
    int receive_counter;
    std::vector<ros::Time> receive_time_stamp;
    std::vector<std::pair<cv::Mat, cv::Mat>> loop_info;
};

int main(int argc, char **argv)
{
    ros::init(argc, argv, "RGBD");
    ros::start();

    if(argc != 4)
    {
        cerr << endl << "Usage: rosrun ORB_SLAM2 Stereo path_to_vocabulary path_to_settings do_rectify" << endl;
        ros::shutdown();
        return 1;
    }    

//argv[1] /home/yuan/XSpace/Ending/DenseSurfelMapping/ORB_SLAM2/Vocabulary/ORBvoc.txt 
//argv[2] /home/yuan/XSpace/Ending/DenseSurfelMapping/ORB_SLAM2/Examples/Stereo/KITTI00-02.yaml
//argv[3] false
//argv[4] /camera/left/image_raw:=/left_image   
//argv[5] /camera/right/image_raw:=/right_image
    // Create SLAM system. It initializes all system threads and gets ready to process frames.
    ORB_SLAM2::System SLAM(argv[1],argv[2],ORB_SLAM2::System::STEREO,true);

    ImageGrabber igb(&SLAM);
    igb.receive_counter = 0;
    igb.first_image = false;

    stringstream ss(argv[3]);
    ss >> boolalpha >> igb.do_rectify;

    if(igb.do_rectify)
    {      
        // Load settings related to stereo calibration
        cv::FileStorage fsSettings(argv[2], cv::FileStorage::READ);
        if(!fsSettings.isOpened())
        {
            cerr << "ERROR: Wrong path to settings" << endl;
            return -1;
        }

        cv::Mat K_l, K_r, P_l, P_r, R_l, R_r, D_l, D_r;
        fsSettings["LEFT.K"] >> K_l;
        fsSettings["RIGHT.K"] >> K_r;

        fsSettings["LEFT.P"] >> P_l;
        fsSettings["RIGHT.P"] >> P_r;

        fsSettings["LEFT.R"] >> R_l;
        fsSettings["RIGHT.R"] >> R_r;

        fsSettings["LEFT.D"] >> D_l;
        fsSettings["RIGHT.D"] >> D_r;

        int rows_l = fsSettings["LEFT.height"];
        int cols_l = fsSettings["LEFT.width"];
        int rows_r = fsSettings["RIGHT.height"];
        int cols_r = fsSettings["RIGHT.width"];

        if(K_l.empty() || K_r.empty() || P_l.empty() || P_r.empty() || R_l.empty() || R_r.empty() || D_l.empty() || D_r.empty() ||
                rows_l==0 || rows_r==0 || cols_l==0 || cols_r==0)
        {
            cerr << "ERROR: Calibration parameters to rectify stereo are missing!" << endl;
            return -1;
        }

        cv::initUndistortRectifyMap(K_l,D_l,R_l,P_l.rowRange(0,3).colRange(0,3),cv::Size(cols_l,rows_l),CV_32F,igb.M1l,igb.M2l);
        cv::initUndistortRectifyMap(K_r,D_r,R_r,P_r.rowRange(0,3).colRange(0,3),cv::Size(cols_r,rows_r),CV_32F,igb.M1r,igb.M2r);
    }

    ros::NodeHandle nh;

    message_filters::Subscriber<sensor_msgs::Image> left_sub(nh, "/camera/left/image_raw", 1000);
    message_filters::Subscriber<sensor_msgs::Image> right_sub(nh, "camera/right/image_raw", 1000);
    typedef message_filters::sync_policies::ApproximateTime<sensor_msgs::Image, sensor_msgs::Image> sync_pol;
    message_filters::Synchronizer<sync_pol> sync(sync_pol(10), left_sub,right_sub);
    sync.registerCallback(boost::bind(&ImageGrabber::GrabStereo,&igb,_1,_2));

    path_publish = nh.advertise<nav_msgs::Path>("orb_slam/path", 1000);
    pose_publish = nh.advertise<nav_msgs::Odometry>("orb_slam/pose", 1000);
    loop_publish = nh.advertise<sensor_msgs::PointCloud>("orb_slam/loop", 1000);

    ros::spin();

    // Stop all threads
    SLAM.Shutdown();

    // Save camera trajectory
    SLAM.SaveKeyFrameTrajectoryTUM("KeyFrameTrajectory_TUM_Format.txt");
    SLAM.SaveTrajectoryTUM("FrameTrajectory_TUM_Format.txt");
    SLAM.SaveTrajectoryKITTI("FrameTrajectory_KITTI_Format.txt");

    ros::shutdown();

    return 0;
}

void ImageGrabber::GrabStereo(const sensor_msgs::ImageConstPtr& msgLeft,const sensor_msgs::ImageConstPtr& msgRight)
{
    if(!first_image)
    {
        first_image = true;
        return;
    }

    receive_counter++;
    receive_time_stamp.push_back(msgLeft->header.stamp);

    // Copy the ros image message to cv::Mat.
    cv_bridge::CvImageConstPtr cv_ptrLeft;
    try
    {
        cv_ptrLeft = cv_bridge::toCvShare(msgLeft);
    }
    catch (cv_bridge::Exception& e)
    {
        ROS_ERROR("cv_bridge exception: %s", e.what());
        return;
    }

    cv_bridge::CvImageConstPtr cv_ptrRight;
    try
    {
        cv_ptrRight = cv_bridge::toCvShare(msgRight);
    }
    catch (cv_bridge::Exception& e)
    {
        ROS_ERROR("cv_bridge exception: %s", e.what());
        return;
    }

    cv::Mat track_result;
    if(do_rectify)
    {
        cv::Mat imLeft, imRight;
        cv::remap(cv_ptrLeft->image,imLeft,M1l,M2l,cv::INTER_LINEAR);
        cv::remap(cv_ptrRight->image,imRight,M1r,M2r,cv::INTER_LINEAR);
        track_result = mpSLAM->TrackStereo(imLeft, imRight, cv_ptrLeft->header.stamp.toSec());
    }
    else
    {
        track_result = mpSLAM->TrackStereo(cv_ptrLeft->image, cv_ptrRight->image, cv_ptrLeft->header.stamp.toSec());
    }

    while(mpSLAM->LocalMappingStopped())
    {
        void();
    }

    // publish
    std::vector<std::pair<cv::Mat, double>> result_vector;
    mpSLAM->GetAllPoses(result_vector);
    nav_msgs::Path result_path;
    result_path.header.stamp = msgLeft->header.stamp;
    result_path.header.frame_id = "world";
    Eigen::Matrix4d temp_matrix, temp_matrix_inverse;
    Eigen::Matrix4d trans_form = Eigen::Matrix4d::Identity();
    // trans_form << 0,0,1,0, -1,0,0,0, 0,-1,0,0, 0,0,0,1;
    for(int i = 0; i < result_vector.size(); i++)
    {
        geometry_msgs::PoseStamped this_pose;
        for (int j = 0; j < receive_time_stamp.size(); j++)
            if (fabs(receive_time_stamp[j].toSec() - result_vector[i].second) < 0.001)
            {
                this_pose.header.stamp = receive_time_stamp[j];
                break;
            }

        for(int row_i = 0; row_i < 4; row_i ++)
            for(int col_i = 0; col_i < 4; col_i ++)
                temp_matrix(row_i, col_i) = result_vector[i].first.at<float>(row_i, col_i);

        temp_matrix_inverse = trans_form * temp_matrix.inverse();
        Eigen::Quaterniond rotation_q(temp_matrix_inverse.block<3, 3>(0, 0));
        this_pose.pose.position.x = temp_matrix_inverse(0, 3);
        this_pose.pose.position.y = temp_matrix_inverse(1, 3);
        this_pose.pose.position.z = temp_matrix_inverse(2, 3);
        this_pose.pose.orientation.x = rotation_q.x();
        this_pose.pose.orientation.y = rotation_q.y();
        this_pose.pose.orientation.z = rotation_q.z();
        this_pose.pose.orientation.w = rotation_q.w();
        result_path.poses.push_back(this_pose);
    }
    path_publish.publish(result_path);

    // get reference stamp
    double reference_stamp;
    reference_stamp = mpSLAM->GetRelativePose();
    int reference_index = 0;
    double time_diff = 1e9;
    for (int i = 0; i < result_vector.size(); i++)
    {
        double this_time_diff = fabs(result_vector[i].second - reference_stamp);
        if (this_time_diff < time_diff)
        {
            reference_index = i;
            time_diff = this_time_diff;
        }
    }
    if (time_diff < 0.01)
        printf("the reference keyframe is %d, keyframe number %d.\n", reference_index, result_vector.size());
    else
        printf("cannot find the reference keyframe! time difference %f, the stamp is %f, current is %f.!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n",
               time_diff,
               reference_stamp,
               msgLeft->header.stamp.toSec());

    // get keyframe decision
    LastKeyframeDecision = mpSLAM->GetKeyframeDecision();
    if (LastKeyframeDecision)
        printf("this is keyframe.\n");

    nav_msgs::Odometry this_odometry;
    this_odometry.header.stamp = msgLeft->header.stamp;
    this_odometry.header.frame_id = "world";
    Eigen::Matrix4d T_cw, T_wc;
    for (int row_i = 0; row_i < 4; row_i++)
        for (int col_i = 0; col_i < 4; col_i++)
            T_cw(row_i, col_i) = track_result.at<float>(row_i, col_i);
    T_wc = T_cw.inverse();
    Eigen::Quaterniond rotation_q(T_wc.block<3, 3>(0, 0));
    this_odometry.pose.pose.position.x = T_wc(0, 3);
    this_odometry.pose.pose.position.y = T_wc(1, 3);
    this_odometry.pose.pose.position.z = T_wc(2, 3);
    this_odometry.pose.pose.orientation.x = rotation_q.x();
    this_odometry.pose.pose.orientation.y = rotation_q.y();
    this_odometry.pose.pose.orientation.z = rotation_q.z();
    this_odometry.pose.pose.orientation.w = rotation_q.w();
    if (LastKeyframeDecision)
        this_odometry.pose.covariance[0] = 1;
    else
        this_odometry.pose.covariance[0] = 0;
    this_odometry.pose.covariance[1] = reference_index;
    pose_publish.publish(this_odometry);

    // get loop index
    sensor_msgs::PointCloud ros_loop_info;
    ros_loop_info.header.stamp = msgLeft->header.stamp;
    ros_loop_info.header.frame_id = "this_is_loop_info";
    std::vector<std::pair<double, double>> loop_result;
    mpSLAM->GetLoopInfo(loop_result);
    sensor_msgs::ChannelFloat32 loop_channel;
    for (int i = 0; i < loop_result.size() && i < 35; i++)
    {
        int first_index = -1;
        int second_index = -1;
        for (int j = 0; j < result_vector.size(); j++)
        {
            if (result_vector[j].second == loop_result[i].first)
                first_index = j;
            if (result_vector[j].second == loop_result[i].second)
                second_index = j;
        }
        if (first_index > 0 && second_index > 0)
        {
            printf("the loop info %d <---> %d\n", first_index, second_index);
            loop_channel.values.push_back(first_index);
            loop_channel.values.push_back(second_index);
        }
        else
            printf("cannot find corresponding!\n");
    }
    ros_loop_info.channels.push_back(loop_channel);
    loop_publish.publish(ros_loop_info);
}


