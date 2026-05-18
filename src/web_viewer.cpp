#include "common.h"
#include "config.h"

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
#include <string>
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
    const char* detect_topic = args[2];
    const char* alarm_topic = args[3];

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
        printf("[web] MQTT connected: %s:%d detect_topic=%s alarm_topic=%s\n",
               host,
               port,
               detect_topic,
               alarm_topic);

        mosquitto_subscribe(mosq, nullptr, detect_topic, 0);

        if (strcmp(detect_topic, alarm_topic) != 0) {
            mosquitto_subscribe(mosq, nullptr, alarm_topic, 0);
        }

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
    "  <title>Edge Person Monitoring System</title>\n"
    "  <style>\n"
    "    body { font-family: Arial, sans-serif; background:#111; color:#eee; margin:0; padding:20px; }\n"
    "    h2 { margin-top:0; }\n"
    "    .toolbar { margin:8px 0 14px 0; }\n"
    "    .panel { display:flex; gap:20px; align-items:flex-start; }\n"
    "    .video-panel { flex: 0 0 auto; }\n"
    "    .side-panel { width:460px; }\n"
    "    img { border:1px solid #444; max-width:100%; background:#000; }\n"
    "    button { padding:8px 14px; margin:4px; cursor:pointer; }\n"
    "    .status { margin:10px 0; color:#aaa; }\n"
    "    .card { background:#1b1b1b; border:1px solid #333; padding:10px; margin-bottom:12px; }\n"
    "    .card h3 { margin:0 0 8px 0; }\n"
    "    .alarm-list { height:210px; overflow:auto; }\n"
    "    .alarm-item { border-left:4px solid #f6c343; background:#222; padding:8px; margin:8px 0; font-size:13px; }\n"
    "    .alarm-title { color:#f6c343; font-weight:bold; }\n"
    "    .muted { color:#999; font-size:12px; }\n"
    "    .snapshot { width:100%; max-height:220px; object-fit:contain; }\n"
    "    pre { background:#000; color:#0f0; padding:10px; height:170px; overflow:auto; font-size:12px; white-space:pre-wrap; }\n"
    "    a { color:#8cc8ff; }\n"
    "  </style>\n"
    "</head>\n"
    "<body>\n"
    "  <h2>Edge Person Monitoring System</h2>\n"
    "  <div class='toolbar'>\n"
    "    <button onclick='toggleBoxes()'>Toggle Boxes</button>\n"
    "    <span id='box_status'>Boxes: OFF</span><span style='margin-left:12px;color:#aaa'>ROI overlay: ON</span>\n"
    "  </div>\n"
    "  <div class='status' id='status'>Waiting for stream...</div>\n"
    "  <div class='panel'>\n"
    "    <div class='video-panel'>\n"
    "      <img id='stream' src='/stream'>\n"
    "    </div>\n"
    "    <div class='side-panel'>\n"
    "      <div class='card'>\n"
    "        <h3>Alarm Events</h3>\n"
    "        <div id='alarms' class='alarm-list'><div class='muted'>No alarm yet.</div></div>\n"
    "      </div>\n"
    "      <div class='card'>\n"
    "        <h3>Latest Snapshot</h3>\n"
    "        <div id='snapshot_empty' class='muted'>No snapshot yet.</div>\n"
    "        <a id='snapshot_link' href='#' target='_blank' style='display:none'>Open snapshot</a>\n"
    "        <div style='margin-top:8px'>\n"
    "          <img id='snapshot_img' class='snapshot' style='display:none'>\n"
    "        </div>\n"
    "      </div>\n"
    "      <div class='card'>\n"
    "        <h3>Raw MQTT Events</h3>\n"
    "        <pre id='events'></pre>\n"
    "      </div>\n"
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
    "    function snapshotUrl(path) {\n"
    "      if (!path) return '';\n"
    "      const file = path.split('/').pop();\n"
    "      if (!file) return '';\n"
    "      return '/snapshot/' + encodeURIComponent(file);\n"
    "    }\n"
    "\n"
    "    function addAlarm(obj) {\n"
    "      const alarms = document.getElementById('alarms');\n"
    "      const empty = alarms.querySelector('.muted');\n"
    "      if (empty) empty.remove();\n"
    "\n"
    "      const item = document.createElement('div');\n"
    "      item.className = 'alarm-item';\n"
    "\n"
    "      const eventId = obj.event_id || '-';\n"
    "      const level = obj.level || 'warning';\n"
    "      const cls = obj.object && obj.object.class ? obj.object.class : '-';\n"
    "      const conf = obj.object && obj.object.confidence !== undefined ? Number(obj.object.confidence).toFixed(3) : '-';\n"
    "      const dwell = obj.rule && obj.rule.dwell_time_ms !== undefined ? obj.rule.dwell_time_ms : '-';\n"
    "      const snap = snapshotUrl(obj.snapshot || '');\n"
    "\n"
    "      item.innerHTML = '' +\n"
    "        '<div class=\"alarm-title\">' + level + ' / ' + (obj.event_type || 'person_intrusion') + '</div>' +\n"
    "        '<div>event_id: ' + eventId + '</div>' +\n"
    "        '<div>object: ' + cls + ' conf=' + conf + '</div>' +\n"
    "        '<div>dwell: ' + dwell + ' ms</div>' +\n"
    "        (snap ? '<div><a href=\"' + snap + '\" target=\"_blank\">snapshot</a></div>' : '');\n"
    "\n"
    "      alarms.prepend(item);\n"
    "\n"
    "      while (alarms.children.length > 10) {\n"
    "        alarms.removeChild(alarms.lastChild);\n"
    "      }\n"
    "\n"
    "      if (snap) {\n"
    "        const img = document.getElementById('snapshot_img');\n"
    "        const link = document.getElementById('snapshot_link');\n"
    "        document.getElementById('snapshot_empty').style.display = 'none';\n"
    "        img.src = snap + '?t=' + Date.now();\n"
    "        img.style.display = 'block';\n"
    "        link.href = snap;\n"
    "        link.style.display = 'inline';\n"
    "      }\n"
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
    "      try {\n"
    "        const obj = JSON.parse(e.data);\n"
    "        if (obj.event_type === 'person_intrusion') {\n"
    "          addAlarm(obj);\n"
    "        }\n"
    "      } catch (err) {\n"
    "      }\n"
    "    };\n"
    "  </script>\n"
    "</body>\n"
    "</html>\n";


