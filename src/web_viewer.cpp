#include "common.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <mosquitto.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <mutex>
#include <vector>

#include "opencv2/core/core.hpp"
#include "opencv2/imgcodecs.hpp"
#include "opencv2/imgproc.hpp"

/*
 * Web Viewer:
 *
 * 当前版本适配 NV12 共享内存。
 *
 * 功能：
 *   1. 打开 /edge_ai_frame 共享内存
 *   2. 从 SharedFrame::nv12 读取 640x480 NV12 图像
 *   3. OpenCV 转 BGR
 *   4. 编码 JPEG
 *   5. 通过 MJPEG 输出到浏览器
 *   6. 可选画框：/stream?boxes=1
 *   7. 通过 MQTT -> SSE 推送检测事件
 *
 * 当前阶段 inference.cpp 还没有跑 YOLOv8，所以 detect_count 通常为 0。
 */

// -------------------- write helper --------------------

static bool write_all(int fd, const void* data, size_t len) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    size_t left = len;

    while (left > 0) {
        ssize_t n = write(fd, p, left);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (n == 0) {
            return false;
        }

        p += n;
        left -= static_cast<size_t>(n);
    }

    return true;
}

// -------------------- SSE clients --------------------

static std::mutex sse_mtx;
static std::vector<int> sse_clients;

static void sse_broadcast(const char* data) {
    char buf[4096];

    int n = snprintf(buf, sizeof(buf), "data: %s\n\n", data);
    if (n <= 0) {
        return;
    }

    if (n > static_cast<int>(sizeof(buf))) {
        n = sizeof(buf);
    }

    std::lock_guard<std::mutex> lk(sse_mtx);

    for (int i = static_cast<int>(sse_clients.size()) - 1; i >= 0; --i) {
        if (!write_all(sse_clients[i], buf, static_cast<size_t>(n))) {
            close(sse_clients[i]);
            sse_clients.erase(sse_clients.begin() + i);
        }
    }
}

// -------------------- MQTT --------------------

static void on_message(struct mosquitto*, void*, const struct mosquitto_message* msg) {
    if (!msg || !msg->payload || msg->payloadlen <= 0) {
        return;
    }

    const int max_len = 4095;
    int len = msg->payloadlen;
    if (len > max_len) {
        len = max_len;
    }

    char payload[4096];
    memcpy(payload, msg->payload, static_cast<size_t>(len));
    payload[len] = '\0';

    sse_broadcast(payload);
}

static void* mqtt_thread(void* arg) {
    const char** args = static_cast<const char**>(arg);

    const char* host = args[0];
    const char* port_str = args[1];
    const char* topic = args[2];

    int port = atoi(port_str);

    mosquitto_lib_init();

    struct mosquitto* mosq = mosquitto_new(nullptr, true, nullptr);
    if (!mosq) {
        fprintf(stderr, "[web] mosquitto_new failed\n");
        mosquitto_lib_cleanup();
        return nullptr;
    }

    mosquitto_message_callback_set(mosq, on_message);

    int ret = mosquitto_connect(mosq, host, port, 60);
    if (ret == MOSQ_ERR_SUCCESS) {
        printf("[web] MQTT connected: %s:%d topic=%s\n", host, port, topic);
        mosquitto_subscribe(mosq, nullptr, topic, 0);
        mosquitto_loop_forever(mosq, -1, 1);
    } else {
        fprintf(stderr,
                "[web] MQTT connect failed: host=%s port=%d ret=%d\n",
                host,
                port,
                ret);
    }

    mosquitto_destroy(mosq);
    mosquitto_lib_cleanup();

    return nullptr;
}

// -------------------- HTML --------------------

