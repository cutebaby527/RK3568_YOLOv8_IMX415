# RK3568 EdgeGuard 性能测试与运行指标文档

> 本文档用于记录 RK3568 + IMX415 + YOLOv8 RKNN 人员监测系统的性能指标、测试方法、MQTT Metrics 格式、Web 指标展示、systemd 运行状态和后续优化方向。  
> 当前项目路径默认如下：

```bash
/userdata/RK3568_Yolo
```

---

## 1. 文档目的

本系统已经完成以下主链路：

```text
IMX415 摄像头采集
→ NV12 视频帧
→ YOLOv8 RKNN 推理
→ person 检测结果
→ ROI / 停留时间 / 冷却时间规则判断
→ MQTT 告警事件
→ 告警截图
→ Web 页面展示
→ Runtime Metrics 发布
```

本文档主要记录：

```text
1. 当前系统性能指标；
2. Metrics MQTT Topic 协议；
3. 如何验证 FPS、告警数和 MQTT 发布数；
4. 如何查看 systemd 运行状态；
5. 当前实测结果；
6. 后续性能优化方向。
```

---

## 2. 当前性能统计能力

当前系统已经实现 Runtime Metrics 模块，并通过 MQTT 周期性发布运行指标。

Metrics Topic：

```text
edge/person/metrics
```

默认发布周期：

```text
5 秒
```

当前已统计字段：

```text
uptime_sec
inference_fps
inference_frame_count
alarm_count
mqtt_detect_publish_count
mqtt_alarm_publish_count
last_alarm_ts
```

---

## 3. Metrics Payload 格式

### 3.1 Topic

```text
edge/person/metrics
```

### 3.2 订阅命令

```bash
mosquitto_sub -h 127.0.0.1 -t "edge/person/metrics" -v
```

### 3.3 Payload 示例

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

### 3.4 字段说明

| 字段                        |    类型 | 单位 | 说明                               |
| --------------------------- | ------: | ---: | ---------------------------------- |
| `uptime_sec`                | integer |   秒 | 主程序运行时长                     |
| `inference_fps`             |  number |  FPS | 最近 5 秒统计周期内的推理帧率      |
| `inference_frame_count`     | integer |   帧 | 累计完成推理的帧数                 |
| `alarm_count`               | integer |   次 | 累计触发告警次数                   |
| `mqtt_detect_publish_count` | integer |   次 | 原始检测结果 MQTT 发布次数         |
| `mqtt_alarm_publish_count`  | integer |   次 | 告警事件 MQTT 发布成功次数         |
| `last_alarm_ts`             | integer | 毫秒 | 最近一次告警时间戳；没有告警时为 0 |

---

## 4. 当前实测结果

### 4.1 测试环境

```text
开发板：RK3568
摄像头：IMX415
视频输入：/dev/video0
输入格式：NV12
采集分辨率：640x480
模型：YOLOv8 RKNN
Web 服务端口：8080
MQTT Broker：127.0.0.1:1883
运行方式：systemd 服务托管
```

### 4.2 实测 Metrics 输出

测试时使用命令：

```bash
mosquitto_sub -h 127.0.0.1 -t "edge/person/metrics" -v
```

实测输出示例：

```text
edge/person/metrics {"uptime_sec":5,"inference_fps":18.79,"inference_frame_count":94,"alarm_count":1,"mqtt_detect_publish_count":94,"mqtt_alarm_publish_count":1,"last_alarm_ts":1779110640405}

edge/person/metrics {"uptime_sec":10,"inference_fps":19.78,"inference_frame_count":193,"alarm_count":3,"mqtt_detect_publish_count":192,"mqtt_alarm_publish_count":3,"last_alarm_ts":1779110646471}

edge/person/metrics {"uptime_sec":15,"inference_fps":19.60,"inference_frame_count":291,"alarm_count":3,"mqtt_detect_publish_count":291,"mqtt_alarm_publish_count":3,"last_alarm_ts":1779110646471}

edge/person/metrics {"uptime_sec":20,"inference_fps":19.38,"inference_frame_count":388,"alarm_count":3,"mqtt_detect_publish_count":388,"mqtt_alarm_publish_count":3,"last_alarm_ts":1779110646471}

edge/person/metrics {"uptime_sec":25,"inference_fps":19.57,"inference_frame_count":486,"alarm_count":3,"mqtt_detect_publish_count":486,"mqtt_alarm_publish_count":3,"last_alarm_ts":1779110646471}
```

