#pragma once

#include <string>
#include <vector>

struct RoiPoint {
    int x = 0;
    int y = 0;
};

struct CameraConfig {
    std::string device = "/dev/video0";
    int width = 640;
    int height = 480;
    std::string format = "NV12";
};

struct ModelConfig {
    std::string path = "../model/yolov8.rknn";
    int input_width = 640;
    int input_height = 640;
    double conf_threshold = 0.45;
    double nms_threshold = 0.45;
};

struct RuleConfig {
    std::string target_class = "person";
    std::vector<RoiPoint> roi;
    int dwell_time_ms = 1000;
    int cooldown_ms = 5000;
};

struct MqttConfig {
    std::string host = "127.0.0.1";
    int port = 1883;
    std::string topic_alarm = "edge/detect";
    std::string topic_status = "edge/person/status";
    std::string topic_metrics = "edge/person/metrics";
};

struct WebConfig {
    int port = 8080;
};

struct SnapshotConfig {
    bool enable = true;
    std::string save_dir = "../events";
};

struct AppConfig {
    CameraConfig camera;
    ModelConfig model;
    RuleConfig rule;
    MqttConfig mqtt;
    WebConfig web;
    SnapshotConfig snapshot;
};

bool load_app_config(const std::string& path, AppConfig& config, std::string& error);
void print_app_config(const AppConfig& config);
