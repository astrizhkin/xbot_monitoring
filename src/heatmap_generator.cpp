//
// Created by Clemens Elflein on 22.11.22.
// Copyright (c) 2022 Clemens Elflein. All rights reserved.
//
#include <filesystem>

#include "ros/ros.h"
#include "rosbag/bag.h"
#include "rosbag/view.h"
#include "xbot_msgs/SensorInfo.h"
#include "xbot_msgs/SensorDataDouble.h"
#include "xbot_msgs/AbsolutePose.h"
#include "grid_map_core/GridMap.hpp"
#include "grid_map_ros/GridMapRosConverter.hpp"
#include "xbot_msgs/Map.h"
#include <boost/algorithm/algorithm.hpp>
#include <boost/algorithm/string/erase.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/algorithm/string/classification.hpp>

ros::NodeHandle *n;

xbot_msgs::AbsolutePose last_pose{};
std::unordered_map<std::string, std::tuple<grid_map::GridMap, ros::Publisher,ros::Time>> pubsub_map{};
xbot_msgs::Map lastMap{};
bool has_map = false;

bool save(const std::string &mapName, const std::string &sensor_id, grid_map::GridMap &gridMap) {
    ROS_INFO_STREAM("[heatmap_generator] Save heatmap "<<mapName<<" for sensor "<<sensor_id);
    bool res = grid_map::GridMapRosConverter::saveToBag(gridMap,mapName + "_" + sensor_id + ".bag","gridmap");    
    if(!res) {
        ROS_ERROR("[heatmap_generator] Grid map save error");
    }
    return res;
    //rosbag::Bag bag;
    //ros::Time min_time = ros::TIME_MIN;
    //bag.open(mapName + "_" + sensor_id + ".bag", rosbag::bagmode::Write);
    //bag.write("pointcloud2", min_time, mapmsg);
    //bag.close();
}

bool load(const std::string &mapName, const std::string &sensor_id,grid_map::GridMap &gridMap) {
    ROS_INFO_STREAM("[heatmap_generator] Load heatmap "<<mapName<<" for sensor "<<sensor_id);
    try {
        bool res = grid_map::GridMapRosConverter::loadFromBag(mapName + "_" + sensor_id + ".bag","gridmap",gridMap);
        if(!res) {
            ROS_ERROR("[heatmap_generator] Grid map load error");
        }
        return res;
    } catch(rosbag::BagIOException &e) {
        return false;
    }

    // rosbag::Bag bag;
    // try {
    //     bag.open(mapName + "_" + sensor_id + ".bag");
    // } catch (rosbag::BagIOException &e) {
    //     ROS_WARN_STREAM("[heatmap_generator] Error opening stored heatmap "<<mapName<<" for sensor "<<sensor_id);
    //     return;
    // }
    // rosbag::View view(bag, rosbag::TopicQuery("pointcloud2"));
    // for (rosbag::MessageInstance const m : view) {
    //     sensor_msgs::PointCloud2Ptr pointCloud2 = m.instantiate<sensor_msgs::PointCloud2>();
    // }
}

void onMap(const xbot_msgs::Map &mapInfo) {
    if(!has_map || lastMap.mapCenterY != mapInfo.mapCenterY || lastMap.mapCenterX != mapInfo.mapCenterX || 
                   lastMap.mapWidth != mapInfo.mapWidth || lastMap.mapHeight != mapInfo.mapHeight ||
                   lastMap.name != mapInfo.name) {
        ROS_INFO("[heatmap_generator] Updated heatmap geometry.");
        for(auto & [key,value] : pubsub_map) {
            grid_map::GridMap &sensorMap = std::get<0>(value);
            sensorMap.setGeometry(grid_map::Length(mapInfo.mapWidth, mapInfo.mapHeight), 0.2,
                                        grid_map::Position(mapInfo.mapCenterX, mapInfo.mapCenterY));
            grid_map::GridMap loadMap;
            if(load(mapInfo.name, key, loadMap)){
                ROS_INFO_STREAM("[heatmap_generator] Copying "<<(int)loadMap.getLayers().size()<<" layers from loaded map "<<mapInfo.name<<" sensor "<<key);
                for (grid_map::GridMapIterator iterator(loadMap); !iterator.isPastEnd(); ++iterator) {
                    const grid_map::Index index(*iterator);
                    grid_map::Position indexPos;
                    loadMap.getPosition(index, indexPos);
                    for(auto &layerName : loadMap.getLayers()){
                        float val = loadMap.at(layerName, index);
                        if(!isnan(val)){
                            ROS_INFO_STREAM("[heatmap_generator] Copy from loaded map "<<mapInfo.name<<" sensor "<<key<<" layer "<<layerName<<" at pos "<<indexPos.x()<<","<<indexPos.y()<<" value "<<val);
                            sensorMap.atPosition(layerName, indexPos) = val;
                        }
                    }
                }
            } else {
                ROS_WARN_STREAM("[heatmap_generator] Unable to load map "<<mapInfo.name<<" sensor "<<key);
            }
        }
        has_map = true;
        lastMap = mapInfo;
    }
}

