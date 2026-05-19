#include "metrics.h"

#include <atomic>
#include <chrono>

static std::atomic<uint64_t> g_start_ms{0};

static std::atomic<uint64_t> g_inference_frame_count{0};
static std::atomic<uint64_t> g_alarm_count{0};

static std::atomic<uint64_t> g_mqtt_detect_publish_count{0};
static std::atomic<uint64_t> g_mqtt_alarm_publish_count{0};

static std::atomic<uint64_t> g_last_alarm_ts_ms{0};

static uint64_t steady_ms() {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count()
    );
}

void metrics_init() {
    g_start_ms.store(steady_ms(), std::memory_order_release);

    g_inference_frame_count.store(0, std::memory_order_release);
    g_alarm_count.store(0, std::memory_order_release);

    g_mqtt_detect_publish_count.store(0, std::memory_order_release);
    g_mqtt_alarm_publish_count.store(0, std::memory_order_release);

    g_last_alarm_ts_ms.store(0, std::memory_order_release);
}

void metrics_record_inference_frame() {
    g_inference_frame_count.fetch_add(1, std::memory_order_relaxed);
}

void metrics_record_alarm(uint64_t alarm_ts_ms) {
    g_alarm_count.fetch_add(1, std::memory_order_relaxed);
    g_last_alarm_ts_ms.store(alarm_ts_ms, std::memory_order_release);
}

void metrics_record_mqtt_detect_publish() {
    g_mqtt_detect_publish_count.fetch_add(1, std::memory_order_relaxed);
}

void metrics_record_mqtt_alarm_publish() {
    g_mqtt_alarm_publish_count.fetch_add(1, std::memory_order_relaxed);
}

MetricsSnapshot metrics_get_snapshot() {
    MetricsSnapshot s;

    const uint64_t start = g_start_ms.load(std::memory_order_acquire);
    const uint64_t now = steady_ms();

    if (start > 0 && now >= start) {
        s.uptime_sec = (now - start) / 1000;
    }

    s.inference_frame_count =
        g_inference_frame_count.load(std::memory_order_acquire);

    s.alarm_count =
        g_alarm_count.load(std::memory_order_acquire);

    s.mqtt_detect_publish_count =
        g_mqtt_detect_publish_count.load(std::memory_order_acquire);

    s.mqtt_alarm_publish_count =
        g_mqtt_alarm_publish_count.load(std::memory_order_acquire);

    s.last_alarm_ts_ms =
        g_last_alarm_ts_ms.load(std::memory_order_acquire);

    return s;
}
