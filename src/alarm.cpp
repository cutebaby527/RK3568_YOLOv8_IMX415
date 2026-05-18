#include "alarm.h"
#include "common.h"
#include "rule_engine.h"
#include "snapshot.h"
#include "metrics.h"

#include <mosquitto.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <unistd.h>

static uint64_t now_ms_for_rule() {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count()
    );
}

static uint64_t unix_time_ms() {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count()
    );
}

static std::string make_event_id(uint64_t alarm_seq) {
    const std::time_t now = std::time(nullptr);

    struct tm tm_buf {};
    localtime_r(&now, &tm_buf);

    char buf[64];
    std::snprintf(
        buf,
        sizeof(buf),
        "%04d%02d%02d-%02d%02d%02d-%04llu",
        tm_buf.tm_year + 1900,
        tm_buf.tm_mon + 1,
        tm_buf.tm_mday,
        tm_buf.tm_hour,
        tm_buf.tm_min,
        tm_buf.tm_sec,
        static_cast<unsigned long long>(alarm_seq % 10000)
    );

    return std::string(buf);
}

static void publish_raw_detection(
    struct mosquitto* mosq,
    const char* topic,
    const InferResult& result
) {
    char payload[4096];
    int offset = 0;

    offset += std::snprintf(
        payload + offset,
        sizeof(payload) - offset,
        "{\"seq\":%llu,\"objects\":[",
        static_cast<unsigned long long>(result.seq)
    );

    for (int i = 0; i < result.count && i < 64; ++i) {
        const DetectResult& d = result.results[i];

        offset += std::snprintf(
            payload + offset,
            sizeof(payload) - offset,
            "%s{\"class\":\"%s\",\"conf\":%.3f,"
            "\"box\":[%d,%d,%d,%d]}",
            i == 0 ? "" : ",",
            d.name,
            d.prop,
            d.left,
            d.top,
            d.right,
            d.bottom
        );

        if (offset >= static_cast<int>(sizeof(payload)) - 256) {
            break;
        }
    }

    offset += std::snprintf(
        payload + offset,
        sizeof(payload) - offset,
        "]}"
    );

    mosquitto_publish(
        mosq,
        nullptr,
        topic,
        static_cast<int>(std::strlen(payload)),
        payload,
        0,
        false
    );
}

static bool publish_alarm_event(
    struct mosquitto* mosq,
    const char* topic,
    const std::string& event_id,
    const RuleDetection& det,
    const RuleResult& rule_result,
    const std::string& snapshot_path
) {
    char payload[2048];

    std::snprintf(
        payload,
        sizeof(payload),
        "{"
        "\"event_id\":\"%s\","
        "\"event_type\":\"person_intrusion\","
        "\"camera_id\":\"rk3568-imx415-01\","
        "\"timestamp\":%llu,"
        "\"level\":\"warning\","
        "\"object\":{"
            "\"class\":\"%s\","
            "\"confidence\":%.3f,"
            "\"bbox\":[%d,%d,%d,%d]"
        "},"
        "\"rule\":{"
            "\"roi_name\":\"restricted_area_1\","
            "\"dwell_time_ms\":%llu"
        "},"
        "\"snapshot\":\"%s\""
        "}",
        event_id.c_str(),
        static_cast<unsigned long long>(unix_time_ms()),
        det.class_name.c_str(),
        det.confidence,
        det.x1,
        det.y1,
        det.x2,
        det.y2,
        static_cast<unsigned long long>(rule_result.dwell_time_ms),
        snapshot_path.c_str()
    );

    const int ret = mosquitto_publish(
        mosq,
        nullptr,
        topic,
        static_cast<int>(std::strlen(payload)),
        payload,
        0,
        false
    );

    if (ret == MOSQ_ERR_SUCCESS) {
        std::printf(
            "[mqtt] alarm published topic=%s event_id=%s\n",
            topic,
            event_id.c_str()
        );
        return true;
    }

    std::fprintf(
        stderr,
        "[mqtt] alarm publish failed topic=%s event_id=%s ret=%d\n",
        topic,
        event_id.c_str(),
        ret
    );

    return false;
}


static void publish_metrics_event(
    struct mosquitto* mosq,
    const char* topic,
    const MetricsSnapshot& snapshot,
    double inference_fps
) {
    char payload[1024];

    std::snprintf(
        payload,
        sizeof(payload),
        "{"
        "\"uptime_sec\":%llu,"
        "\"inference_fps\":%.2f,"
        "\"inference_frame_count\":%llu,"
        "\"alarm_count\":%llu,"
        "\"mqtt_detect_publish_count\":%llu,"
        "\"mqtt_alarm_publish_count\":%llu,"
        "\"last_alarm_ts\":%llu"
        "}",
        static_cast<unsigned long long>(snapshot.uptime_sec),
        inference_fps,
        static_cast<unsigned long long>(snapshot.inference_frame_count),
        static_cast<unsigned long long>(snapshot.alarm_count),
        static_cast<unsigned long long>(snapshot.mqtt_detect_publish_count),
        static_cast<unsigned long long>(snapshot.mqtt_alarm_publish_count),
        static_cast<unsigned long long>(snapshot.last_alarm_ts_ms)
    );

    const int ret = mosquitto_publish(
        mosq,
        nullptr,
        topic,
        static_cast<int>(std::strlen(payload)),
        payload,
        0,
        false
    );

    if (ret == MOSQ_ERR_SUCCESS) {
        std::printf("[metrics] topic=%s payload=%s\n", topic, payload);
    } else {
        std::fprintf(stderr, "[metrics] publish failed topic=%s ret=%d\n", topic, ret);
    }
}

