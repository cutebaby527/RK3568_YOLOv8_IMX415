# RK3568 EdgeGuard 人员监测系统部署文档

> 本文档用于记录 RK3568 + IMX415 + YOLOv8 RKNN 项目的编译、运行、服务部署、Web 访问、MQTT 验证和常见问题排查流程。  
> 当前项目路径默认如下，如部署路径不同，需要同步修改脚本和 systemd 服务文件中的路径。

```bash
/userdata/RK3568_Yolo
```

---

## 1. 系统功能概述

当前项目已经从基础 YOLOv8 Demo 升级为边缘端人员监测与告警系统，主要功能包括：

1. IMX415 摄像头采集实时视频；
2. YOLOv8 RKNN 模型进行人员检测；
3. Web 页面显示实时视频流；
4. Web 页面支持检测框显示；
5. Web 页面支持 ROI 区域叠加显示；
6. 支持 ROI 入侵规则判断；
7. 支持人员停留时间判断；
8. 支持告警冷却时间控制；
9. 支持 MQTT 原始检测结果发布；
10. 支持 MQTT 结构化告警事件发布；
11. 支持 MQTT 系统运行指标发布；
12. 支持告警截图留证；
13. 支持 Web 页面展示告警事件、最近截图和系统指标；
14. 支持 systemd 服务化部署和开机自启动。

---

## 2. 项目目录结构

当前核心目录如下：

```text
/userdata/RK3568_Yolo
├── build/                         # CMake 构建目录，由 build.sh 自动生成
├── install/                       # 安装运行目录，由 build.sh 自动生成
├── config/
│   └── config.json                # 系统配置文件
├── docs/
│   └── deployment.md              # 当前部署文档
├── events/                        # 告警截图保存目录
├── include/                       # 头文件目录
├── model/
│   ├── yolov8.rknn                # RKNN 模型
│   └── coco_80_labels_list.txt    # COCO 类别标签
├── scripts/
│   ├── run_pipeline.sh            # 主检测程序启动脚本
│   ├── run_web.sh                 # Web 服务启动脚本
│   └── install_service.sh         # systemd 服务安装脚本
├── service/
│   ├── edge-person-monitor.service
│   └── edge-person-web.service
├── src/                           # 源码目录
├── third_party/                   # RKNN、RGA、YOLOv8 后处理等第三方依赖
├── build.sh                       # 编译安装脚本
└── CMakeLists.txt                 # CMake 构建配置
```

---

## 3. 运行环境要求

### 3.1 硬件环境

```text
开发板：RK3568
摄像头：IMX415
NPU：Rockchip RKNN Runtime
视频输入：/dev/video0
默认分辨率：640x480
默认格式：NV12
```

### 3.2 软件环境

建议开发板系统中安装以下依赖：

```bash
sudo apt update

sudo apt install -y \
  git \
  cmake \
  make \
  g++ \
  gdb \
  vim \
  v4l-utils \
  net-tools \
  mosquitto \
  mosquitto-clients \
  libmosquitto-dev \
  libopencv-dev
```

### 3.3 MQTT Broker

本项目默认使用本机 Mosquitto：

```text
host: 127.0.0.1
port: 1883
```

启动并设置开机自启：

```bash
sudo systemctl enable mosquitto
sudo systemctl restart mosquitto
systemctl status mosquitto --no-pager
```

---

## 4. 配置文件说明

主配置文件：

```bash
/userdata/RK3568_Yolo/config/config.json
```

典型配置如下：

```json
{
  "camera": {
    "device": "/dev/video0",
    "width": 640,
    "height": 480,
    "format": "NV12"
  },
  "model": {
    "path": "../model/yolov8.rknn",
    "input_width": 640,
    "input_height": 640,
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
    "host": "127.0.0.1",
    "port": 1883,
    "topic_detect": "edge/detect",
    "topic_alarm": "edge/person/alarm",
    "topic_status": "edge/person/status",
    "topic_metrics": "edge/person/metrics"
  },
  "web": {
    "port": 8080
  },
  "snapshot": {
    "enable": true,
    "save_dir": "../events"
  }
}
```

### 4.1 配置字段说明

#### camera

```json
"camera": {
  "device": "/dev/video0",
  "width": 640,
  "height": 480,
  "format": "NV12"
}
```

中文说明：

