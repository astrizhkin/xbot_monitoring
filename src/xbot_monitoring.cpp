//
// Created by Clemens Elflein on 22.11.22.
// Copyright (c) 2022 Clemens Elflein. All rights reserved.
//
#include <filesystem>

#include "ros/ros.h"
#include <memory>
#include <boost/regex.hpp>
#include "xbot_msgs/SensorInfo.h"
#include "xbot_msgs/Map.h"
#include "xbot_msgs/SensorDataString.h"
#include "xbot_msgs/SensorDataDouble.h"
#include "xbot_msgs/RobotState.h"
#include "xbot_msgs/ActionData.h"
#include "xbot_msgs/RegisterActionsSrv.h"
#include "xbot_msgs/ActionInfo.h"
#include "xbot_msgs/MapOverlay.h"
#include <mqtt/async_client.h>
#include <nlohmann/json.hpp>
#include "geometry_msgs/Twist.h"
#include "std_msgs/String.h"
#include "nav_msgs/Path.h"

//#define SEND_VEL_COMD

using json = nlohmann::json;

void publish_sensor_metadata();
void publish_map();
void publish_map_overlay();
void publish_actions();
void publish_version();

// Stores registered actions (prefix to vector<action>)
std::map<std::string, std::vector<xbot_msgs::ActionInfo>> registered_actions;
std::vector<xbot_msgs::ActionInfo> last_registered_actions;
std::string last_actions_node;

// Maps a topic to a subscriber.
std::map<std::string, ros::Subscriber> active_subscribers;
std::map<std::string, xbot_msgs::SensorInfo> found_sensors;
std::vector<ros::Subscriber> sensor_data_subscribers;

ros::NodeHandle *n;

// The MQTT Client
std::shared_ptr<mqtt::async_client> client_;
std::shared_ptr<mqtt::async_client> client_external_;

std::mutex mqtt_callback_mutex;

// Publisher for cmd_vel and commands
ros::Publisher cmd_vel_pub;
ros::Publisher action_pub;
ros::Publisher action_ext_pub;

#ifdef SEND_VEL_COMD
geometry_msgs::Twist last_cmd_vel;
#endif

bool publish_json = false;

// properties for external mqtt
bool external_mqtt_enable = false;
std::string external_mqtt_username = "";
std::string external_mqtt_password = "";
std::string external_mqtt_hostname = "";
std::string external_mqtt_topic_prefix = "";
std::string external_mqtt_port = "";
std::string version_string = "";

class MqttCallback : public mqtt::callback {

    void connected(const mqtt::string &string) override {
        ROS_INFO_STREAM("[xbot_monitoring] MQTT Connected");
        publish_sensor_metadata();
        publish_map();
        publish_map_overlay();
        publish_actions();
        publish_version();

        // BEGIN: Deprecated code (1/2)
        // Earlier implementations subscribed to "/action" and "prefix//action" topics, we do it to not break stuff as well.
        client_->subscribe(this->mqtt_topic_prefix + "/teleop", 0);
        client_->subscribe(this->mqtt_topic_prefix + "/command", 0);
        client_->subscribe(this->mqtt_topic_prefix + "/action", 0);
        // END: Deprecated code (1/2)

        client_->subscribe(this->mqtt_topic_prefix + "teleop", 0);
        client_->subscribe(this->mqtt_topic_prefix + "command", 0);
        client_->subscribe(this->mqtt_topic_prefix + "action", 0);
        client_->subscribe(this->mqtt_topic_prefix + "actionJson", 0);
    }

public:
    void setMqttClient(std::shared_ptr<mqtt::async_client> c, const std::string &mqtt_topic_prefix) {
        this->client_ = std::move(c);
        this->mqtt_topic_prefix = mqtt_topic_prefix;
    }
    void message_arrived(mqtt::const_message_ptr ptr) override {
        if(ptr->get_topic() == this->mqtt_topic_prefix + "teleop") {
            try {
                json json = json::from_bson(ptr->get_payload().begin(), ptr->get_payload().end());
                geometry_msgs::Twist t;
                t.linear.x = json["vx"];
                t.angular.z = json["vz"];
                cmd_vel_pub.publish(t);
            } catch (const json::exception &e) {
                ROS_ERROR_STREAM("[xbot_monitoring] Error decoding /teleop bson: " << e.what());
            }
        } else if(ptr->get_topic() == this->mqtt_topic_prefix + "actionJson") {
            ROS_INFO_STREAM("[xbot_monitoring] Got json action: " + ptr->get_payload());
            xbot_msgs::ActionData action_msg;
            json json = json::parse(ptr->get_payload_str());
            if(!json.contains("action")){
                ROS_INFO_STREAM("[xbot_monitoring] No required 'action' json property " << json);
                return;
            }
            action_msg.action_id = json["action"];
            if(json.contains("parameters")) {
              action_msg.parameters = json["parameters"];
            }else{
              ROS_INFO_STREAM("[xbot_monitoring] json 'parameters' property not found");
            }
            //json &parametersJson = json.at("parameters");
            //std::map<std::string, bool> parmeters = json.at("parameters").get<std::map<std::string, bool>>();
            action_ext_pub.publish(action_msg);
        } else if(ptr->get_topic() == this->mqtt_topic_prefix + "action") {
            ROS_INFO_STREAM("[xbot_monitoring] Got action: " + ptr->get_payload());
            std_msgs::String action_msg;
            action_msg.data = ptr->get_payload_str();
            action_pub.publish(action_msg);
        } else if(ptr->get_topic() == this->mqtt_topic_prefix + "/action") {
            // BEGIN: Deprecated code (2/2)
            ROS_WARN_STREAM("[xbot_monitoring] Got action on deprecated topic! Change your topic names!: " + ptr->get_payload());
            std_msgs::String action_msg;
            action_msg.data = ptr->get_payload_str();
            action_pub.publish(action_msg);
            // END: Deprecated code (2/2)
        }
    }
private:
    std::shared_ptr<mqtt::async_client> client_;
    std::string mqtt_topic_prefix = "";
};