void alarm_thread(
    RingQueue<InferResult, 4>& queue,
    std::atomic<bool>& running,
    const char* mqtt_host,
    int mqtt_port,
    const char* mqtt_detect_topic,
    const char* mqtt_alarm_topic,
    const char* mqtt_metrics_topic,
    RuleConfig rule_config,
    double conf_threshold,
    SnapshotConfig snapshot_config
) {
    RuleEngine rule_engine(rule_config, conf_threshold);
    uint64_t alarm_seq = 0;

    uint64_t last_metrics_ms = now_ms_for_rule();
    uint64_t last_metrics_inference_count = 0;

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
        std::fprintf(stderr, "[alarm] mosquitto_new failed\n");
        mosquitto_lib_cleanup();
        return;
    }

    int ret = mosquitto_connect(mosq, mqtt_host, mqtt_port, 60);
    if (ret != MOSQ_ERR_SUCCESS) {
        std::fprintf(
            stderr,
            "[alarm] MQTT connect failed: host=%s port=%d ret=%d\n",
            mqtt_host,
            mqtt_port,
            ret
        );
        mosquitto_destroy(mosq);
        mosquitto_lib_cleanup();
        return;
    }

    std::printf(
        "[alarm] MQTT connected: %s:%d detect_topic=%s alarm_topic=%s metrics_topic=%s\n",
        mqtt_host,
        mqtt_port,
        mqtt_detect_topic,
        mqtt_alarm_topic,
        mqtt_metrics_topic
    );

    while (running.load(std::memory_order_acquire)) {
        const uint64_t metrics_now_ms = now_ms_for_rule();
        if (metrics_now_ms >= last_metrics_ms + 5000) {
            MetricsSnapshot snapshot = metrics_get_snapshot();

            const uint64_t delta_frames =
                snapshot.inference_frame_count >= last_metrics_inference_count
                    ? snapshot.inference_frame_count - last_metrics_inference_count
                    : 0;

            const uint64_t delta_ms = metrics_now_ms - last_metrics_ms;
            const double inference_fps =
                delta_ms > 0 ? static_cast<double>(delta_frames) * 1000.0 / static_cast<double>(delta_ms) : 0.0;

            publish_metrics_event(
                mosq,
                mqtt_metrics_topic,
                snapshot,
                inference_fps
            );

            last_metrics_ms = metrics_now_ms;
            last_metrics_inference_count = snapshot.inference_frame_count;
        }

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
                const std::string event_id = make_event_id(++alarm_seq);
                metrics_record_alarm(unix_time_ms());

                std::printf(
                    "[alarm] person_intrusion event_id=%s seq=%llu bbox=[%d,%d,%d,%d] center=[%d,%d] dwell=%llums\n",
                    event_id.c_str(),
                    static_cast<unsigned long long>(result.seq),
                    rule_det.x1,
                    rule_det.y1,
                    rule_det.x2,
                    rule_det.y2,
                    rule_result.center_x,
                    rule_result.center_y,
                    static_cast<unsigned long long>(rule_result.dwell_time_ms)
                );

                std::string snapshot_path;
                std::string snapshot_error;

                if (save_alarm_snapshot(
                        event_id,
                        rule_det,
                        rule_config.roi,
                        snapshot_config,
                        snapshot_path,
                        snapshot_error)) {
                    if (!snapshot_path.empty()) {
                        std::printf(
                            "[snapshot] saved path=%s\n",
                            snapshot_path.c_str()
                        );
                    }
                } else {
                    std::fprintf(
                        stderr,
                        "[snapshot] save failed event_id=%s error=%s\n",
                        event_id.c_str(),
                        snapshot_error.c_str()
                    );
                }

                if (publish_alarm_event(
                        mosq,
                        mqtt_alarm_topic,
                        event_id,
                        rule_det,
                        rule_result,
                        snapshot_path)) {
                    metrics_record_mqtt_alarm_publish();
                }
            }
        }

        publish_raw_detection(mosq, mqtt_detect_topic, result);
        metrics_record_mqtt_detect_publish();
    }

    std::printf("[alarm] exiting\n");

    mosquitto_disconnect(mosq);
    mosquitto_destroy(mosq);
    mosquitto_lib_cleanup();
}
