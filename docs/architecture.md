# RK3568 EdgeGuard 系统架构文档

> 本文档用于说明 RK3568 + IMX415 + YOLOv8 RKNN 人员监测系统的整体架构、线程模型、数据流、共享内存、MQTT、Web Viewer、规则引擎、截图留证和 systemd 部署关系。  
> 当前项目路径默认如下：

```bash
/userdata/RK3568_Yolo
```

---

## 1. 系统定位

本项目从基础 YOLOv8 RKNN Demo 升级为边缘端人员监测与告警系统。

系统目标：

```text
1. 在 RK3568 开发板上实时采集 IMX415 摄像头画面；
2. 使用 RKNN Runtime 调用 NPU 执行 YOLOv8 推理；
3. 检测 person 目标；
4. 根据 ROI、停留时间和冷却时间判断是否触发告警；
5. 通过 MQTT 发布原始检测结果、结构化告警事件和运行指标；
6. 告警时保存截图留证；
7. Web 页面展示视频流、检测框、ROI、告警列表、最近截图和系统指标；
8. 使用 systemd 实现服务化部署和开机自启动。
```

---

## 2. 整体架构图

```text
┌──────────────────────────────────────────────────────────────────────┐
│                           RK3568 开发板                               │
│                                                                      │
│  ┌──────────────┐      ┌────────────────┐      ┌─────────────────┐   │
│  │ IMX415 Camera│ ───▶ │ capture_thread │ ───▶ │ RawFrame Queue   │   │
│  └──────────────┘      └────────────────┘      └─────────────────┘   │
│                                                        │             │
│                                                        ▼             │
│                         ┌────────────────────────────────────────┐   │
│                         │ inference_thread                       │   │
│                         │ - NV12 -> RGB888                       │   │
│                         │ - YOLOv8 RKNN inference                │   │
│                         │ - postprocess                          │   │
│                         │ - write SharedFrame                    │   │
│                         │ - push InferResult                     │   │
│                         └────────────────────────────────────────┘   │
│                              │                         │             │
│                              │                         │             │
│                              ▼                         ▼             │
│                  ┌────────────────────┐      ┌───────────────────┐   │
│                  │ /dev/shm/edge_ai...│      │ InferResult Queue  │   │
│                  │ SharedFrame         │      └───────────────────┘   │
│                  └────────────────────┘                │             │
│                              ▲                         ▼             │
│                              │              ┌────────────────────┐   │
│                              │              │ alarm_thread        │   │
│                              │              │ - RuleEngine        │   │
│                              │              │ - MQTT publish      │   │
│                              │              │ - Snapshot save     │   │
│                              │              │ - Metrics publish   │   │
│                              │              └────────────────────┘   │
│                              │                         │             │
│                              │                         ▼             │
│                              │              ┌────────────────────┐   │
│                              │              │ Mosquitto MQTT      │   │
│                              │              │ 127.0.0.1:1883      │   │
│                              │              └────────────────────┘   │
│                              │                         ▲             │
│                              │                         │             │
│                     ┌────────────────────────────────────────────┐    │
│                     │ web_viewer                                 │    │
│                     │ - read SharedFrame from shared memory       │    │
│                     │ - MJPEG stream                             │    │
│                     │ - draw bbox / ROI                          │    │
│                     │ - subscribe MQTT topics                    │    │
│                     │ - SSE push to browser                      │    │
│                     │ - serve snapshot files                     │    │
│                     └────────────────────────────────────────────┘    │
│                                      │                               │
└──────────────────────────────────────┼───────────────────────────────┘
                                       ▼
                         ┌────────────────────────┐
                         │ Browser Web UI          │
                         │ http://<board-ip>:8080  │
                         └────────────────────────┘
```

---

## 3. 进程划分

系统运行时主要有两个用户态进程。

### 3.1 edge_ai_pipeline

主检测与告警进程。

职责：

```text
1. 摄像头采集；
2. YOLOv8 RKNN 推理；
3. 检测结果写入共享内存；
4. 推理结果进入告警队列；
5. ROI 规则判断；
6. MQTT 发布；
7. 告警截图；
8. Runtime Metrics 统计。
```

启动方式：