MqttCallback mqtt_callback;
MqttCallback mqtt_callback_external;

json map;
json map_overlay;
bool has_map = false;
bool has_map_overlay = false;

void setupMqttClient() {
    // setup mqtt client for app use
    {
        // MQTT connection options
        mqtt::connect_options connect_options_;

        // basic client connection options
        connect_options_.set_automatic_reconnect(true);
        connect_options_.set_clean_session(true);
        connect_options_.set_keep_alive_interval(1000);
        connect_options_.set_max_inflight(10);

        // create MQTT client
        std::string uri = "tcp" + std::string("://") + "127.0.0.1" +
                          std::string(":") + std::to_string(1883);

        try {
            client_ = std::make_shared<mqtt::async_client>(
                    uri, "xbot_monitoring");
            mqtt_callback.setMqttClient(client_, "");
            client_->set_callback(mqtt_callback);

            client_->connect(connect_options_);

        } catch (const mqtt::exception &e) {
            ROS_ERROR("[xbot_monitoring] Client could not be initialized: %s", e.what());
            exit(EXIT_FAILURE);
        }
    }
    // setup external mqtt client
    if(external_mqtt_enable) {
        // MQTT connection options
        mqtt::connect_options connect_options_;

        // basic client connection options
        connect_options_.set_automatic_reconnect(true);
        connect_options_.set_clean_session(true);
        connect_options_.set_keep_alive_interval(1000);
        connect_options_.set_max_inflight(10);

        if(!external_mqtt_username.empty()) {
            connect_options_.set_user_name(external_mqtt_username);
            connect_options_.set_password(external_mqtt_password);
        }

        // create MQTT client
        std::string uri = "tcp" + std::string("://") + external_mqtt_hostname +
                          std::string(":") + external_mqtt_port;

        try {
            client_external_ = std::make_shared<mqtt::async_client>(
                    uri, "ext_xbot_monitoring");
            mqtt_callback_external.setMqttClient(client_external_, external_mqtt_topic_prefix);
            client_external_->set_callback(mqtt_callback_external);

            client_external_->connect(connect_options_);

        } catch (const mqtt::exception &e) {
            ROS_ERROR("[xbot_monitoring] External Client could not be initialized: %s", e.what());
            exit(EXIT_FAILURE);
        }
    }
}

void try_publish(std::string topic, std::string data, bool retain = false) {
    if(publish_json) {
        try {
            if (retain) {
                // QOS 1 so that the data actually arrives at the client at least once.
                client_->publish(topic, data, 1, true);
            } else {
                client_->publish(topic, data);
            }
        } catch (const mqtt::exception &e) {
            // client disconnected or something, we drop it.
        }
    }
    // publish external
    if(external_mqtt_enable) {
        try {
            if (retain) {
                // QOS 1 so that the data actually arrives at the client at least once.
                client_external_->publish(external_mqtt_topic_prefix + topic, data, 1, true);
            } else {
                client_external_->publish(external_mqtt_topic_prefix + topic, data);
            }
        } catch (const mqtt::exception &e) {
            // client disconnected or something, we drop it.
        }
    }
}

