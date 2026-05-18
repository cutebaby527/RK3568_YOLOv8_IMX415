#include "alarm.h"
#include "common.h"
#include "rule_engine.h"

#include <mosquitto.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <unistd.h>

static uint64_t now_ms_for_rule() {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count()
    );
}

void alarm_thread(
    RingQueue<InferResult, 4>& queue,
    std::atomic<bool>& running,
    const char* mqtt_host,
    int mqtt_port,
    const char* mqtt_topic,
    RuleConfig rule_config,
    double conf_threshold
) {
    RuleEngine rule_engine(rule_config, conf_threshold);

    std::printf(
        "[rule] enabled target=%s conf=%.3f dwell=%dms cooldown=%dms roi_points=%zu\n",
        rule_config.target_class.c_str(),
        conf_threshold,
        rule_config.dwell_time_ms,
        rule_config.cooldown_ms,
        rule_config.roi.size()
    );

    mosquitto_lib_init();

    struct mosquitto* mosq = mosquitto_new(nullptr, true, nullptr);
    if (!mosq) {
        fprintf(stderr, "[alarm] mosquitto_new failed\n");
        mosquitto_lib_cleanup();
        return;
    }

    int ret = mosquitto_connect(mosq, mqtt_host, mqtt_port, 60);
    if (ret != MOSQ_ERR_SUCCESS) {
        fprintf(stderr,
                "[alarm] MQTT connect failed: host=%s port=%d ret=%d\n",
                mqtt_host,
                mqtt_port,
                ret);
        mosquitto_destroy(mosq);
        mosquitto_lib_cleanup();
        return;
    }

    printf("[alarm] MQTT connected: %s:%d topic=%s\n",
           mqtt_host,
           mqtt_port,
           mqtt_topic);

    while (running.load(std::memory_order_acquire)) {
        InferResult result;

        if (!queue.pop(result)) {
            usleep(10000);
            continue;
        }

        for (int i = 0; i < result.count && i < 64; ++i) {
            const DetectResult& d = result.results[i];

            RuleDetection rule_det;
            rule_det.class_name = d.name;
            rule_det.confidence = d.prop;
            rule_det.x1 = d.left;
            rule_det.y1 = d.top;
            rule_det.x2 = d.right;
            rule_det.y2 = d.bottom;

            const uint64_t now_ms = now_ms_for_rule();
            RuleResult rule_result = rule_engine.evaluate(rule_det, now_ms);

            if (rule_result.is_target && rule_result.confidence_ok && result.seq % 5 == 0) {
                std::printf(
                    "[rule] seq=%llu class=%s conf=%.3f center=[%d,%d] inside_roi=%d dwell=%llums reason=%s\n",
                    static_cast<unsigned long long>(result.seq),
                    rule_det.class_name.c_str(),
                    rule_det.confidence,
                    rule_result.center_x,
                    rule_result.center_y,
                    rule_result.inside_roi ? 1 : 0,
                    static_cast<unsigned long long>(rule_result.dwell_time_ms),
                    rule_result.reason.c_str()
                );
            }

            if (rule_result.alarm) {
                std::printf(
                    "[alarm] person_intrusion seq=%llu bbox=[%d,%d,%d,%d] center=[%d,%d] dwell=%llums\n",
                    static_cast<unsigned long long>(result.seq),
                    rule_det.x1,
                    rule_det.y1,
                    rule_det.x2,
                    rule_det.y2,
                    rule_result.center_x,
                    rule_result.center_y,
                    static_cast<unsigned long long>(rule_result.dwell_time_ms)
                );
            }
        }

        char payload[4096];
        int offset = 0;

        offset += snprintf(payload + offset,
                           sizeof(payload) - offset,
                           "{\"seq\":%llu,\"objects\":[",
                           static_cast<unsigned long long>(result.seq));

        for (int i = 0; i < result.count && i < 64; ++i) {
            const DetectResult& d = result.results[i];

            offset += snprintf(payload + offset,
                               sizeof(payload) - offset,
                               "%s{\"class\":\"%s\",\"conf\":%.3f,"
                               "\"box\":[%d,%d,%d,%d]}",
                               i == 0 ? "" : ",",
                               d.name,
                               d.prop,
                               d.left,
                               d.top,
                               d.right,
                               d.bottom);

            if (offset >= static_cast<int>(sizeof(payload)) - 256) {
                break;
            }
        }

        offset += snprintf(payload + offset,
                           sizeof(payload) - offset,
                           "]}");

        mosquitto_publish(mosq,
                          nullptr,
                          mqtt_topic,
                          static_cast<int>(strlen(payload)),
                          payload,
                          0,
                          false);
    }

    printf("[alarm] exiting\n");

    mosquitto_disconnect(mosq);
    mosquitto_destroy(mosq);
    mosquitto_lib_cleanup();
}
