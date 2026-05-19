# RK3568 YOLOv8 IMX415 智能检测项目

## 项目简介

本项目基于 RK3568 开发板、ATK-MCIMX415 摄像头和 YOLOv8 RKNN 模型，实现摄像头实时采集、NPU 推理、Web 实时显示检测框和 MQTT 检测结果发布。

## 硬件环境

- 开发板：ATK-DLRK3568 / RK3568
- 摄像头：ATK-MCIMX415 V1.5
- 系统：Debian 11 aarch64
- NPU：Rockchip RKNN Runtime

## 软件链路

```text
IMX415 Camera
→ /dev/video0
→ 640x480 NV12
→ YOLOv8 RKNN
→ shared memory
→ web_viewer
→ browser
→ MQTT
# RK3568 EdgeGuard Person Monitoring System

基于 RK3568 + IMX415 + YOLOv8 RKNN 的边缘端人员监测与告警系统。

本项目从基础 YOLOv8 RKNN 摄像头 Demo 升级为完整的边缘端人员检测、ROI 规则判断、MQTT 告警、截图留证、Web 可视化和 systemd 服务化部署系统。

---

## 1. 功能特性

当前系统支持：

- IMX415 摄像头实时采集；
- YOLOv8 RKNN NPU 推理；
- person 目标检测；
- Web 实时视频流显示；
- Web 检测框显示；
- Web ROI 区域叠加显示；
- ROI 入侵判断；
- 停留时间 dwell time 判断；
- 告警冷却 cooldown 控制；
- MQTT 原始检测结果发布；
- MQTT 结构化告警事件发布；
- MQTT Runtime Metrics 发布；
- 告警截图留证；
- Web 告警事件列表；
- Web 最近截图显示；
- Web System Metrics 显示；
- systemd 服务化部署；
- 开机自启动。

---

## 2. 项目路径

默认部署路径：

```bash
/userdata/RK3568_Yolo
```

如果部署到其他目录，需要同步修改：

```text
scripts/run_pipeline.sh
scripts/run_web.sh
service/edge-person-monitor.service
service/edge-person-web.service
```

---

## 3. 目录结构

```text
.
├── CMakeLists.txt
├── build.sh
├── config/
│   └── config.json
├── docs/
│   ├── architecture.md
│   ├── deployment.md
│   ├── mqtt_protocol.md
│   └── performance.md
├── events/
├── include/
├── model/
│   ├── yolov8.rknn
│   └── coco_80_labels_list.txt
├── scripts/
│   ├── install_service.sh
│   ├── run_pipeline.sh
│   └── run_web.sh
├── service/
│   ├── edge-person-monitor.service
│   └── edge-person-web.service
├── src/
└── third_party/
```

---

## 4. 编译

```bash
cd /userdata/RK3568_Yolo
./build.sh
```

编译成功后生成：

```text
install/edge_ai_pipeline
install/web_viewer
```

---

## 5. 手动运行

### 5.1 启动 MQTT Broker

```bash
sudo systemctl restart mosquitto
systemctl status mosquitto --no-pager
```

### 5.2 启动主检测程序

```bash
cd /userdata/RK3568_Yolo/install
export LD_LIBRARY_PATH=$PWD/lib:$LD_LIBRARY_PATH

./edge_ai_pipeline --config ../config/config.json
```

### 5.3 启动 Web Viewer

另开一个终端：

```bash
cd /userdata/RK3568_Yolo/install
export LD_LIBRARY_PATH=$PWD/lib:$LD_LIBRARY_PATH

./web_viewer   127.0.0.1   1883   edge/detect   8080   edge/person/alarm   edge/person/metrics   ../config/config.json
```

### 5.4 访问 Web 页面

```text
http://<开发板IP>:8080
```

---

## 6. systemd 部署

安装服务：

```bash
cd /userdata/RK3568_Yolo
./scripts/install_service.sh
```

启动服务：

```bash
sudo systemctl restart edge-person-monitor
sudo systemctl restart edge-person-web
```

查看状态：

```bash
systemctl status edge-person-monitor --no-pager
systemctl status edge-person-web --no-pager
```

查看日志：

```bash
journalctl -u edge-person-monitor -f
journalctl -u edge-person-web -f
```

---

## 7. MQTT Topics

| Topic                 | 说明               |
| --------------------- | ------------------ |
| `edge/detect`         | 原始检测结果       |
| `edge/person/alarm`   | 结构化人员入侵告警 |
| `edge/person/metrics` | 系统运行指标       |

监听全部消息：

```bash
mosquitto_sub -h 127.0.0.1 -t "#" -v
```

监听告警：

```bash
mosquitto_sub -h 127.0.0.1 -t "edge/person/alarm" -v
```

监听指标：

```bash
mosquitto_sub -h 127.0.0.1 -t "edge/person/metrics" -v
```

---

## 8. 配置文件

主配置文件：

```bash
config/config.json
```

核心配置示例：

```json
{
  "model": {
    "path": "../model/yolov8.rknn",
    "conf_threshold": 0.45,
    "nms_threshold": 0.45
  },
  "rule": {
    "target_class": "person",
    "roi": [[120, 80], [540, 80], [580, 420], [90, 420]],
    "dwell_time_ms": 1000,
    "cooldown_ms": 5000
  },
  "mqtt": {
    "topic_detect": "edge/detect",
    "topic_alarm": "edge/person/alarm",
    "topic_metrics": "edge/person/metrics"
  },
  "snapshot": {
    "enable": true,
    "save_dir": "../events"
  }
}
```

修改配置后重启服务：

```bash
sudo systemctl restart edge-person-monitor
sudo systemctl restart edge-person-web
```

---

## 9. 告警截图

截图保存目录：

```bash
events/
```

Web 访问：

```text
http://<开发板IP>:8080/snapshot/<event_id>.jpg
```

---

## 10. 性能指标

当前实测推理性能约：

```text
18 ~ 20 FPS
```

Metrics 示例：

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

## 11. 文档索引

| 文档                    | 内容                                            |
| ----------------------- | ----------------------------------------------- |
| `docs/architecture.md`  | 系统架构、线程模型、数据流、共享内存、Web、MQTT |
| `docs/deployment.md`    | 编译、运行、systemd 部署、常见问题              |
| `docs/mqtt_protocol.md` | MQTT topic、payload、字段说明、调试命令         |
| `docs/performance.md`   | 性能指标、FPS、systemd 状态、优化方向           |

---

## 12. 常用命令

### 编译

```bash
./build.sh
```

### 启动服务

```bash
sudo systemctl restart edge-person-monitor
sudo systemctl restart edge-person-web
```

### 停止服务

```bash
sudo systemctl stop edge-person-web
sudo systemctl stop edge-person-monitor
```

### 查看服务

```bash
systemctl status edge-person-monitor --no-pager
systemctl status edge-person-web --no-pager
```

### 查看日志

```bash
journalctl -u edge-person-monitor -f
journalctl -u edge-person-web -f
```

### 查看 MQTT

```bash
mosquitto_sub -h 127.0.0.1 -t "#" -v
```

---

## 13. 当前版本状态

当前系统已经完成：

```text
摄像头采集
YOLOv8 RKNN 推理
人员检测
ROI 规则
告警冷却
MQTT 告警
截图留证
Web 可视化
Runtime Metrics
systemd 服务部署
```

项目已从原始 YOLOv8 Demo 升级为可部署的边缘端人员监测系统。