```bash
cd /userdata/RK3568_Yolo/install
export LD_LIBRARY_PATH=$PWD/lib:$LD_LIBRARY_PATH
./edge_ai_pipeline --config ../config/config.json
```

systemd 服务：

```text
edge-person-monitor.service
```

---

### 3.2 web_viewer

Web 可视化进程。

职责：

```text
1. 读取 /dev/shm/edge_ai_frame 共享内存；
2. 将 NV12 帧转换为 BGR；
3. 绘制 ROI；
4. 根据请求绘制检测框；
5. 编码 MJPEG 输出到浏览器；
6. 订阅 MQTT topic；
7. 将 MQTT 消息通过 SSE 推送给浏览器；
8. 提供 /snapshot/<file>.jpg 静态截图访问。
```

启动方式：

```bash
cd /userdata/RK3568_Yolo/install
export LD_LIBRARY_PATH=$PWD/lib:$LD_LIBRARY_PATH

./web_viewer   127.0.0.1   1883   edge/detect   8080   edge/person/alarm   edge/person/metrics   ../config/config.json
```

systemd 服务：

```text
edge-person-web.service
```

---

## 4. 线程模型

### 4.1 edge_ai_pipeline 线程

`edge_ai_pipeline` 内部主要有 3 个工作线程。

```text
main thread
├── capture_thread
├── inference_thread
└── alarm_thread
```

### 4.2 capture_thread

职责：

```text
1. 打开 /dev/video0；
2. 配置摄像头格式和分辨率；
3. 采集 640x480 NV12 帧；
4. 将 RawFrame 推入 capture_queue。
```

输入：

```text
IMX415 摄像头
```

输出：

```text
RingQueue<RawFrame, 4>
```

---

### 4.3 inference_thread

职责：

```text
1. 从 capture_queue 读取 RawFrame；
2. 将 NV12 写入共享内存 SharedFrame；
3. 使用 OpenCV 将 NV12 转 RGB888；
4. 调用 inference_yolov8_model()；
5. 执行 YOLOv8 后处理；
6. 将检测结果写入 SharedFrame.detects；
7. 将 InferResult 推入 infer_queue；
8. 更新 inference metrics。
```

输入：

```text
RingQueue<RawFrame, 4>
```

输出：

```text
1. /dev/shm/edge_ai_frame
2. RingQueue<InferResult, 4>
```

---

### 4.4 alarm_thread

职责：

```text
1. 从 infer_queue 读取 InferResult；
2. 发布原始检测结果 edge/detect；
3. 调用 RuleEngine 判断是否触发告警；
4. 触发告警时生成 event_id；
5. 保存告警截图；
6. 发布结构化告警 edge/person/alarm；
7. 周期发布运行指标 edge/person/metrics。
```

输入：

```text
RingQueue<InferResult, 4>
```

输出：

```text
1. MQTT edge/detect
2. MQTT edge/person/alarm
3. MQTT edge/person/metrics
4. events/<event_id>.jpg
```

---

## 5. 数据结构

### 5.1 RawFrame

表示摄像头采集的一帧原始 NV12 图像。

```cpp
struct RawFrame {
    uint8_t data[FRAME_NV12_SIZE];
    int width = FRAME_WIDTH;
    int height = FRAME_HEIGHT;
    uint64_t seq = 0;
};
```

用途：

```text
capture_thread → inference_thread
```

---

### 5.2 DetectResult

表示单个检测目标。

```cpp
struct DetectResult {
    char name[16];
    float prop;
    int left, top, right, bottom;
};
```

字段说明：

| 字段     | 说明                    |
| -------- | ----------------------- |
| `name`   | 类别名称，例如 `person` |
| `prop`   | 置信度                  |
| `left`   | bbox 左坐标             |
| `top`    | bbox 上坐标             |
| `right`  | bbox 右坐标             |
| `bottom` | bbox 下坐标             |

---

### 5.3 InferResult

表示一帧推理结果。

```cpp
struct InferResult {
    DetectResult results[64];
    int count = 0;
    uint64_t seq = 0;
};
```

用途：

```text
inference_thread → alarm_thread
```

---

### 5.4 SharedFrame

共享内存中保存的帧数据结构。