// -------------------- ROI drawing --------------------

static void draw_roi_overlay(cv::Mat& bgr, const std::vector<RoiPoint>& roi) {
    if (roi.size() < 3) {
        return;
    }

    std::vector<cv::Point> points;
    points.reserve(roi.size());

    for (const auto& p : roi) {
        points.emplace_back(p.x, p.y);
    }

    const cv::Scalar roi_color(0, 255, 255);

    cv::polylines(
        bgr,
        points,
        true,
        roi_color,
        2,
        cv::LINE_AA
    );

    for (size_t i = 0; i < points.size(); ++i) {
        cv::circle(
            bgr,
            points[i],
            4,
            roi_color,
            -1,
            cv::LINE_AA
        );

        char label[16];
        snprintf(label, sizeof(label), "P%zu", i + 1);

        cv::putText(
            bgr,
            label,
            cv::Point(points[i].x + 5, points[i].y - 5),
            cv::FONT_HERSHEY_SIMPLEX,
            0.45,
            roi_color,
            1,
            cv::LINE_AA
        );
    }

    cv::putText(
        bgr,
        "ROI",
        cv::Point(points[0].x, points[0].y - 18),
        cv::FONT_HERSHEY_SIMPLEX,
        0.65,
        roi_color,
        2,
        cv::LINE_AA
    );
}

// -------------------- MJPEG --------------------

struct MjpegArg {
    int fd;
    SharedFrame* shm;
    bool show_boxes;
    bool show_roi;
    std::vector<RoiPoint> roi;
};

