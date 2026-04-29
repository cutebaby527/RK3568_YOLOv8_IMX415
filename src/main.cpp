#include "common.h"
#include "capture.h"
#include "inference.h"
#include "alarm.h"

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <chrono>

static std::atomic<bool>* g_running = nullptr;

static void signal_handler(int signo) {
    if (signo == SIGINT || signo == SIGTERM) {
        if (g_running) {
            g_running->store(false);
        }
    }
}

int main(int argc, char** argv) {
    const char* model_path = argc > 1 ? argv[1] : "../model/yolov8.rknn";
    const char* mqtt_host = argc > 2 ? argv[2] : "127.0.0.1";
    int mqtt_port = argc > 3 ? atoi(argv[3]) : 1883;
    const char* mqtt_topic = argc > 4 ? argv[4] : "edge/detect";

    printf("========================================\n");
    printf(" RK3568 YOLOv8 Edge AI Pipeline\n");
    printf(" Temporary NV12 passthrough stage\n");
    printf("========================================\n");
    printf("[main] model path : %s\n", model_path);
    printf("[main] mqtt host  : %s\n", mqtt_host);
    printf("[main] mqtt port  : %d\n", mqtt_port);
    printf("[main] mqtt topic : %s\n", mqtt_topic);
    printf("[main] camera     : /dev/video0\n");
    printf("[main] format     : %dx%d NV12\n", FRAME_WIDTH, FRAME_HEIGHT);
    printf("========================================\n");

    std::atomic<bool> running(true);
    g_running = &running;

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    RingQueue<RawFrame, 4> capture_queue;
    RingQueue<InferResult, 4> infer_queue;

    std::thread cap_thread(
        capture_thread,
        std::ref(capture_queue),
        std::ref(running)
    );

    std::thread inf_thread(
        inference_thread,
        std::ref(capture_queue),
        std::ref(infer_queue),
        std::ref(running),
        model_path
    );

    std::thread alm_thread(
        alarm_thread,
        std::ref(infer_queue),
        std::ref(running),
        mqtt_host,
        mqtt_port,
        mqtt_topic
    );

    printf("[main] all threads started\n");
    printf("[main] press Ctrl+C to stop\n");

    while (running.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    printf("[main] stopping...\n");

    if (cap_thread.joinable()) {
        cap_thread.join();
    }

    if (inf_thread.joinable()) {
        inf_thread.join();
    }

    if (alm_thread.joinable()) {
        alm_thread.join();
    }

    printf("[main] exited\n");

    return 0;
}