```cpp
struct SharedFrame {
    std::atomic<uint64_t> seq{0};
    uint8_t nv12[FRAME_NV12_SIZE];
    SharedDetect detects[64];
    int detect_count{0};
};
```

用途：

```text
edge_ai_pipeline 写入
web_viewer 读取
```

---

## 6. 共享内存机制

### 6.1 共享内存名称

```text
/edge_ai_frame
```

Linux 实际路径：

```bash
/dev/shm/edge_ai_frame
```

### 6.2 创建方

共享内存由 `inference_thread` 创建。

逻辑：

```text
1. shm_unlink(SHM_NAME)
2. shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666)
3. ftruncate(fd, sizeof(SharedFrame))
4. mmap()
5. placement new 初始化 SharedFrame
```

### 6.3 读取方

`web_viewer` 只读打开共享内存：

```text
shm_open(SHM_NAME, O_RDONLY, 0666)
mmap(PROT_READ)
```

### 6.4 共享内存启动依赖

`web_viewer` 必须在 `edge_ai_pipeline` 创建共享内存后启动。

否则会出现：

```text
[web] shm_open: No such file or directory
```

当前已在 `scripts/run_web.sh` 中加入等待逻辑：

```text
等待 /dev/shm/edge_ai_frame 出现后再启动 web_viewer。
```

---

## 7. 数据流说明

### 7.1 视频与检测数据流

```text
IMX415 Camera
  ↓
capture_thread
  ↓ RawFrame
capture_queue
  ↓
inference_thread
  ├── 写入 SharedFrame.nv12
  ├── 写入 SharedFrame.detects
  └── 推送 InferResult
        ↓
      infer_queue
        ↓
      alarm_thread
```

### 7.2 Web 视频数据流

```text
SharedFrame.nv12
  ↓
web_viewer mmap 读取
  ↓
OpenCV NV12 -> BGR
  ↓
绘制 ROI
  ↓
可选绘制 bbox
  ↓
JPEG 编码
  ↓
MJPEG stream
  ↓
Browser
```

浏览器视频接口：

```text
/stream
/stream?boxes=1
```

### 7.3 告警数据流

```text
InferResult
  ↓
RuleEngine
  ↓
满足规则
  ↓
生成 event_id
  ↓
保存截图
  ↓
发布 MQTT edge/person/alarm
  ↓
web_viewer 订阅
  ↓
SSE /events
  ↓
Browser Alarm Events
```

### 7.4 Metrics 数据流

```text
metrics.cpp 全局原子计数
  ↓
alarm_thread 每 5 秒读取快照
  ↓
发布 MQTT edge/person/metrics
  ↓
web_viewer 订阅
  ↓
SSE /events
  ↓
Browser System Metrics
```

---

## 8. RuleEngine 规则引擎

### 8.1 规则配置

配置文件：

```bash
config/config.json
```

核心字段：

```json
"rule": {
  "target_class": "person",
  "roi": [[120, 80], [540, 80], [580, 420], [90, 420]],
  "dwell_time_ms": 1000,
  "cooldown_ms": 5000
}
```

### 8.2 判断流程

```text
输入 DetectResult
  ↓
判断 class 是否等于 target_class
  ↓
判断 confidence 是否 >= conf_threshold
  ↓
计算 bbox center
  ↓
判断 center 是否在 ROI 多边形内
  ↓
判断停留时间是否超过 dwell_time_ms
  ↓
判断是否处于 cooldown_ms 冷却期
  ↓
输出 RuleResult
```

### 8.3 告警条件

只有同时满足以下条件才会触发告警：

```text
1. class == person
2. confidence >= conf_threshold
3. bbox center inside ROI
4. dwell_time_ms reached
5. cooldown_ms passed
```

### 8.4 ROI 判断方式

当前使用点在多边形内判断：

```text
point-in-polygon
```

判断点为：

```text
bbox center = ((left + right) / 2, (top + bottom) / 2)
```

---

## 9. MQTT 架构

### 9.1 Topic 划分

