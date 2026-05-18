#pragma once

#include "common.h"
#include "config.h"

#include <atomic>

void alarm_thread(
    RingQueue<InferResult, 4>& queue,
    std::atomic<bool>& running,
    const char* mqtt_host,
    int mqtt_port,
    const char* mqtt_topic,
    RuleConfig rule_config,
    double conf_threshold
);
