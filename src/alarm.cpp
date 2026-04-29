#include "alarm.h"
#include "common.h"

#include <mosquitto.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <unistd.h>

/*
 * 当前阶段 inference.cpp 暂时不产生检测结果，
 * 所以 alarm_thread 大多数时候不会发布消息。
 *
 * 等 YOLOv8 接入后，inference_thread 会把 InferResult 推入队列，
 * alarm_thread 再发布 MQTT JSON。
 */

void alarm_thread(
    RingQueue<InferResult, 4>& queue,
    std::atomic<bool>& running,
    const char* mqtt_host,
    int mqtt_port,
    const char* mqtt_topic
) {
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