| Topic                 | 发布方           | 订阅方                | 用途         |
| --------------------- | ---------------- | --------------------- | ------------ |
| `edge/detect`         | edge_ai_pipeline | web_viewer / 调试端   | 原始检测结果 |
| `edge/person/alarm`   | edge_ai_pipeline | web_viewer / 告警平台 | 结构化告警   |
| `edge/person/metrics` | edge_ai_pipeline | web_viewer / 监控平台 | 运行指标     |

### 9.2 原始检测结果

```text
edge/detect
```

示例：

```json
{
  "seq": 123,
  "objects": [
    {
      "class": "person",
      "conf": 0.873,
      "box": [178, 92, 321, 431]
    }
  ]
}
```

### 9.3 告警事件

```text
edge/person/alarm
```

示例：

```json
{
  "event_id": "20260518-200221-0001",
  "event_type": "person_intrusion",
  "camera_id": "rk3568-imx415-01",
  "timestamp": 1779110640405,
  "level": "warning",
  "object": {
    "class": "person",
    "confidence": 0.873,
    "bbox": [178, 92, 321, 431]
  },
  "rule": {
    "roi_name": "restricted_area_1",
    "dwell_time_ms": 1100
  },
  "snapshot": "../events/20260518-200221-0001.jpg"
}
```

### 9.4 运行指标

```text
edge/person/metrics
```

示例：

```json
{
  "uptime_sec": 25,
  "inference_fps": 19.57,
  "inference_frame_count": 486,
  "alarm_count": 3,
  "mqtt_detect_publish_count": 486,
  "mqtt_alarm_publish_count": 3,
  "last_alarm_ts": 1779110646471
}
```

---

## 10. Web Viewer 架构

### 10.1 HTTP 路由

| 路由                   | 说明                     |
| ---------------------- | ------------------------ |
| `/`                    | Web 首页                 |
| `/stream`              | MJPEG 视频流             |
| `/stream?boxes=1`      | MJPEG 视频流，叠加检测框 |
| `/events`              | SSE 事件流               |
| `/snapshot/<file>.jpg` | 告警截图访问             |

### 10.2 页面模块

Web 页面包括：

```text
1. 视频窗口；
2. Toggle Boxes 按钮；
3. ROI overlay；
4. Alarm Events；
5. Latest Snapshot；
6. System Metrics；
7. Raw MQTT Events。
```

### 10.3 SSE 消息流

`web_viewer` 订阅 MQTT 后，将 payload 通过 SSE 推送给浏览器：

```text
MQTT message
  ↓
on_message()
  ↓
sse_broadcast()
  ↓
/events
  ↓
Browser EventSource
```

浏览器端根据 JSON 内容判断消息类型：

```text
event_type == person_intrusion → Alarm Events
存在 uptime_sec / inference_fps → System Metrics
其他 JSON → Raw MQTT Events
```

---

## 11. 截图留证架构

### 11.1 触发时机

告警触发时，`alarm_thread` 调用截图模块。

```text
RuleResult.alarm == true
```

### 11.2 截图流程

```text
打开共享内存 /edge_ai_frame
  ↓
复制当前 NV12 帧
  ↓
OpenCV NV12 -> BGR
  ↓
绘制 ROI
  ↓
绘制 bbox
  ↓
绘制 event_id / timestamp / confidence
  ↓
保存 JPEG
```

### 11.3 保存路径

配置文件中：

```json
"snapshot": {
  "enable": true,
  "save_dir": "../events"
}
```

实际路径：

```bash
/userdata/RK3568_Yolo/events
```

文件名：

```text
<event_id>.jpg
```

示例：

```text
20260518-200221-0001.jpg
```

### 11.4 Web 访问

```text
http://<board-ip>:8080/snapshot/<event_id>.jpg
```

---

## 12. Metrics 架构

### 12.1 Metrics 模块

Metrics 模块使用全局原子变量统计运行状态。

当前统计：

```text
inference_frame_count
alarm_count
mqtt_detect_publish_count
mqtt_alarm_publish_count
last_alarm_ts_ms
uptime_sec
```

### 12.2 数据记录点