void try_publish_binary(std::string topic, const void *data, size_t size, bool retain = false) {
    try {
        if (retain) {
            // QOS 1 so that the data actually arrives at the client at least once.
            client_->publish(topic, data, size, 1, true);
        } else {
            client_->publish(topic, data, size);
        }
    } catch (const mqtt::exception &e) {
        // client disconnected or something, we drop it.
    }
}

void publish_version() {
    json version = {
            {"version", version_string}
    };
    if(publish_json) {
        try_publish("version", version.dump(), true);
    }
    auto bson = json::to_bson(version);
    try_publish_binary("version", bson.data(), bson.size(), true);
}

void publish_sensor_metadata() {
    std::unique_lock<std::mutex> lk(mqtt_callback_mutex);

    if(found_sensors.empty())
        return;

    json sensor_info;
    for (const auto &kv: found_sensors) {
        json info;
        info["sensor_id"] = kv.second.sensor_id;
        info["sensor_name"] = kv.second.sensor_name;

        switch (kv.second.value_type) {
            case xbot_msgs::SensorInfo::TYPE_STRING: {
                info["value_type"] = "STRING";
                break;
            }
            case xbot_msgs::SensorInfo::TYPE_DOUBLE: {
                info["value_type"] = "DOUBLE";
                break;
            }
            default: {
                info["value_type"] = "UNKNOWN";
                break;
            }


        }

        switch (kv.second.value_description) {
            case xbot_msgs::SensorInfo::VALUE_DESCRIPTION_TEMPERATURE: {
                info["value_description"] = "TEMPERATURE";
                break;
            }
            case xbot_msgs::SensorInfo::VALUE_DESCRIPTION_VELOCITY: {
                info["value_description"] = "VELOCITY";
                break;
            }
            case xbot_msgs::SensorInfo::VALUE_DESCRIPTION_ACCELERATION: {
                info["value_description"] = "ACCELERATION";
                break;
            }
            case xbot_msgs::SensorInfo::VALUE_DESCRIPTION_VOLTAGE: {
                info["value_description"] = "VOLTAGE";
                break;
            }
            case xbot_msgs::SensorInfo::VALUE_DESCRIPTION_CURRENT: {
                info["value_description"] = "CURRENT";
                break;
            }
            case xbot_msgs::SensorInfo::VALUE_DESCRIPTION_PERCENT: {
                info["value_description"] = "PERCENT";
                break;
            }
            case xbot_msgs::SensorInfo::VALUE_DESCRIPTION_RPM: {
                info["value_description"] = "REVOLUTIONS";
                break;
            }
            case xbot_msgs::SensorInfo::VALUE_DESCRIPTION_DEGREE: {
                info["value_description"] = "DEGREE";
                break;
            }
            default: {
                info["value_description"] = "UNKNOWN";
                break;
            }
        }

        info["unit"] = kv.second.unit;
        info["has_min_max"] = kv.second.has_min_max;
        info["min_value"] = kv.second.min_value;
        info["max_value"] = kv.second.max_value;
        info["has_critical_low"] = kv.second.has_critical_low;
        info["lower_critical_value"] = kv.second.lower_critical_value;
        info["has_critical_high"] = kv.second.has_critical_high;
        info["upper_critical_value"] = kv.second.upper_critical_value;
        sensor_info.push_back(info);
    }
    if(publish_json) {
        try_publish("sensor_infos/json", sensor_info.dump(), true);
    }
    json data;
    data["d"] = sensor_info;
    auto bson = json::to_bson(data);
    try_publish_binary("sensor_infos/bson", bson.data(), bson.size(), true);
}