```text
device：摄像头设备节点。
width：采集宽度。
height：采集高度。
format：采集格式，当前主流程使用 NV12。
```

#### model

```json
"model": {
  "path": "../model/yolov8.rknn",
  "conf_threshold": 0.45,
  "nms_threshold": 0.45
}
```

中文说明：

```text
path：模型路径。注意该路径是相对于 install/ 运行目录。
conf_threshold：人员检测置信度阈值。
nms_threshold：NMS 阈值。
```

如果现场人员检测置信度偏低，可以临时调低：

```json
"conf_threshold": 0.30
```

但正式部署时建议根据现场测试结果设定。

#### rule

```json
"rule": {
  "target_class": "person",
  "roi": [[120, 80], [540, 80], [580, 420], [90, 420]],
  "dwell_time_ms": 1000,
  "cooldown_ms": 5000
}
```

中文说明：

```text
target_class：目标类别，当前为 person。
roi：检测区域，多边形坐标，坐标系为 640x480 图像坐标。
dwell_time_ms：人员进入 ROI 后持续停留多久才触发告警。
cooldown_ms：告警冷却时间，避免持续刷屏告警。
```

调试时可使用全画面 ROI：

```json
"roi": [[10, 10], [630, 10], [630, 470], [10, 470]]
```

#### mqtt

```json
"mqtt": {
  "host": "127.0.0.1",
  "port": 1883,
  "topic_detect": "edge/detect",
  "topic_alarm": "edge/person/alarm",
  "topic_metrics": "edge/person/metrics"
}
```

中文说明：

```text
topic_detect：原始检测结果 topic。
topic_alarm：结构化告警事件 topic。
topic_metrics：系统运行指标 topic。
```

#### snapshot

```json
"snapshot": {
  "enable": true,
  "save_dir": "../events"
}
```

中文说明：

```text
enable：是否启用告警截图。
save_dir：截图保存目录。注意该路径相对于 install/ 运行目录。
```

当前实际保存目录为：

```bash
/userdata/RK3568_Yolo/events
```

---

## 5. 编译项目

进入项目根目录：

```bash
cd /userdata/RK3568_Yolo
```

执行编译：

```bash
./build.sh
```

编译成功后输出目录：

```bash
/userdata/RK3568_Yolo/install
```

安装目录中应包含：

```text
edge_ai_pipeline
web_viewer
lib/
model/
```

确认可执行文件：

```bash
ls -lh /userdata/RK3568_Yolo/install
```

---

## 6. 手动运行方式

手动运行适合调试阶段使用。

### 6.1 启动 MQTT

```bash
sudo systemctl restart mosquitto
systemctl status mosquitto --no-pager
```

### 6.2 启动主检测程序

```bash
cd /userdata/RK3568_Yolo/install

export LD_LIBRARY_PATH=$PWD/lib:$LD_LIBRARY_PATH

./edge_ai_pipeline --config ../config/config.json
```

正常日志中应看到：

```text
[main] config path : ../config/config.json
[main] detect topic: edge/detect
[main] alarm topic : edge/person/alarm
[main] metrics topic: edge/person/metrics
[inference] YOLOv8 RKNN mode
[inference] YOLOv8 model initialized
[alarm] MQTT connected
```

### 6.3 启动 Web 服务

另开一个终端：

```bash
cd /userdata/RK3568_Yolo/install

export LD_LIBRARY_PATH=$PWD/lib:$LD_LIBRARY_PATH

./web_viewer \
  127.0.0.1 \
  1883 \
  edge/detect \
  8080 \
  edge/person/alarm \
  edge/person/metrics \
  ../config/config.json
```

正常日志中应看到：

```text
[web] config loaded: ../config/config.json roi_points=4
[web] ROI overlay: ON points=4
[web] MQTT connected: 127.0.0.1:1883 detect_topic=edge/detect alarm_topic=edge/person/alarm metrics_topic=edge/person/metrics
[web] HTTP: http://0.0.0.0:8080
```

### 6.4 访问 Web 页面

在电脑浏览器中访问：

```text
http://<开发板IP>:8080
```

页面应显示：

```text
1. 实时视频流
2. ROI 黄色多边形
3. Toggle Boxes 按钮
4. 检测框
5. Alarm Events
6. Latest Snapshot
7. System Metrics
8. Raw MQTT Events
```

