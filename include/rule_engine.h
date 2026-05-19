#pragma once

#include "config.h"

#include <cstdint>
#include <string>

struct RuleDetection {
    std::string class_name;
    float confidence = 0.0f;
    int x1 = 0;
    int y1 = 0;
    int x2 = 0;
    int y2 = 0;
};

struct RuleResult {
    bool is_target = false;
    bool confidence_ok = false;
    bool inside_roi = false;
    bool alarm = false;

    int center_x = 0;
    int center_y = 0;

    uint64_t dwell_time_ms = 0;
    std::string reason;
};

class RuleEngine {
public:
    RuleEngine();
    RuleEngine(const RuleConfig& rule_config, double conf_threshold);

    void configure(const RuleConfig& rule_config, double conf_threshold);
    void reset();

    RuleResult evaluate(const RuleDetection& detection, uint64_t now_ms);
    bool isInsideRoi(int x, int y) const;

private:
    bool pointOnSegment(int x, int y, const RoiPoint& a, const RoiPoint& b) const;

private:
    RuleConfig rule_config_;
    double conf_threshold_ = 0.45;

    bool dwell_active_ = false;
    uint64_t dwell_start_ms_ = 0;

    bool has_last_alarm_ = false;
    uint64_t last_alarm_ms_ = 0;
};
