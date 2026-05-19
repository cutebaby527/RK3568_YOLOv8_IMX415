#include "rule_engine.h"

#include <cmath>

RuleEngine::RuleEngine() = default;

RuleEngine::RuleEngine(const RuleConfig& rule_config, double conf_threshold) {
    configure(rule_config, conf_threshold);
}

void RuleEngine::configure(const RuleConfig& rule_config, double conf_threshold) {
    rule_config_ = rule_config;
    conf_threshold_ = conf_threshold;
    reset();
}

void RuleEngine::reset() {
    dwell_active_ = false;
    dwell_start_ms_ = 0;
    has_last_alarm_ = false;
    last_alarm_ms_ = 0;
}

bool RuleEngine::pointOnSegment(int x, int y, const RoiPoint& a, const RoiPoint& b) const {
    const long long cross =
        static_cast<long long>(x - a.x) * static_cast<long long>(b.y - a.y) -
        static_cast<long long>(y - a.y) * static_cast<long long>(b.x - a.x);

    if (cross != 0) {
        return false;
    }

    const int min_x = a.x < b.x ? a.x : b.x;
    const int max_x = a.x > b.x ? a.x : b.x;
    const int min_y = a.y < b.y ? a.y : b.y;
    const int max_y = a.y > b.y ? a.y : b.y;

    return x >= min_x && x <= max_x && y >= min_y && y <= max_y;
}

bool RuleEngine::isInsideRoi(int x, int y) const {
    const auto& polygon = rule_config_.roi;

    if (polygon.size() < 3) {
        return false;
    }

    for (size_t i = 0; i < polygon.size(); ++i) {
        const RoiPoint& a = polygon[i];
        const RoiPoint& b = polygon[(i + 1) % polygon.size()];
        if (pointOnSegment(x, y, a, b)) {
            return true;
        }
    }

    bool inside = false;

    for (size_t i = 0, j = polygon.size() - 1; i < polygon.size(); j = i++) {
        const int xi = polygon[i].x;
        const int yi = polygon[i].y;
        const int xj = polygon[j].x;
        const int yj = polygon[j].y;

        const bool intersect =
            ((yi > y) != (yj > y)) &&
            (x < (static_cast<double>(xj - xi) * static_cast<double>(y - yi) /
                  static_cast<double>(yj - yi) + xi));

        if (intersect) {
            inside = !inside;
        }
    }

    return inside;
}

RuleResult RuleEngine::evaluate(const RuleDetection& detection, uint64_t now_ms) {
    RuleResult result;

    result.center_x = (detection.x1 + detection.x2) / 2;
    result.center_y = (detection.y1 + detection.y2) / 2;

    result.is_target = (detection.class_name == rule_config_.target_class);
    if (!result.is_target) {
        result.reason = "class_mismatch";
        dwell_active_ = false;
        dwell_start_ms_ = 0;
        return result;
    }

    result.confidence_ok = (detection.confidence >= conf_threshold_);
    if (!result.confidence_ok) {
        result.reason = "low_confidence";
        dwell_active_ = false;
        dwell_start_ms_ = 0;
        return result;
    }

    result.inside_roi = isInsideRoi(result.center_x, result.center_y);
    if (!result.inside_roi) {
        result.reason = "outside_roi";
        dwell_active_ = false;
        dwell_start_ms_ = 0;
        return result;
    }

    if (!dwell_active_) {
        dwell_active_ = true;
        dwell_start_ms_ = now_ms;
    }

    result.dwell_time_ms = now_ms >= dwell_start_ms_ ? now_ms - dwell_start_ms_ : 0;

    if (result.dwell_time_ms < static_cast<uint64_t>(rule_config_.dwell_time_ms)) {
        result.reason = "dwell_time_not_reached";
        return result;
    }

    const bool cooldown_ok =
        !has_last_alarm_ ||
        now_ms >= last_alarm_ms_ + static_cast<uint64_t>(rule_config_.cooldown_ms);

    if (!cooldown_ok) {
        result.reason = "cooldown";
        return result;
    }

    result.alarm = true;
    result.reason = "alarm";
    has_last_alarm_ = true;
    last_alarm_ms_ = now_ms;

    return result;
}