void subscribe_to_sensor(std::string topic) {
    auto &sensor = found_sensors[topic];

    ROS_INFO_STREAM("[xbot_monitoring] Subscribing to sensor data for sensor with name: " << sensor.sensor_name);

    std::string data_topic = "xbot_monitoring/sensors/" + sensor.sensor_id + "/data";

    switch (sensor.value_type) {
        case xbot_msgs::SensorInfo::TYPE_DOUBLE: {
            ros::Subscriber s = n->subscribe<xbot_msgs::SensorDataDouble>(data_topic, 10, [&info = sensor](
                    const xbot_msgs::SensorDataDouble::ConstPtr &msg) {
                if(publish_json) {
                    try_publish("sensors/" + info.sensor_id + "/data", std::to_string(msg->data));
                }

                json data;
                data["d"] = msg->data;
                auto bson = json::to_bson(data);
                try_publish_binary("sensors/" + info.sensor_id + "/bson", bson.data(), bson.size());
            });
            sensor_data_subscribers.push_back(s);
            break;
        }
        case xbot_msgs::SensorInfo::TYPE_STRING: {
            ros::Subscriber s = n->subscribe<xbot_msgs::SensorDataString>(data_topic, 10, [&info = sensor](
                    const xbot_msgs::SensorDataString::ConstPtr &msg) {
                if(publish_json) {
                        try_publish("sensors/" + info.sensor_id + "/data", msg->data);
                }

                json data;
                data["d"] = msg->data;
                auto bson = json::to_bson(data);
                try_publish_binary("sensors/" + info.sensor_id + "/bson", bson.data(), bson.size());
            });
            sensor_data_subscribers.push_back(s);
            break;
        }
        default: {
            ROS_ERROR_STREAM("[xbot_monitoring] Inavlid Sensor Data Type: " << (int) sensor.value_type);
        }
    }
}

void robot_state_callback(const xbot_msgs::RobotState::ConstPtr &msg) {
    // Build a JSON and publish it
    json j;

    j["battery_percentage"] = msg->battery_percentage;
    j["gps_percentage"] = msg->gps_percentage;
    j["current_action_progress"] = msg->current_action_progress;
    j["current_state"] = msg->current_state;
    j["current_sub_state"] = msg->current_sub_state;
    j["current_area"] = msg->current_area;
    j["current_path"] = msg->current_path;
    j["current_pose_index"] = msg->current_pose_index;
    j["emergency"] = msg->emergency;
    j["is_charging"] = msg->is_charging;
    j["pose"]["x"] = msg->robot_pose.pose.pose.position.x;
    j["pose"]["y"] = msg->robot_pose.pose.pose.position.y;
    j["pose"]["heading"] = msg->robot_pose.vehicle_heading;
    j["pose"]["pos_accuracy"] = msg->robot_pose.position_accuracy;
    j["pose"]["heading_accuracy"] = msg->robot_pose.orientation_accuracy;
    j["pose"]["heading_valid"] = msg->robot_pose.orientation_valid;
#ifdef SEND_VEL_COMD
    j["cmd_vel"]["x"] = last_cmd_vel.linear.x;
    j["cmd_vel"]["rz"] = last_cmd_vel.angular.z;
#endif

    if(publish_json) {
        try_publish("robot_state/json", j.dump());
    }
    json data;
    data["d"] = j;
    auto bson = json::to_bson(data);
    try_publish_binary("robot_state/bson", bson.data(), bson.size());
}

#ifdef SEND_VEL_COMD
void cmd_vel_callback(const geometry_msgs::Twist::ConstPtr &msg) {
    last_cmd_vel = *msg;
}
#endif

void publish_actions() {
    json actions = json::array();
    /*for(const auto &kv : registered_actions) {
        for(const auto &action : kv.second) {
            json action_info;
            action_info["action_id"] = kv.first + "/" + action.action_id;
            action_info["action_name"] = action.action_name;
            action_info["enabled"] = action.enabled;
            actions.push_back(action_info);
        }
    }*/
    for(const auto &action : last_registered_actions) {
        json action_info;
        action_info["action_id"] = last_actions_node + "/" + action.action_id;
        action_info["action_name"] = action.action_name;
        action_info["enabled"] = action.enabled;
        actions.push_back(action_info);
    }

    if(publish_json) {
        try_publish("actions/json", actions.dump(), true);
    }
    json data;
    data["d"] = actions;

    auto bson = json::to_bson(data);
    try_publish_binary("actions/bson", bson.data(), bson.size(), true);
}

void publish_map() {
    if(!has_map)
        return;
    if(publish_json) {
        try_publish("map/json", map.dump(), true);
    }
    json data;
    data["d"] = map;
    auto bson = json::to_bson(data);
    try_publish_binary("map/bson", bson.data(), bson.size(), true);
}