| 指标                        | 记录位置                             |
| --------------------------- | ------------------------------------ |
| `inference_frame_count`     | inference_thread 推送 InferResult 后 |
| `alarm_count`               | alarm_thread 触发告警时              |
| `mqtt_detect_publish_count` | publish_raw_detection 后             |
| `mqtt_alarm_publish_count`  | publish_alarm_event 成功后           |
| `last_alarm_ts_ms`          | 触发告警时                           |
| `uptime_sec`                | metrics_init 后按 steady_clock 计算  |

### 12.3 发布周期

```text
5 秒
```

发布方：

```text
alarm_thread
```

Topic：

```text
edge/person/metrics
```

---

## 13. systemd 部署架构

### 13.1 服务列表

| 服务                          | 程序               | 说明             |
| ----------------------------- | ------------------ | ---------------- |
| `edge-person-monitor.service` | `edge_ai_pipeline` | 主检测与告警服务 |
| `edge-person-web.service`     | `web_viewer`       | Web 可视化服务   |
| `mosquitto.service`           | `mosquitto`        | MQTT Broker      |

### 13.2 启动依赖

```text
edge-person-web.service
  Requires=edge-person-monitor.service
  After=edge-person-monitor.service
```

Web 服务还依赖共享内存：

```text
/dev/shm/edge_ai_frame
```

因此 `run_web.sh` 增加等待逻辑。

### 13.3 推荐启动顺序

```text
1. mosquitto.service
2. edge-person-monitor.service
3. edge-person-web.service
```

### 13.4 服务状态检查

```bash
systemctl status edge-person-monitor --no-pager
systemctl status edge-person-web --no-pager
```

预期：

```text
active (running)
```

---

## 14. 配置文件关系

主配置文件：

```bash
config/config.json
```

被以下模块使用：

| 模块               | 使用字段                                             |
| ------------------ | ---------------------------------------------------- |
| `edge_ai_pipeline` | model、rule、mqtt、snapshot                          |
| `web_viewer`       | rule.roi                                             |
| `RuleEngine`       | target_class、roi、dwell_time_ms、cooldown_ms        |
| `Snapshot`         | enable、save_dir                                     |
| `MQTT`             | host、port、topic_detect、topic_alarm、topic_metrics |

---

## 15. 当前系统能力边界

当前系统已经支持：

```text
1. 单路摄像头；
2. 单个 ROI；
3. 单一目标类别 person；
4. 单机本地 Mosquitto；
5. Web MJPEG 视频；
6. 本地 JPEG 截图；
7. systemd 托管运行；
8. 基础运行指标。
```

当前尚未支持：

```text
1. 多摄像头；
2. 多 ROI；
3. 不同 ROI 不同规则；
4. 远端对象存储；
5. HTTPS；
6. 用户登录认证；
7. 告警数据库；
8. Web 历史事件查询；
9. CPU / 内存 / 温度指标；
10. RGA 硬件预处理优化。
```

---

## 16. 后续扩展方向

### 16.1 多 ROI 支持

当前配置：

```json
"roi": [[120, 80], [540, 80], [580, 420], [90, 420]]
```

后续可扩展为：

```json
"rois": [
  {
    "name": "restricted_area_1",
    "points": [[120, 80], [540, 80], [580, 420], [90, 420]],
    "dwell_time_ms": 1000,
    "cooldown_ms": 5000
  }
]
```

### 16.2 多事件类型

当前事件类型：

```text
person_intrusion
```

后续可扩展：

```text
person_loitering
person_absence
vehicle_intrusion
line_crossing
```

### 16.3 性能优化

建议优先方向：

```text
1. 使用 RGA 替代 OpenCV CPU 颜色空间转换；
2. 限制 Web MJPEG FPS；
3. 降低第三方库日志输出；
4. 增加 queue_drop_count；
5. 增加 preprocess_time_ms / inference_time_ms / postprocess_time_ms。
```

### 16.4 平台对接

可扩展字段：

```json
{
  "device_id": "rk3568-001",
  "site_id": "factory-a",
  "zone_id": "zone-1",
  "image_url": "http://<board-ip>:8080/snapshot/<event_id>.jpg"
}
```

---

## 17. 当前版本说明

```text
version: 1.0
date: 2026-05-19
project: RK3568 EdgeGuard Person Monitoring
```

当前架构已完成从基础 Demo 到可部署边缘端人员监测系统的升级。