void onSensorData(const std::string& sensor_id, const xbot_msgs::SensorDataDouble::ConstPtr &msg) {
    if(!has_map) {
        return;
    }
    auto & [map, publisher, last_save] = pubsub_map[sensor_id];
    try {
        grid_map::Position pos(last_pose.pose.pose.position.x, last_pose.pose.pose.position.y);
        float oldValue = map.atPosition("intensity", pos);
        if(isnan(oldValue)) oldValue = 0;
        float oldCount = map.atPosition("count", pos);
        if(isnan(oldCount)) oldCount = 0; 

        float newValue = (oldValue * oldCount + msg->data) / (oldCount + 1);
        map.atPosition("intensity", pos) = newValue;
        map.atPosition("elevation", pos) = sensor_id == "om_height" ? newValue : 0;
        map.atPosition("count", pos) = oldCount + 1;
        //ROS_INFO_STREAM("[heatmap_generator] "<<sensor_id << " count=" << map.atPosition("count", pos) << " intensity=" << map.atPosition("intensity", pos) << " elevation="<<map.atPosition("elevation", pos));
    } catch (std::exception &e) {
        // error inserting into map, skip it
        ROS_WARN_STREAM("[heatmap_generator] Could not add point to heatmap: " << e.what());
        return;
    }

    sensor_msgs::PointCloud2 mapmsg;
    grid_map::GridMapRosConverter::toPointCloud(map, map.getLayers(), "elevation", mapmsg);
    publisher.publish(mapmsg);
    ros::Time now = ros::Time::now();
    if(now > last_save + ros::Duration(10.0)) {
        save(lastMap.name, sensor_id, map);
        last_save = now; 
    }
}

void onRobotPos(const xbot_msgs::AbsolutePose ::ConstPtr &msg) {
    last_pose = *msg;
}

int main(int argc, char **argv) {
    ros::init(argc, argv, "heatmap_generator");


    n = new ros::NodeHandle();
    ros::NodeHandle paramNh("~");

    std::vector<std::string> sensor_ids;
    std::string sensor_ids_string;
    if(!paramNh.getParam("sensor_ids", sensor_ids_string)) {
        ROS_ERROR("[heatmap_generator] Please specify sensor_ids for heatmap generation as comma separated list.");
        return 1;
    }

    boost::erase_all(sensor_ids_string, " ");
    boost::split ( sensor_ids, sensor_ids_string, boost::is_any_of(","));

    // Subscribe to robot position
    ros::Subscriber stateSubscriber = n->subscribe("xbot_positioning/xb_pose", 10, onRobotPos);

    // Subscribe to map info
    ros::Subscriber mapSubscriber = n->subscribe("xbot_monitoring/map", 1, onMap);

    std::vector<ros::Subscriber> sensorSubscribers{};
    for(const auto & sensor_id : sensor_ids)
    {
        ROS_INFO_STREAM("[heatmap_generator] Generating heatmap for sensor id: " << sensor_id);
        // Add layer to map
        grid_map::GridMap map{};
        map.add("intensity");
        map.add("elevation");
        map.add("count");
        map.setFrameId("map");
        // Create a publisher for the heatmap
        ros::Publisher p = n->advertise<sensor_msgs::PointCloud2>("heatmap/"+sensor_id, 10, true);
        // Subscribe to sensor data
        ros::Subscriber s = n->subscribe<xbot_msgs::SensorDataDouble>("xbot_monitoring/sensors/" + sensor_id + "/data", 10,
                                                        [sensor_id](const xbot_msgs::SensorDataDouble::ConstPtr &msg) {
                                                            onSensorData(sensor_id, msg);
                                                        });
        // store in map, so that they don't get deconstructed
        pubsub_map[sensor_id] = std::make_tuple(std::move(map), std::move(p), ros::Time::ZERO);
        sensorSubscribers.push_back(std::move(s));
    }

    ros::spin();
    return 0;
}