### 4.3 推理 FPS 统计

根据上述测试，推理 FPS 如下：

| 运行时长 | inference_fps | 累计推理帧数 |
| -------: | ------------: | -----------: |
|     5 秒 |         18.79 |           94 |
|    10 秒 |         19.78 |          193 |
|    15 秒 |         19.60 |          291 |
|    20 秒 |         19.38 |          388 |
|    25 秒 |         19.57 |          486 |

当前平均 FPS 约为：

```text
19.42 FPS
```

计算方式：

```text
(18.79 + 19.78 + 19.60 + 19.38 + 19.57) / 5 ≈ 19.42
```

### 4.4 当前性能结论

当前系统在 RK3568 开发板上运行 YOLOv8 RKNN 推理时，实测推理性能约为：

```text
18 ~ 20 FPS
```

在当前阶段，该性能已经可以满足基础边缘端实时人员监测和告警验证。

---

## 5. systemd 运行状态记录

### 5.1 主检测服务

服务名称：

```text
edge-person-monitor.service
```

查看状态：

```bash
systemctl status edge-person-monitor --no-pager
```

实测状态：

```text
Active: active (running)
Main PID: edge_ai_pipeline
```

示例：

```text
● edge-person-monitor.service - Edge Person Monitoring Pipeline
     Loaded: loaded (/etc/systemd/system/edge-person-monitor.service; enabled)
     Active: active (running)
   Main PID: 7393 (edge_ai_pipeline)
```

### 5.2 Web 服务

服务名称：

```text
edge-person-web.service
```

查看状态：

```bash
systemctl status edge-person-web --no-pager
```

实测状态：

```text
Active: active (running)
Main PID: web_viewer
```

示例：

```text
● edge-person-web.service - Edge Person Monitoring Web Viewer
     Loaded: loaded (/etc/systemd/system/edge-person-web.service; enabled)
     Active: active (running)
   Main PID: 7486 (web_viewer)
```

### 5.3 Web 等待共享内存机制

`web_viewer` 依赖主程序创建共享内存：

```bash
/dev/shm/edge_ai_frame
```

当前 `scripts/run_web.sh` 已经加入等待逻辑。

正常日志：

```text
[run_web] waiting for shared memory: /dev/shm/edge_ai_frame
[run_web] shared memory ready: /dev/shm/edge_ai_frame
```

---

## 6. 性能验证流程

### 6.1 启动服务

```bash
sudo systemctl restart edge-person-monitor
sleep 3
sudo systemctl restart edge-person-web
```

### 6.2 查看服务状态

```bash
systemctl status edge-person-monitor --no-pager
systemctl status edge-person-web --no-pager
```

预期：

```text
active (running)
```

### 6.3 订阅 Metrics

```bash
mosquitto_sub -h 127.0.0.1 -t "edge/person/metrics" -v
```

预期每 5 秒收到一条：

```json
{
  "uptime_sec": 5,
  "inference_fps": 18.79,
  "inference_frame_count": 94,
  "alarm_count": 1,
  "mqtt_detect_publish_count": 94,
  "mqtt_alarm_publish_count": 1,
  "last_alarm_ts": 1779110640405
}
```

### 6.4 Web 页面验证

浏览器访问：

```text
http://<开发板IP>:8080
```

确认 `System Metrics` 卡片刷新以下内容：

```text
Uptime
Inference FPS
Inference Frames
Alarm Count
MQTT Detect
MQTT Alarm
Last Alarm TS
```

---

## 7. 日志查看命令

### 7.1 主程序实时日志

```bash
journalctl -u edge-person-monitor -f
```

### 7.2 主程序最近日志

```bash
journalctl -u edge-person-monitor -n 100 --no-pager
```

### 7.3 Web 实时日志

```bash
journalctl -u edge-person-web -f
```

### 7.4 Web 最近日志

```bash
journalctl -u edge-person-web -n 100 --no-pager
```

### 7.5 查看完整日志