void publish_map_overlay() {
    if(!has_map_overlay)
        return;
    if(publish_json) {
        //retain false and QoS = 0 important because map overlay flood the MQTT during area recording
        try_publish("map_overlay/json", map_overlay.dump(), false);
    }
    json data;
    data["d"] = map_overlay;
    auto bson = json::to_bson(data);
    //retain false and QoS = 0 important because map overlay flood the MQTT during area recording
    try_publish_binary("map_overlay/bson", bson.data(), bson.size(), false);
}

void map_callback(const xbot_msgs::Map::ConstPtr &msg) {
    // Build a JSON and publish it
    json j;

    j["docking_pose"]["x"] = msg->dockX;
    j["docking_pose"]["y"] = msg->dockY;
    j["docking_pose"]["heading"] = msg->dockHeading;

    j["meta"]["mapWidth"] = msg->mapWidth;
    j["meta"]["mapHeight"] = msg->mapHeight;
    j["meta"]["mapCenterX"] = msg->mapCenterX;
    j["meta"]["mapCenterY"] = msg->mapCenterY;


    json areas_j;
    for(const auto &area : msg->areas) {
        json area_j;
        area_j["name"] = area.name;
        area_j["area_type"] = area.area_type;
        {
            json outline_poly_j;
            for (const auto &pt: area.area.points) {
                json p_j;
                p_j["x"] = pt.x;
                p_j["y"] = pt.y;
                outline_poly_j.push_back(p_j);
            }
            area_j["outline"] = outline_poly_j;
        }
        areas_j.push_back(area_j);
    }

    j["areas"] = areas_j;

    map = j;
    has_map = true;

    publish_map();
}

void convert_to_json_and_publish_map_overlay(const xbot_msgs::MapOverlay &mapOverlay) {
    // Build a JSON and publish it
    json polys;
    for(const auto &poly : mapOverlay.polygons) {
        if(poly.polygon.points.size() < 2)
            continue;
        json poly_j;
        {
            json outline_poly_j;
            for (const auto &pt: poly.polygon.points) {
                json p_j;
                p_j["x"] = pt.x;
                p_j["y"] = pt.y;
                outline_poly_j.push_back(p_j);
            }
            poly_j["poly"] = outline_poly_j;
            poly_j["is_closed"] = poly.closed;
            poly_j["line_width"] = poly.line_width;
            poly_j["color"] = poly.color;
        }
        polys.push_back(poly_j);
    }

    json j;
    j["polygons"] = polys;
    map_overlay = j;
    has_map_overlay = true;

    publish_map_overlay();
}

void map_overlay_callback(const xbot_msgs::MapOverlay::ConstPtr &msg) {
    convert_to_json_and_publish_map_overlay(*msg);
}

void plan_callback(const nav_msgs::Path::ConstPtr &msg) {
    xbot_msgs::MapOverlay mapOverlay;
    // push a new poly to the visualization overlay
    {
        xbot_msgs::MapOverlayPolygon poly_viz;
        poly_viz.closed = false;
        poly_viz.line_width = 0.1;
        poly_viz.color = "blue";
        mapOverlay.polygons.push_back(poly_viz);
    }
    auto &poly_viz = mapOverlay.polygons.back();
    for(auto &pose : msg->poses) {
        geometry_msgs::Point32 pt;
        pt.x = pose.pose.position.x;
        pt.y = pose.pose.position.y;
        pt.z = pose.pose.position.z;
        poly_viz.polygon.points.push_back(pt);
    }
    convert_to_json_and_publish_map_overlay(mapOverlay);
}

bool registerActions(xbot_msgs::RegisterActionsSrvRequest &req, xbot_msgs::RegisterActionsSrvResponse &res) {

    ROS_INFO_STREAM("[xbot_monitoring] new actions registered: " << req.node_prefix << " registered " << req.actions.size() << " actions.");

    registered_actions[req.node_prefix] = req.actions;
    last_registered_actions = req.actions;
    last_actions_node = req.node_prefix;

    publish_actions();
    return true;
}