---

## 7. 使用运行脚本启动

项目已经封装两个运行脚本。

### 7.1 启动主程序

```bash
cd /userdata/RK3568_Yolo
./scripts/run_pipeline.sh
```

### 7.2 启动 Web

```bash
cd /userdata/RK3568_Yolo
./scripts/run_web.sh
```

`run_web.sh` 会等待共享内存创建：

```bash
/dev/shm/edge_ai_frame
```

正常日志：

```text
[run_web] waiting for shared memory: /dev/shm/edge_ai_frame
[run_web] shared memory ready: /dev/shm/edge_ai_frame
```

中文说明：

```text
web_viewer 依赖 edge_ai_pipeline 创建共享内存。
如果 web_viewer 先启动，会因为找不到 /dev/shm/edge_ai_frame 失败。
因此 run_web.sh 中增加了等待逻辑。
```

---

## 8. systemd 服务部署

### 8.1 服务文件

主程序服务：

```bash
/userdata/RK3568_Yolo/service/edge-person-monitor.service
```

Web 服务：

```bash
/userdata/RK3568_Yolo/service/edge-person-web.service
```

### 8.2 安装服务

```bash
cd /userdata/RK3568_Yolo

./scripts/install_service.sh
```

该脚本会执行：

```text
1. 复制 service 文件到 /etc/systemd/system/
2. 执行 systemctl daemon-reload
3. enable edge-person-monitor.service
4. enable edge-person-web.service
```

### 8.3 启动服务

```bash
sudo systemctl restart edge-person-monitor
sudo systemctl restart edge-person-web
```

建议先启动主程序，再启动 Web：

```bash
sudo systemctl restart edge-person-monitor
sleep 3
sudo systemctl restart edge-person-web
```

### 8.4 查看服务状态

```bash
systemctl status edge-person-monitor --no-pager
systemctl status edge-person-web --no-pager
```

正常状态：

```text
Active: active (running)
```

### 8.5 停止服务

```bash
sudo systemctl stop edge-person-web
sudo systemctl stop edge-person-monitor
```

### 8.6 设置开机自启

```bash
sudo systemctl enable edge-person-monitor
sudo systemctl enable edge-person-web
```

### 8.7 取消开机自启

```bash
sudo systemctl disable edge-person-monitor
sudo systemctl disable edge-person-web
```

---

## 9. 日志查看

### 9.1 主程序实时日志

```bash
journalctl -u edge-person-monitor -f
```

### 9.2 主程序最近日志

```bash
journalctl -u edge-person-monitor -n 100 --no-pager
```

### 9.3 Web 实时日志

```bash
journalctl -u edge-person-web -f
```

### 9.4 Web 最近日志

```bash
journalctl -u edge-person-web -n 100 --no-pager
```

### 9.5 查看完整不截断日志

```bash
journalctl -u edge-person-monitor -n 100 --no-pager -l
journalctl -u edge-person-web -n 100 --no-pager -l
```

中文说明：

```text
systemctl status 中部分日志可能会被省略。
如果看到 Hint: Some lines were ellipsized，可以加 -l 查看完整日志。
```

---

## 10. MQTT 协议验证

### 10.1 原始检测结果 Topic

Topic：

```text
edge/detect
```

订阅：

```bash
mosquitto_sub -h 127.0.0.1 -t "edge/detect" -v
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

中文说明：

```text
该 topic 用于调试检测结果和 Web 原始事件显示。
它会持续发布检测结果。
```

### 10.2 告警事件 Topic

Topic：

```text
edge/person/alarm
```

订阅：

```bash
mosquitto_sub -h 127.0.0.1 -t "edge/person/alarm" -v
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

中文说明：

```text
只有满足以下条件时才会发布告警：
1. 检测类别为 person；
2. 置信度大于等于 conf_threshold；
3. bbox 中心点位于 ROI 内；
4. 停留时间超过 dwell_time_ms；
5. 当前不在 cooldown_ms 冷却周期内。
```

### 10.3 运行指标 Topic

Topic：

```text
edge/person/metrics
```

订阅：

