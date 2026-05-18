#include "common.h"
#include "capture.h"
#include "inference.h"
#include "alarm.h"
#include "config.h"

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <chrono>
#include <string>
#include <vector>

static std::atomic<bool>* g_running = nullptr;

static void signal_handler(int signo) {
    if (signo == SIGINT || signo == SIGTERM) {
        if (g_running) {
            g_running->store(false);
        }
    }
}

static void print_usage(const char* program) {
    std::printf("Usage:\n");
    std::printf("  %s [model_path] [mqtt_host] [mqtt_port] [mqtt_topic]\n", program);
    std::printf("  %s --config <config.json> [model_path] [mqtt_host] [mqtt_port] [mqtt_topic]\n", program);
    std::printf("\n");
    std::printf("Examples:\n");
    std::printf("  %s ../model/yolov8.rknn\n", program);
    std::printf("  %s --config ../config/config.json\n", program);
}

int main(int argc, char** argv) {
    AppConfig app_config;
    bool config_loaded = false;
    std::string config_path;
    std::vector<std::string> positional_args;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return 0;
        }

        if (arg == "--config") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "[main] missing value after --config\n");
                print_usage(argv[0]);
                return 1;
            }
            config_path = argv[++i];
            continue;
        }

        positional_args.push_back(arg);
    }

    if (!config_path.empty()) {
        std::string error;
        if (!load_app_config(config_path, app_config, error)) {
            std::fprintf(stderr, "[main] failed to load config: %s\n", error.c_str());
            return 1;
        }
        config_loaded = true;
    }

    std::string model_path = config_loaded ? app_config.model.path : "../model/yolov8.rknn";
    std::string mqtt_host = config_loaded ? app_config.mqtt.host : "127.0.0.1";
    int mqtt_port = config_loaded ? app_config.mqtt.port : 1883;
    std::string mqtt_topic = config_loaded ? app_config.mqtt.topic_alarm : "edge/detect";

    if (positional_args.size() > 0) {
        model_path = positional_args[0];
    }

    if (positional_args.size() > 1) {
        mqtt_host = positional_args[1];
    }

    if (positional_args.size() > 2) {
        mqtt_port = std::atoi(positional_args[2].c_str());
    }

    if (positional_args.size() > 3) {
        mqtt_topic = positional_args[3];
    }

    printf("========================================\n");
    printf(" RK3568 YOLOv8 Edge AI Pipeline\n");
    printf(" Temporary NV12 passthrough stage\n");
    printf("========================================\n");

    if (config_loaded) {
        printf("[main] config path : %s\n", config_path.c_str());
        print_app_config(app_config);
        printf("[main] note        : step 1 only loads config; capture/inference internals are not changed yet\n");
    } else {
        printf("[main] config path : <not used>\n");
    }

    printf("[main] model path  : %s\n", model_path.c_str());
    printf("[main] mqtt host   : %s\n", mqtt_host.c_str());
    printf("[main] mqtt port   : %d\n", mqtt_port);
    printf("[main] mqtt topic  : %s\n", mqtt_topic.c_str());
    printf("[main] camera      : /dev/video0\n");
    printf("[main] format      : %dx%d NV12\n", FRAME_WIDTH, FRAME_HEIGHT);
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
        model_path.c_str()
    );

    std::thread alm_thread(
        alarm_thread,
        std::ref(infer_queue),
        std::ref(running),
        mqtt_host.c_str(),
        mqtt_port,
        mqtt_topic.c_str()
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
