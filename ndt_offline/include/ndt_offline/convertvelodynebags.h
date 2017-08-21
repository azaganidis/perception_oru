#ifndef CONVERTVELODYNEBAGS_H
#define CONVERTVELODYNEBAGS_H
/**
* Reads a bag file that contains
* 1) Velodyne raw messages
* 2) tf messages (e.g. as odometry)
* This class reads the _whole_ tf to cache and uses this to sync the velodyne messages with the motion of the platform.
* The result is returned as pcl::PointCloud<PointXYZI> in the sensor coordinates
*
* The class uses Velodyne ros pkg, with one hack:

Add the following to rawdata.h:
#include <angles/angles.h>

int setupOffline(std::string calibration_file, double max_range_, double min_range_)
{

    config_.max_range = max_range_;
    config_.min_range = min_range_;
    ROS_INFO_STREAM("data ranges to publish: ["
  << config_.min_range << ", "
  << config_.max_range << "]");

    config_.calibrationFile = calibration_file;

    ROS_INFO_STREAM("correction angles: " << config_.calibrationFile);

    calibration_.read(config_.calibrationFile);
    if (!calibration_.initialized) {
  ROS_ERROR_STREAM("Unable to open calibration file: " <<
      config_.calibrationFile);
  return -1;
    }

    // Set up cached values for sin and cos of all the possible headings
    for (uint16_t rot_index = 0; rot_index < ROTATION_MAX_UNITS; ++rot_index) {
  float rotation = angles::from_degrees(ROTATION_RESOLUTION * rot_index);
  cos_rot_table_[rot_index] = cosf(rotation);
  sin_rot_table_[rot_index] = sinf(rotation);
    }
    return 0;
}

*
* NOTE: In order for the synchronization of velodyne and vehicle motion to work
* you have to express the vehicle motion in the velodyne sensor coordinates.
* If your log file only contains odometry or similar for the vehicle you can give
* an extra link as a parameter (note that then give id to this extra link in constructor in this case).
*
* @author Jari Saarinen (jari.saarinen@aalto.fi)
*/

#include <ros/ros.h>
#include <rosbag/bag.h>
#include <rosbag/view.h>
#include <boost/foreach.hpp>
#include <sensor_msgs/PointCloud2.h>
#include <pcl/conversions.h>
#include <pcl/point_cloud.h>
#include <pcl/point_types.h>
#include <pcl/io/pcd_io.h>
#include <pcl/point_types.h>
#include <velodyne_pointcloud/rawdata.h>
#include <velodyne_pointcloud/point_types.h>
#include <ndt_offline/PoseInterpolationNavMsgsOdo.h>
#include <iostream>
#include <pcl_ros/impl/transforms.hpp>

#ifdef READ_RMLD_MESSAGES
#include<SynchronizedRMLD.h>
#endif
typedef pcl::PointCloud<pcl::PointXYZ> PointCloud;

class ConvertVelodyneBagsToPcl{
public:
  /**
   * Constructor
   * @param calibration_file path and name to your velodyne calibration file
   * @param bagfilename The path and name of the bagfile you want to handle
   * @param velodynetopic The topic that contains velodyne_msgs/VelodyneScan
   * @param tf_pose_id The id of the tf that you want to use
   * @param fixed_frame_id The name of the fixed frame in tf (default = "/world")
   * @param tftopic name of tf (default "/tf")
   * @param dur The buffer size (must be larger than the length of the bag file gives) default = 3600s
   * @param sensor_link An optional static link that takes e.g. your /odom to the sensor frame
   */
  ConvertVelodyneBagsToPcl(std::string outbag_name,
                           std::string calibration_file,
                           std::string bagfilename,
                           std::string velodynetopic,
                           std::string tf_pose_id,
                           std::string fixed_frame_id="/world",
                           std::string tftopic="/tf",
                           ros::Duration dur = ros::Duration(3600),
                           tf::StampedTransform *sensor_link=NULL,
                           double velodyne_max_range=130.0,
                           double velodyne_min_range=2.0,
                           double sensor_time_offset=0.0
      )