```bash
mosquitto_sub -h 127.0.0.1 -t "edge/person/metrics" -v
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

中文说明：

```text
该 topic 默认每 5 秒发布一次。
Web 页面中的 System Metrics 卡片会显示该 topic 的内容。
```

---

## 11. 告警截图

### 11.1 截图保存目录

```bash
/userdata/RK3568_Yolo/events
```

查看截图：

```bash
ls -lh /userdata/RK3568_Yolo/events
```

### 11.2 截图命名规则

```text
<event_id>.jpg
```

示例：

```text
20260518-200221-0001.jpg
```

### 11.3 Web 访问截图

```text
http://<开发板IP>:8080/snapshot/<event_id>.jpg
```

示例：

```text
http://192.168.1.100:8080/snapshot/20260518-200221-0001.jpg
```

中文说明：

```text
Web 页面 Latest Snapshot 会显示最近一次告警截图。
Alarm Events 中也会给出 snapshot 链接。
```

---

## 12. 摄像头检查

查看摄像头设备：

```bash
v4l2-ctl --list-devices
```

查看 `/dev/video0` 参数：

```bash
v4l2-ctl -d /dev/video0 --all
```

查看支持格式：

```bash
v4l2-ctl -d /dev/video0 --list-formats-ext
```

当前项目默认使用：

```text
/dev/video0
640x480
NV12
```

---

## 13. 常见问题排查

### 13.1 Web 服务报 shm_open: No such file or directory

现象：

```text
[web] shm_open: No such file or directory
[web] make sure edge_ai_pipeline is running before web_viewer
```

原因：

```text
web_viewer 启动时，edge_ai_pipeline 还没有创建共享内存 /dev/shm/edge_ai_frame。
```

检查：

```bash
ls -lh /dev/shm/edge_ai_frame
```

修复：

```bash
sudo systemctl restart edge-person-monitor
sleep 3
sudo systemctl restart edge-person-web
```

当前 `scripts/run_web.sh` 已经加入等待逻辑，正常情况下会自动等待共享内存出现。

---

### 13.2 Web 页面打不开

检查 Web 服务：

```bash
systemctl status edge-person-web --no-pager
journalctl -u edge-person-web -n 100 --no-pager
```

检查 8080 端口：

```bash
sudo ss -tunlp | grep 8080
```

重启 Web：

```bash
sudo systemctl restart edge-person-web
```

---

### 13.3 Web 页面是旧版本

浏览器强制刷新：

```text
Ctrl + F5
```

或访问：

```text
http://<开发板IP>:8080/?t=1
```

重新编译并重启服务：

```bash
cd /userdata/RK3568_Yolo
./build.sh