static const char INDEX_HTML[] =
    "HTTP/1.0 200 OK\r\n"
    "Content-Type: text/html; charset=utf-8\r\n"
    "\r\n"
    "<!doctype html>\n"
    "<html>\n"
    "<head>\n"
    "  <meta charset='utf-8'>\n"
    "  <title>Edge AI Live Viewer</title>\n"
    "  <style>\n"
    "    body { font-family: Arial, sans-serif; background:#111; color:#eee; margin:0; padding:20px; }\n"
    "    h2 { margin-top:0; }\n"
    "    .panel { display:flex; gap:20px; align-items:flex-start; }\n"
    "    img { border:1px solid #444; max-width:100%; background:#000; }\n"
    "    button { padding:8px 14px; margin:4px; cursor:pointer; }\n"
    "    pre { background:#000; color:#0f0; padding:10px; width:420px; height:480px; overflow:auto; }\n"
    "    .status { margin:10px 0; color:#aaa; }\n"
    "  </style>\n"
    "</head>\n"
    "<body>\n"
    "  <h2>Edge AI Live Viewer</h2>\n"
    "  <div>\n"
    "    <button onclick='toggleBoxes()'>Toggle Boxes</button>\n"
    "    <span id='box_status'>Boxes: OFF</span>\n"
    "  </div>\n"
    "  <div class='status' id='status'>Waiting for stream...</div>\n"
    "  <div class='panel'>\n"
    "    <div>\n"
    "      <img id='stream' src='/stream'>\n"
    "    </div>\n"
    "    <div>\n"
    "      <h3>MQTT / Detection Events</h3>\n"
    "      <pre id='events'></pre>\n"
    "    </div>\n"
    "  </div>\n"
    "\n"
    "  <script>\n"
    "    let boxes = false;\n"
    "    let frameCount = 0;\n"
    "    let lastTime = performance.now();\n"
    "\n"
    "    function toggleBoxes() {\n"
    "      boxes = !boxes;\n"
    "      document.getElementById('box_status').textContent = boxes ? 'Boxes: ON' : 'Boxes: OFF';\n"
    "      document.getElementById('stream').src = boxes ? '/stream?boxes=1&t=' + Date.now() : '/stream?t=' + Date.now();\n"
    "    }\n"
    "\n"
    "    const img = document.getElementById('stream');\n"
    "    img.onload = function() {\n"
    "      frameCount++;\n"
    "      const now = performance.now();\n"
    "      if (now - lastTime >= 1000) {\n"
    "        document.getElementById('status').textContent = 'MJPEG stream active';\n"
    "        frameCount = 0;\n"
    "        lastTime = now;\n"
    "      }\n"
    "    };\n"
    "\n"
    "    const es = new EventSource('/events');\n"
    "    es.onmessage = function(e) {\n"
    "      const log = document.getElementById('events');\n"
    "      const now = new Date().toLocaleTimeString();\n"
    "      log.textContent = '[' + now + '] ' + e.data + '\\n' + log.textContent;\n"
    "    };\n"
    "  </script>\n"
    "</body>\n"
    "</html>\n";

// -------------------- MJPEG --------------------

struct MjpegArg {
    int fd;
    SharedFrame* shm;
    bool show_boxes;
};

static void serve_mjpeg(int fd, SharedFrame* shm, bool show_boxes) {
    const char* hdr =
        "HTTP/1.0 200 OK\r\n"
        "Content-Type: multipart/x-mixed-replace;boundary=frame\r\n"
        "Cache-Control: no-cache\r\n"
        "Pragma: no-cache\r\n"
        "\r\n";

    if (!write_all(fd, hdr, strlen(hdr))) {
        close(fd);
        return;
    }

    uint64_t last_seq = UINT64_MAX;

    std::vector<uint8_t> local_nv12(FRAME_NV12_SIZE);

    while (true) {
        uint64_t seq = shm->seq.load(std::memory_order_acquire);

        if (seq == last_seq) {
            usleep(10000);
            continue;
        }

        last_seq = seq;

        /*
         * 先复制到本地 buffer，再做 cvtColor。
         * 避免 inference 线程正在写共享内存时，web_viewer 同时读取导致花屏。
         */
        memcpy(local_nv12.data(), shm->nv12, FRAME_NV12_SIZE);

        cv::Mat nv12(SHM_HEIGHT * 3 / 2,
                     SHM_WIDTH,
                     CV_8UC1,
                     local_nv12.data());

        cv::Mat bgr;

        try {
            cv::cvtColor(nv12, bgr, cv::COLOR_YUV2BGR_NV12);
        } catch (const cv::Exception& e) {
            fprintf(stderr, "[web] cvtColor NV12 failed: %s\n", e.what());
            usleep(10000);
            continue;
        }

        if (show_boxes) {
            int cnt = shm->detect_count;
            if (cnt < 0) {
                cnt = 0;
            }
            if (cnt > 64) {
                cnt = 64;
            }

            for (int i = 0; i < cnt; ++i) {
                const SharedDetect& d = shm->detects[i];

                int left = d.left;
                int top = d.top;
                int right = d.right;
                int bottom = d.bottom;

                if (left < 0) left = 0;
                if (top < 0) top = 0;
                if (right > SHM_WIDTH) right = SHM_WIDTH;
                if (bottom > SHM_HEIGHT) bottom = SHM_HEIGHT;

                if (right <= left || bottom <= top) {
                    continue;
                }

                cv::rectangle(bgr,
                              cv::Point(left, top),
                              cv::Point(right, bottom),
                              cv::Scalar(0, 255, 0),
                              2);

                char label[64];
                snprintf(label,
                         sizeof(label),
                         "%s %.0f%%",
                         d.name,
                         d.prop * 100.0f);

                int text_y = top - 4;
                if (text_y < 12) {
                    text_y = top + 16;
                }

                cv::putText(bgr,
                            label,
                            cv::Point(left, text_y),
                            cv::FONT_HERSHEY_SIMPLEX,
                            0.5,
                            cv::Scalar(0, 255, 0),
                            1);
            }
        }

        std::vector<uint8_t> jpeg;
        std::vector<int> params;
        params.push_back(cv::IMWRITE_JPEG_QUALITY);
        params.push_back(70);

        bool ok = false;

        try {
            ok = cv::imencode(".jpg", bgr, jpeg, params);
        } catch (const cv::Exception& e) {
            fprintf(stderr, "[web] imencode failed: %s\n", e.what());
            ok = false;
        }

        if (!ok || jpeg.empty()) {
            usleep(10000);
            continue;
        }

        char part_hdr[256];
        int n = snprintf(part_hdr,
                         sizeof(part_hdr),
                         "--frame\r\n"
                         "Content-Type: image/jpeg\r\n"
                         "Content-Length: %zu\r\n"
                         "\r\n",
                         jpeg.size());

        if (n <= 0) {
            break;
        }

        if (!write_all(fd, part_hdr, static_cast<size_t>(n))) {
            break;
        }

        if (!write_all(fd, jpeg.data(), jpeg.size())) {
            break;
        }

        if (!write_all(fd, "\r\n", 2)) {
            break;
        }
    }

    close(fd);
}