int main(int argc, char **argv) {
    ros::init(argc, argv, "xbot_monitoring");
    has_map = false;
    has_map_overlay = false;


    n = new ros::NodeHandle();
    ros::NodeHandle paramNh("~");

    version_string = paramNh.param("software_version", std::string("UNKNOWN VERSION"));
    if(version_string.empty()) {
        version_string = "UNKNOWN VERSION";
    }

    publish_json = paramNh.param("publish_json", false);

    external_mqtt_enable = paramNh.param("external_mqtt_enable", false);
    external_mqtt_topic_prefix = paramNh.param("external_mqtt_topic_prefix", std::string(""));
    if(!external_mqtt_topic_prefix.empty() && external_mqtt_topic_prefix.back() != '/') {
        // append the /
        external_mqtt_topic_prefix = external_mqtt_topic_prefix+"/";
    }

    external_mqtt_hostname = paramNh.param("external_mqtt_hostname", std::string(""));
    external_mqtt_port = paramNh.param("external_mqtt_port", std::to_string(1883));
    external_mqtt_username = paramNh.param("external_mqtt_username", std::string(""));
    external_mqtt_password = paramNh.param("external_mqtt_password", std::string(""));

    if(external_mqtt_enable) {
        ROS_INFO_STREAM("[xbot_monitoring] Using extnernal MQTT broker: " << external_mqtt_hostname << ":" << external_mqtt_port << " with topic prefix: " + external_mqtt_topic_prefix);
    }

    // First setup MQTT
    setupMqttClient();

    ros::ServiceServer register_action_service = n->advertiseService("xbot/register_actions", registerActions);

    ros::Subscriber robotStateSubscriber = n->subscribe("xbot_monitoring/robot_state", 10, robot_state_callback);
    ros::Subscriber mapSubscriber = n->subscribe("xbot_monitoring/map", 10, map_callback);
#ifdef SEND_VEL_COMD
    ros::Subscriber cmdVelSubscriber = n->subscribe("cmd_vel", 10, cmd_vel_callback);
#endif
    ros::Subscriber mapOverlaySubscriber = n->subscribe("xbot_monitoring/map_overlay", 10, map_overlay_callback);

    ///move_base_flex/DockingFTCPlanner/global_point
    ///move_base_flex/FTCPlanner/global_point
    ros::Subscriber plan1Subscriber = n->subscribe("move_base_flex/DockingFTCPlanner/global_plan", 10, plan_callback);
    ros::Subscriber plan2Subscriber = n->subscribe("move_base_flex/FTCPlanner/global_plan", 10, plan_callback);
    

    cmd_vel_pub = n->advertise<geometry_msgs::Twist>("xbot_monitoring/remote_cmd_vel", 1);
    action_pub = n->advertise<std_msgs::String>("xbot/action", 1);
    action_ext_pub = n->advertise<xbot_msgs::ActionData>("xbot/action_ext", 1);

    ros::AsyncSpinner spinner(1);
    spinner.start();

    ros::Rate sensor_check_rate(10.0);

    boost::regex topic_regex("/xbot_monitoring/sensors/.*/info");

    while (ros::ok()) {
        // Read the topics in /xbot_monitoring/sensors/.*/info and subscribe to them.
        ros::master::V_TopicInfo topics;
        ros::master::getTopics(topics);
        std::for_each(topics.begin(), topics.end(), [&](const ros::master::TopicInfo &item) {
            
            if (!boost::regex_match(item.name, topic_regex) || active_subscribers.count(item.name) != 0)
                return;

            ROS_INFO_STREAM("[xbot_monitoring] Found new sensor topic " << item.name);
            active_subscribers[item.name] = n->subscribe<xbot_msgs::SensorInfo>(
                item.name, 1, [topic = item.name](const xbot_msgs::SensorInfo::ConstPtr &msg) {
                    ROS_INFO_STREAM("[xbot_monitoring] Got sensor info for sensor on topic " << msg->sensor_name << " on topic " << topic);
                    auto exist = found_sensors.count(topic);

                    // Sensor already known and sensor-info equals?
                    if(exist != 0 && found_sensors[topic] == *msg)
                        return;
                    
                    {
                        // Sensor is new or sensor-info differ from the buffered one
                        std::unique_lock<std::mutex> lk(mqtt_callback_mutex);
                        found_sensors[topic] = *msg;  // Save the (new|changed) sensor info
                    }

                    // Let the info subscription alive for dynamic threshold changes
                    //active_subscribers.erase(topic);  // Stop subscribing to infos

                    if (exist == 0) {
                        subscribe_to_sensor(topic);  // Subscribe for data
                    }

                    // Republish (new|changed) sensor info
                    // NOTE: If a sensor name or id changes, the related data topic wouldn't change!
                    //       But do we dynamically change a sensor name or id?
                    publish_sensor_metadata();
                }
            );
        });
        sensor_check_rate.sleep();
    }
    return 0;
}