sudo systemctl restart edge-person-monitor
sudo systemctl restart edge-person-web
```

---

### 13.4 没有检测框

检查 Web 页面是否开启：

```text
Toggle Boxes
```

检查检测结果 topic：

```bash
mosquitto_sub -h 127.0.0.1 -t "edge/detect" -v
```

检查主程序日志：

```bash
journalctl -u edge-person-monitor -n 100 --no-pager
```

---

### 13.5 没有告警

检查告警 topic：

```bash
mosquitto_sub -h 127.0.0.1 -t "edge/person/alarm" -v
```

常见原因：

```text
1. person 置信度低于 conf_threshold；
2. bbox 中心点不在 ROI 内；
3. 停留时间没有超过 dwell_time_ms；
4. 当前处于 cooldown_ms 冷却时间；
5. 模型将人员误识别为其他类别；
6. ROI 设置过小或位置不准确。
```

调试建议：

```json
"conf_threshold": 0.30,
"roi": [[10, 10], [630, 10], [630, 470], [10, 470]],
"dwell_time_ms": 300,
"cooldown_ms": 3000
```

修改配置后重启：

```bash
sudo systemctl restart edge-person-monitor
sudo systemctl restart edge-person-web
```

---

### 13.6 没有新截图

检查是否真的触发了告警：

```bash
journalctl -u edge-person-monitor -n 100 --no-pager | grep alarm
```

检查截图日志：

```bash
journalctl -u edge-person-monitor -n 100 --no-pager | grep snapshot
```

检查目录：

```bash
ls -lh /userdata/RK3568_Yolo/events
```

如果没有告警，就不会有新截图。

---

### 13.7 Metrics 不刷新

检查 metrics topic：

```bash
mosquitto_sub -h 127.0.0.1 -t "edge/person/metrics" -v
```

检查主程序日志：

```bash
journalctl -u edge-person-monitor -n 100 --no-pager | grep metrics
```

检查 Web 是否订阅 metrics topic：

```bash
journalctl -u edge-person-web -n 100 --no-pager | grep metrics
```

正常日志应包含：

```text
metrics_topic=edge/person/metrics
```

---

### 13.8 systemd 服务一直重启

查看服务状态：

```bash
systemctl status edge-person-monitor --no-pager -l
systemctl status edge-person-web --no-pager -l
```

查看日志：

```bash
journalctl -u edge-person-monitor -n 200 --no-pager -l
journalctl -u edge-person-web -n 200 --no-pager -l
```

常见原因：

```text
1. 模型路径错误；
2. RKNN Runtime 动态库路径错误；
3. 摄像头 /dev/video0 不存在；
4. MQTT Broker 未启动；
5. Web 启动时共享内存未创建；
6. 端口 8080 被占用。
```

---

## 14. 开机自启动验证

重启开发板：

```bash
sudo reboot
```

重新 SSH 登录后检查：

```bash
systemctl status edge-person-monitor --no-pager
systemctl status edge-person-web --no-pager
```

确认两个服务均为：

```text
active (running)
```

浏览器访问：

```text
http://<开发板IP>:8080
```

确认页面功能：

```text
1. 视频流正常；
2. ROI 显示正常；
3. 检测框显示正常；
4. 告警列表正常；
5. 最近截图正常；
6. System Metrics 正常刷新。
```

---

## 15. Git 提交流程建议

每次修改后先检查：

```bash
git status
git diff --stat
```

编译：

```bash
./build.sh
```

验证运行后提交：

```bash
git add <modified-files>
git commit -m "type: message"
```

查看提交链：

```bash
git log --oneline --decorate -15
```

当前升级过程建议保持每一步一个 commit，便于定位问题和回滚。

---

## 16. 回滚方式

查看提交：

```bash
git log --oneline --decorate
```

回滚到基线：

```bash
git reset --hard baseline-before-edgeguard
```

回滚最近一次提交：

```bash
git reset --hard HEAD~1
```

仅撤销某个文件的未提交修改：

```bash
git restore <file>
```

---

## 17. 当前服务命令速查

### 编译

```bash
cd /userdata/RK3568_Yolo
./build.sh
```

### 手动运行主程序

```bash
cd /userdata/RK3568_Yolo/install
export LD_LIBRARY_PATH=$PWD/lib:$LD_LIBRARY_PATH
./edge_ai_pipeline --config ../config/config.json
```

### 手动运行 Web

```bash
cd /userdata/RK3568_Yolo/install
export LD_LIBRARY_PATH=$PWD/lib:$LD_LIBRARY_PATH
./web_viewer 127.0.0.1 1883 edge/detect 8080 edge/person/alarm edge/person/metrics ../config/config.json
```

### 脚本运行

```bash
cd /userdata/RK3568_Yolo
./scripts/run_pipeline.sh
./scripts/run_web.sh
```

### systemd 启动

```bash
sudo systemctl restart edge-person-monitor
sudo systemctl restart edge-person-web
```

### systemd 停止

```bash
sudo systemctl stop edge-person-web
sudo systemctl stop edge-person-monitor
```

### 查看状态

```bash
systemctl status edge-person-monitor --no-pager
systemctl status edge-person-web --no-pager
```

### 查看日志

```bash
journalctl -u edge-person-monitor -f
journalctl -u edge-person-web -f
```

### Web 地址

```text
http://<开发板IP>:8080
```

### MQTT 验证

```bash
mosquitto_sub -h 127.0.0.1 -t "edge/detect" -v
mosquitto_sub -h 127.0.0.1 -t "edge/person/alarm" -v
mosquitto_sub -h 127.0.0.1 -t "edge/person/metrics" -v
```

---

## 18. 当前部署状态记录

最后一次确认状态：

```bash
systemctl status edge-person-monitor --no-pager
systemctl status edge-person-web --no-pager
```

已验证：

```text
edge-person-monitor.service: active (running)
edge-person-web.service: active (running)
```

Web 服务已验证等待共享内存：

```text
[run_web] waiting for shared memory: /dev/shm/edge_ai_frame
[run_web] shared memory ready: /dev/shm/edge_ai_frame
```

系统已完成从手动 Demo 到可部署边缘人员监测服务的升级。