static void* mjpeg_thread(void* arg) {
    MjpegArg* a = static_cast<MjpegArg*>(arg);

    serve_mjpeg(a->fd, a->shm, a->show_boxes);

    delete a;
    return nullptr;
}

// -------------------- HTTP dispatch --------------------

static void handle_client(int fd, SharedFrame* shm) {
    char buf[1024];
    memset(buf, 0, sizeof(buf));

    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    if (n <= 0) {
        close(fd);
        return;
    }

    if (strncmp(buf, "GET /stream", 11) == 0) {
        bool boxes = strstr(buf, "boxes=1") != nullptr;

        pthread_t t;
        MjpegArg* a = new MjpegArg;
        a->fd = fd;
        a->shm = shm;
        a->show_boxes = boxes;

        if (pthread_create(&t, nullptr, mjpeg_thread, a) != 0) {
            fprintf(stderr, "[web] pthread_create mjpeg failed\n");
            delete a;
            close(fd);
            return;
        }

        pthread_detach(t);
        return;
    }

    if (strncmp(buf, "GET /events", 11) == 0) {
        const char* hdr =
            "HTTP/1.0 200 OK\r\n"
            "Content-Type: text/event-stream\r\n"
            "Cache-Control: no-cache\r\n"
            "Connection: keep-alive\r\n"
            "\r\n";

        if (!write_all(fd, hdr, strlen(hdr))) {
            close(fd);
            return;
        }

        std::lock_guard<std::mutex> lk(sse_mtx);
        sse_clients.push_back(fd);
        return;
    }

    write_all(fd, INDEX_HTML, strlen(INDEX_HTML));
    close(fd);
}

// -------------------- main --------------------

int main(int argc, char** argv) {
    const char* mqtt_host = argc > 1 ? argv[1] : "127.0.0.1";
    const char* mqtt_port = argc > 2 ? argv[2] : "1883";
    const char* mqtt_topic = argc > 3 ? argv[3] : "edge/detect";
    int http_port = argc > 4 ? atoi(argv[4]) : 8080;

    int shm_fd = shm_open(SHM_NAME, O_RDONLY, 0666);
    if (shm_fd < 0) {
        perror("[web] shm_open");
        fprintf(stderr,
                "[web] make sure edge_ai_pipeline is running before web_viewer\n");
        return 1;
    }

    SharedFrame* shm = static_cast<SharedFrame*>(
        mmap(nullptr,
             sizeof(SharedFrame),
             PROT_READ,
             MAP_SHARED,
             shm_fd,
             0)
    );

    if (shm == MAP_FAILED) {
        perror("[web] mmap");
        close(shm_fd);
        return 1;
    }

    const char* mqtt_args[] = {
        mqtt_host,
        mqtt_port,
        mqtt_topic
    };

    pthread_t mt;
    if (pthread_create(&mt, nullptr, mqtt_thread, (void*)mqtt_args) == 0) {
        pthread_detach(mt);
    } else {
        fprintf(stderr, "[web] MQTT thread create failed, continue without MQTT\n");
    }

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) {
        perror("[web] socket");
        munmap(shm, sizeof(SharedFrame));
        close(shm_fd);
        return 1;
    }

    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));

    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(static_cast<uint16_t>(http_port));

    if (bind(srv, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        perror("[web] bind");
        close(srv);
        munmap(shm, sizeof(SharedFrame));
        close(shm_fd);
        return 1;
    }

    if (listen(srv, 8) < 0) {
        perror("[web] listen");
        close(srv);
        munmap(shm, sizeof(SharedFrame));
        close(shm_fd);
        return 1;
    }

    printf("[web] viewer started\n");
    printf("[web] HTTP: http://0.0.0.0:%d\n", http_port);
    printf("[web] MQTT: %s:%s topic=%s\n", mqtt_host, mqtt_port, mqtt_topic);
    printf("[web] shared memory: %s\n", SHM_NAME);
    printf("[web] expected frame: %dx%d NV12\n", SHM_WIDTH, SHM_HEIGHT);

    while (true) {
        int fd = accept(srv, nullptr, nullptr);
        if (fd < 0) {
            if (errno == EINTR) {
                continue;
            }

            perror("[web] accept");
            continue;
        }

        handle_client(fd, shm);
    }

    close(srv);
    munmap(shm, sizeof(SharedFrame));
    close(shm_fd);

    return 0;
}