```bash
journalctl -u edge-person-monitor -n 100 --no-pager -l
journalctl -u edge-person-web -n 100 --no-pager -l
```

---

## 8. 当前指标含义与判断标准

### 8.1 inference_fps

```text
含义：最近统计周期内，YOLOv8 RKNN 推理线程成功处理的帧率。
当前实测：18~20 FPS。
判断：如果低于 10 FPS，需要检查模型大小、CPU 转换耗时、NPU 状态或系统负载。
```

### 8.2 inference_frame_count

```text
含义：主程序启动后累计完成推理的帧数。
判断：该值应持续增长。如果停止增长，说明推理线程可能异常或摄像头采集异常。
```

### 8.3 alarm_count

```text
含义：累计触发的业务告警次数。
判断：人员进入 ROI 并满足停留时间后，该值应增加。
```

### 8.4 mqtt_detect_publish_count

```text
含义：edge/detect 原始检测结果发布次数。
判断：正常运行时该值应随 inference_frame_count 同步增长。
```

### 8.5 mqtt_alarm_publish_count

```text
含义：edge/person/alarm 告警事件发布成功次数。
判断：该值通常应小于或等于 alarm_count。
如果 alarm_count 增加但 mqtt_alarm_publish_count 不增加，需要检查 MQTT 发布失败日志。
```

### 8.6 last_alarm_ts

```text
含义：最近一次告警的 Unix 毫秒时间戳。
判断：没有告警时为 0；触发告警后更新。
```

---

## 9. 性能异常排查

### 9.1 inference_fps 明显降低

检查系统负载：

```bash
top
htop
```

检查主程序日志：

```bash
journalctl -u edge-person-monitor -n 100 --no-pager
```

检查是否有大量截图：

```bash
ls -lh /userdata/RK3568_Yolo/events
```

可能原因：

```text
1. CPU NV12 -> RGB888 转换耗时较高；
2. Web 多客户端访问导致 JPEG 编码压力增加；
3. 告警频繁触发，截图保存过多；
4. 模型较大；
5. 系统温度过高导致降频；
6. 其他进程占用 CPU 或内存。
```

### 9.2 inference_frame_count 不增长

检查摄像头：

```bash
v4l2-ctl --list-devices
v4l2-ctl -d /dev/video0 --all
```

检查主程序服务：

```bash
systemctl status edge-person-monitor --no-pager
journalctl -u edge-person-monitor -n 100 --no-pager
```

可能原因：

```text
1. 摄像头节点不存在；
2. 摄像头被其他程序占用；
3. 推理线程异常退出；
4. RKNN 模型加载失败；
5. 共享内存异常。
```

### 9.3 mqtt_detect_publish_count 不增长

检查 MQTT：

```bash
systemctl status mosquitto --no-pager
mosquitto_sub -h 127.0.0.1 -t "#" -v
```

检查主程序日志：

```bash
journalctl -u edge-person-monitor -n 100 --no-pager | grep MQTT
```

可能原因：

```text
1. Mosquitto 未启动；
2. MQTT 连接失败；
3. alarm_thread 未收到 InferResult；
4. 推理结果队列异常。
```

### 9.4 alarm_count 不增长

监听原始检测：

```bash
mosquitto_sub -h 127.0.0.1 -t "edge/detect" -v
```

检查配置：

```bash
cat /userdata/RK3568_Yolo/config/config.json
```

常见原因：

```text
1. person 置信度低于 conf_threshold；
2. bbox 中心点没有进入 ROI；
3. dwell_time_ms 未达到；
4. cooldown_ms 冷却中；
5. 模型误识别人员类别。
```

### 9.5 mqtt_alarm_publish_count 小于 alarm_count

检查主程序日志：

```bash
journalctl -u edge-person-monitor -n 100 --no-pager | grep "alarm publish"
```

可能原因：

```text
1. MQTT Broker 短暂不可用；
2. MQTT publish 返回错误；
3. 网络或本机 broker 异常。
```

---

## 10. 当前性能瓶颈分析

当前实现中，主要可能的性能瓶颈包括：

### 10.1 CPU 图像格式转换

当前推理线程中使用 OpenCV CPU 做：

```text
NV12 -> RGB888
```