  {
    // The view_direction / view_width is important to have set. The min/max range is overwritten in the setupOffline
    outbag_name_=outbag_name;
    outbag.open(outbag_name,rosbag::bagmode::Write);
    double view_direction = 0;
    double view_width = 2*M_PI;
    dataParser.setParameters(velodyne_min_range,
                             velodyne_max_range,
                             view_direction,
                             view_width);

    dataParser.setupOffline(calibration_file, velodyne_max_range, velodyne_min_range);
    sensor_time_offset_ = ros::Duration(sensor_time_offset);
    fprintf(stderr,"Opening '%s'\n",bagfilename.c_str());

    bag.open(bagfilename, rosbag::bagmode::Read);

    velodynetopic_ = velodynetopic;
    tf_pose_id_ = tf_pose_id;

    std::vector<std::string> topics;
    topics.push_back(tftopic);
    topics.push_back(velodynetopic_);
    topics.push_back("diagnostics");
    topics.push_back("rosout");
    topics.push_back("rosout_agg");
    topics.push_back("velodyne_nodelet_manager/bond");
    topics.push_back("/vmc_navserver/encoders");
    topics.push_back("/vmc_navserver/laserway");
    topics.push_back("/vmc_navserver/odom");
    topics.push_back("/vmc_navserver/state");
    topics.push_back("/wifi_sniffer/wlan0");
    topics.push_back("/wifi_sniffer/wlan1");

#ifdef READ_RMLD_MESSAGES
    topics.push_back("/rmld/data");
    topics.push_back("/amtec/tilt_state");
#endif
    for(int i=0; i<topics.size(); ++i) {
      fprintf(stderr,"Searched Topic [%d] = '%s'\n",i,topics[i].c_str());
    }

    view = new rosbag::View(bag, rosbag::TopicQuery(topics));
    I = view->begin();

    odosync = new PoseInterpolationNavMsgsOdo(view,tftopic, fixed_frame_id,dur, sensor_link);
#ifdef READ_RMLD_MESSAGES
    rmldsync = new SynchronizedRMLD(view,tftopic,"/base_link",dur);
#endif
    //odosync = NULL;
  }

  /**
   * Reads the next measurment.
   * @param cloud The generated point cloud
   * @param sensor_pose [out] The pose of the sensor origin. (Utilizes the tf_pose_id and sensor_link from the constructor).
   **/
  bool ConvertToPclBag(tf::Transform &sensor_pose){
    if(I == view->end()){
      fprintf(stderr,"End of measurement file Reached!!\n");
      return false;
    }
    //if(odosync == NULL) return true;
    rosbag::MessageInstance const m = *I;
    if(m.getTopic()==velodynetopic_){
      PointCloud::Ptr cloud (new PointCloud);
      velodyne_msgs::VelodyneScan::ConstPtr scan = m.instantiate<velodyne_msgs::VelodyneScan>();
      if (scan != NULL){

        velodyne_rawdata::VPointCloud pnts,conv_points;
        // process each packet provided by the driver
        tf::Transform T;
        ros::Time t0=scan->header.stamp + sensor_time_offset_;
        timestamp_of_last_sensor_message=t0;
        if(odosync->getTransformationForTime(t0, tf_pose_id_, sensor_pose)){
          for (size_t next = 0; next < scan->packets.size(); ++next){
            dataParser.unpack(scan->packets[next], pnts); // unpack the raw data
            ros::Time t1=scan->packets[next].stamp + sensor_time_offset_;

            if(odosync->getTransformationForTime(t0,t1,tf_pose_id_,T)){
              pcl_ros::transformPointCloud(pnts,conv_points,T);
              for(size_t i = 0;i<pnts.size();i++){
               // PointT p;
                pcl::PointXYZ p;
                p.x = conv_points.points[i].x; p.y=conv_points.points[i].y; p.z=conv_points.points[i].z;
                cloud->push_back(p);
              }
            }else{
              //fprintf(stderr,"No transformation\n");
            }
            pnts.clear();
          }

        }else{
          fprintf(stderr,"No transformation\n");
        }

      /*  geometry_msgs::Point32 p;
        sensor_msgs::PointCloud cld;
        for(int i=0;i<cloud.size();i++){
          p.x=cloud[i].x;
          p.y=cloud[i].y;
          p.z=cloud[i].z;
          cld.points.push_back(p);*/

        std::cout<<"Frame:"<<++counter<<", size:"<<cloud->size()<<std::endl;
        cloud->header.frame_id="/velodyne";
        pcl_conversions::toPCL( timestamp_of_last_sensor_message,cloud->header.stamp);
        outbag.write("/sensor_lidar",timestamp_of_last_sensor_message,cloud);
        //cld.header.frame_id=cloud.header.frame_id;
        //cld.header.stamp=timestamp_of_last_sensor_message;
        //outbag.write("/sensor_lidar",timestamp_of_last_sensor_message,  cld);
        }
    }
    else{
      // std::cout<<"wrinting topic="<<m.getTopic()<<std::endl;
      outbag.write(m.getTopic(),m.getTime(),  m);
    }


    I++;
    return true;
  }
  void CloseOutputBag(){
    outbag.close();
  }


  /**
       * Get pose for latest measurement with pose id
       */
  bool getPoseFor(tf::Transform &pose, std::string pose_id){
    if(odosync->getTransformationForTime(timestamp_of_last_sensor_message,pose_id,pose)){
      return true;
    }
    return false;
  }


  ros::Time getTimeStampOfLastSensorMsg() const {
    return timestamp_of_last_sensor_message;
  }


private:
  unsigned int counter=0;
  rosbag::Bag outbag;
  std::string outbag_name_;
  PoseInterpolationNavMsgsOdo *odosync;
  velodyne_rawdata::RawData dataParser;
  rosbag::Bag bag;
  rosbag::View *view;
  rosbag::View::iterator I;
  std::string velodynetopic_;
  std::string tf_pose_id_;
  velodyne_msgs::VelodyneScan::ConstPtr global_scan;
  ros::Time timestamp_of_last_sensor_message;
  ros::Duration sensor_time_offset_;

};

#endif // CONVERTVELODYNEBAGS_H