static void serve_mjpeg(
    int fd,
    SharedFrame* shm,
    bool show_boxes,
    bool show_roi,
    const std::vector<RoiPoint>& roi
) {
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

        if (show_roi) {
            draw_roi_overlay(bgr, roi);
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

    serve_mjpeg(a->fd, a->shm, a->show_boxes, a->show_roi, a->roi);

    delete a;
    return nullptr;
}


// -------------------- Snapshot file serving --------------------

static bool has_bad_path_char(const std::string& filename) {
    return filename.empty() ||
           filename.find("..") != std::string::npos ||
           filename.find('/') != std::string::npos ||
           filename.find('\\') != std::string::npos;
}

static void send_http_error(int fd, int code, const char* text) {
    char hdr[256];
    int n = snprintf(
        hdr,
        sizeof(hdr),
        "HTTP/1.0 %d %s\r\n"
        "Content-Type: text/plain; charset=utf-8\r\n"
        "Cache-Control: no-cache\r\n"
        "\r\n"
        "%s\n",
        code,
        text,
        text
    );

    if (n > 0) {
        write_all(fd, hdr, static_cast<size_t>(n));
    }

    close(fd);
}

static void serve_snapshot(int fd, const char* request) {
    const char* prefix = "GET /snapshot/";
    const size_t prefix_len = strlen(prefix);

    const char* start = request + prefix_len;
    const char* end = strchr(start, ' ');
    if (!end || end <= start) {
        send_http_error(fd, 400, "Bad Request");
        return;
    }

    std::string filename(start, static_cast<size_t>(end - start));

    if (has_bad_path_char(filename)) {
        send_http_error(fd, 400, "Bad Request");
        return;
    }

    std::string file_path = std::string("../events/") + filename;

    int img_fd = open(file_path.c_str(), O_RDONLY);
    if (img_fd < 0) {
        send_http_error(fd, 404, "Not Found");
        return;
    }

    struct stat st {};
    if (fstat(img_fd, &st) != 0 || st.st_size <= 0) {
        close(img_fd);
        send_http_error(fd, 404, "Not Found");
        return;
    }

    const char* content_type = "image/jpeg";
    if (filename.size() >= 4) {
        const std::string suffix = filename.substr(filename.size() - 4);
        if (suffix == ".png" || suffix == ".PNG") {
            content_type = "image/png";
        }
    }

    char hdr[256];
    int n = snprintf(
        hdr,
        sizeof(hdr),
        "HTTP/1.0 200 OK\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %lld\r\n"
        "Cache-Control: no-cache\r\n"
        "\r\n",
        content_type,
        static_cast<long long>(st.st_size)
    );

    if (n <= 0 || !write_all(fd, hdr, static_cast<size_t>(n))) {
        close(img_fd);
        close(fd);
        return;
    }

    char buf[8192];
    while (true) {
        ssize_t r = read(img_fd, buf, sizeof(buf));
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (r == 0) {
            break;
        }
        if (!write_all(fd, buf, static_cast<size_t>(r))) {
            break;
        }
    }

    close(img_fd);
    close(fd);
}

// -------------------- HTTP dispatch --------------------

static void handle_client(
    int fd,
    SharedFrame* shm,
    const std::vector<RoiPoint>& roi
) {
    char buf[1024];
    memset(buf, 0, sizeof(buf));

    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    if (n <= 0) {
        close(fd);
        return;
    }

    if (strncmp(buf, "GET /snapshot/", 14) == 0) {
        serve_snapshot(fd, buf);
        return;
    }

    if (strncmp(buf, "GET /stream", 11) == 0) {
        bool boxes = strstr(buf, "boxes=1") != nullptr;

        pthread_t t;
        MjpegArg* a = new MjpegArg;
        a->fd = fd;
        a->shm = shm;
        a->show_boxes = boxes;
        a->show_roi = !roi.empty();
        a->roi = roi;

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
    const char* mqtt_detect_topic = argc > 3 ? argv[3] : "edge/detect";
    int http_port = argc > 4 ? atoi(argv[4]) : 8080;
    const char* mqtt_alarm_topic = argc > 5 ? argv[5] : "edge/person/alarm";
    const char* config_path = argc > 6 ? argv[6] : "../config/config.json";

    AppConfig app_config;
    std::string config_error;

    if (load_app_config(config_path, app_config, config_error)) {
        printf("[web] config loaded: %s roi_points=%zu\n",
               config_path,
               app_config.rule.roi.size());
    } else {
        fprintf(stderr,
                "[web] config load failed: %s, error=%s\n",
                config_path,
                config_error.c_str());
        fprintf(stderr,
                "[web] continue without ROI overlay\n");
    }

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
        mqtt_detect_topic,
        mqtt_alarm_topic
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
    printf("[web] MQTT: %s:%s detect_topic=%s alarm_topic=%s\n",
           mqtt_host,
           mqtt_port,
           mqtt_detect_topic,
           mqtt_alarm_topic);
    printf("[web] shared memory: %s\n", SHM_NAME);
    printf("[web] expected frame: %dx%d NV12\n", SHM_WIDTH, SHM_HEIGHT);
    printf("[web] ROI overlay: %s points=%zu\n",
           app_config.rule.roi.empty() ? "OFF" : "ON",
           app_config.rule.roi.size());

    while (true) {
        int fd = accept(srv, nullptr, nullptr);
        if (fd < 0) {
            if (errno == EINTR) {
                continue;
            }

            perror("[web] accept");
            continue;
        }

        handle_client(fd, shm, app_config.rule.roi);
    }

    close(srv);
    munmap(shm, sizeof(SharedFrame));
    close(shm_fd);

    return 0;
}