该步骤会占用 CPU。

后续优化方向：

```text
使用 RGA 做硬件颜色空间转换和 resize。
```

### 10.2 Web JPEG 编码

Web Viewer 每帧将 NV12 转 BGR 后再编码 JPEG：

```text
NV12 -> BGR -> JPEG
```

如果多个浏览器客户端同时访问，CPU 压力会增加。

后续优化方向：

```text
1. 限制 MJPEG 输出 FPS；
2. 复用编码结果；
3. 降低 JPEG 质量；
4. 控制 Web 客户端数量。
```

### 10.3 告警截图保存

告警触发时会：

```text
读取共享内存
NV12 转 BGR
绘制 ROI / bbox / 文本
保存 JPEG
```

如果告警过于频繁，会增加 CPU 和存储压力。

后续优化方向：

```text
1. 合理设置 cooldown_ms；
2. 限制 events 目录最大文件数；
3. 定期清理旧截图。
```

### 10.4 日志输出过多

当前 RKNN / RGA / 后处理库会输出较多日志，例如：

```text
rknn_run
fill dst image
scale=...
```

后续优化方向：

```text
1. 降低第三方库日志等级；
2. 将调试日志改为可配置开关；
3. systemd 中限制日志增长。
```

---

## 11. 后续建议增加的指标

当前 Metrics 已具备基础运行指标。后续可扩展：

```text
capture_fps
preprocess_time_ms
inference_time_ms
postprocess_time_ms
snapshot_save_count
snapshot_save_time_ms
dropped_frame_count
queue_drop_count
mqtt_publish_latency_ms
cpu_usage
memory_usage
temperature
web_client_count
```

建议优先级：

| 优先级 | 指标                         | 原因                    |
| ------ | ---------------------------- | ----------------------- |
| 高     | `inference_time_ms`          | 判断 NPU 推理耗时       |
| 高     | `preprocess_time_ms`         | 判断 CPU/RGA 预处理耗时 |
| 高     | `queue_drop_count`           | 判断队列是否丢帧        |
| 中     | `snapshot_save_time_ms`      | 判断截图是否影响实时性  |
| 中     | `web_client_count`           | 判断 Web 访问压力       |
| 低     | `cpu_usage` / `memory_usage` | 需要额外读取系统状态    |

---

## 12. 性能测试记录模板

后续每次优化后，可以按下面模板记录。

```text
测试日期：
开发板型号：
系统版本：
模型版本：
输入分辨率：
运行方式：
测试时长：
Web 客户端数量：
是否开启检测框：
是否开启 ROI：
是否触发告警：
平均 inference_fps：
最低 inference_fps：
最高 inference_fps：
alarm_count：
mqtt_detect_publish_count：
mqtt_alarm_publish_count：
CPU 负载：
内存占用：
温度：
备注：
```

示例：

```text
测试日期：2026-05-19
开发板型号：RK3568
系统版本：Debian 11 aarch64
模型版本：yolov8.rknn
输入分辨率：640x480 NV12
运行方式：systemd
测试时长：25 秒
Web 客户端数量：1
是否开启检测框：是
是否开启 ROI：是
是否触发告警：是
平均 inference_fps：约 19.42 FPS
最低 inference_fps：18.79 FPS
最高 inference_fps：19.78 FPS
alarm_count：3
mqtt_detect_publish_count：486
mqtt_alarm_publish_count：3
备注：系统运行稳定，Web 和 MQTT 均正常。
```

---

## 13. 当前性能结论

当前系统在 RK3568 开发板上的实测结果：

```text
推理 FPS：约 18~20 FPS
平均 FPS：约 19.42 FPS
MQTT Metrics 周期：5 秒
主服务状态：active (running)
Web 服务状态：active (running)
Web 指标展示：正常
告警截图：正常
ROI 显示：正常
```

当前性能满足项目阶段目标：

```text
1. 实时视频预览；
2. 实时人员检测；
3. ROI 入侵告警；
4. MQTT 告警上报；
5. Web 可视化；
6. systemd 长期运行。
```

---

## 14. 当前版本

```text
version: 1.0
date: 2026-05-19
project: RK3568 EdgeGuard Person Monitoring
```