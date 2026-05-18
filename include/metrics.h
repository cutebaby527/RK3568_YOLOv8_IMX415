#pragma once

#include <cstdint>

struct MetricsSnapshot {
    uint64_t uptime_sec = 0;

    uint64_t inference_frame_count = 0;
    uint64_t alarm_count = 0;

    uint64_t mqtt_detect_publish_count = 0;
    uint64_t mqtt_alarm_publish_count = 0;

    uint64_t last_alarm_ts_ms = 0;
};

void metrics_init();

void metrics_record_inference_frame();
void metrics_record_alarm(uint64_t alarm_ts_ms);
void metrics_record_mqtt_detect_publish();
void metrics_record_mqtt_alarm_publish();

MetricsSnapshot metrics_get_snapshot();
